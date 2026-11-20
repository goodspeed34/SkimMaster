#include "report/reporting.h"

#include <httplib.h>
#include <openssl/evp.h>
#include <sqlite3.h>

#include <boost/asio.hpp>
#include <boost/interprocess/sync/file_lock.hpp>
#include <chrono>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <nlohmann/json.hpp>
#include <random>
#include <regex>
#include <sstream>
#include <thread>

#include "core/clock_health.h"
#include "core/schema.h"

namespace skim {
namespace {
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
double Epoch() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
             .count() /
         1000.;
}

std::string ShortMd5(const std::string& text) {
  unsigned char digest[EVP_MAX_MD_SIZE];
  unsigned count = 0;
  if (!EVP_Digest(text.data(), text.size(), digest, &count, EVP_md5(),
                  nullptr) ||
      count < 4)
    throw std::runtime_error("MD5 unavailable for rx/6 checksum");
  std::ostringstream result;
  for (int i = 0; i < 4; ++i)
    result << std::hex << std::setw(2) << std::setfill('0')
           << static_cast<int>(digest[i]);
  return result.str();
}
struct SmReception {
  int64_t id, hz, utc;
  std::string call, grid, mode, message, time, snr, speed, date, power;
  int64_t dial;
  double dt;
};
class SmOutbox {
 public:
  explicit SmOutbox(const char* path) {
    sqlite3* handle = nullptr;
    int rc = sqlite3_open(path, &handle);
    db_.reset(handle);
    Check(rc);
    Check(sqlite3_busy_timeout(handle, 5000));
    MigrateStationSchema(handle);
  }
  std::vector<SmReception> Pending(const std::string& feed, int max_age = 180) {
    auto q = Prepare(
        "SELECT "
        "s.rowid,frequency_hz,utc_ms,callsign,coalesce(grid,''),mode,message,"
        "strftime('%H%MZ',utc_ms/1000,'unixepoch'),coalesce(cast(round(l.snr) "
        "as integer),''),"
        "coalesce(cast(round(l.speed) as "
        "integer),''),substr(strftime('%Y%m%d',utc_ms/"
        "1000,'unixepoch'),3),coalesce(w.power,''),w.dial,w.dt FROM "
        "live_receptions l "
        "JOIN spots s ON s.rowid=l.spot_id LEFT JOIN wspr_details w ON "
        "w.spot_id=s.rowid "
        "WHERE utc_ms BETWEEN (unixepoch()-?)*1000 AND (unixepoch()+1)*1000 "
        "AND length(callsign) BETWEEN 2 AND 16 "
        "AND frequency_hz BETWEEN 1 AND 1000000000000 "
        "AND NOT EXISTS(SELECT 1 FROM report_receipts WHERE feed=? AND "
        "report_receipts.spot_id=s.rowid) ORDER BY s.rowid");
    Check(sqlite3_bind_int(q.get(), 1, max_age));
    Bind(q.get(), 2, feed);
    std::vector<SmReception> rows;
    int rc;
    auto str = [&](int col) {
      return std::string(
          reinterpret_cast<const char*>(sqlite3_column_text(q.get(), col)));
    };
    static const std::regex call_pattern("[A-Z0-9/-]{2,16}");
    static const std::regex grid_pattern("[A-Ra-r]{2}[0-9]{2}([A-Xa-x]{2})?");
    while ((rc = sqlite3_step(q.get())) == SQLITE_ROW) {
      auto call = str(3);
      if (!std::regex_match(call, call_pattern)) continue;
      auto grid = str(4);
      if (!std::regex_match(grid, grid_pattern)) grid.clear();
      rows.push_back({sqlite3_column_int64(q.get(), 0),
                      sqlite3_column_int64(q.get(), 1),
                      sqlite3_column_int64(q.get(), 2), std::move(call),
                      std::move(grid), str(5), str(6), str(7), str(8), str(9),
                      str(10), str(11), sqlite3_column_int64(q.get(), 12),
                      sqlite3_column_double(q.get(), 13)});
    }
    if (rc != SQLITE_DONE) Check(rc);
    return rows;
  }
  int Recent(const std::string& feed) {
    auto q = Prepare(
        "SELECT count(*) FROM report_receipts WHERE feed=? AND "
        "sent_at>unixepoch()-180");
    Bind(q.get(), 1, feed);
    if (sqlite3_step(q.get()) != SQLITE_ROW)
      throw std::runtime_error("failed to read upload quota");
    return sqlite3_column_int(q.get(), 0);
  }
  void Accept(const std::string& feed, const std::vector<int64_t>& ids) {
    Check(
        sqlite3_exec(db_.get(), "BEGIN IMMEDIATE", nullptr, nullptr, nullptr));
    try {
      auto q = Prepare(
          "INSERT OR IGNORE INTO report_receipts VALUES(?,?,unixepoch())");
      Bind(q.get(), 1, feed);
      for (auto id : ids) {
        Check(sqlite3_bind_int64(q.get(), 2, id));
        if (sqlite3_step(q.get()) != SQLITE_DONE)
          throw std::runtime_error(sqlite3_errmsg(db_.get()));
        Check(sqlite3_reset(q.get()));
      }
      Check(sqlite3_exec(db_.get(), "COMMIT", nullptr, nullptr, nullptr));
    } catch (...) {
      sqlite3_exec(db_.get(), "ROLLBACK", nullptr, nullptr, nullptr);
      throw;
    }
  }

 private:
  void Check(int rc) {
    if (rc != SQLITE_OK) throw std::runtime_error(sqlite3_errmsg(db_.get()));
  }
  using Statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;
  Statement Prepare(const char* sql) {
    sqlite3_stmt* q = nullptr;
    Check(sqlite3_prepare_v2(db_.get(), sql, -1, &q, nullptr));
    return Statement(q, sqlite3_finalize);
  }
  void Bind(sqlite3_stmt* q, int index, const std::string& s) {
    Check(sqlite3_bind_text(q, index, s.data(), s.size(), SQLITE_TRANSIENT));
  }
  std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db_{nullptr,
                                                         sqlite3_close};
};

bool Allowed(const Json& windows, const SmReception& row, const char* type) {
  for (const auto& w : windows) {
    double khz = row.hz / 1000.;
    if (khz < w.at("startFreq").get<double>() ||
        khz > w.at("endFreq").get<double>() || !w.value(type, false))
      continue;
    std::istringstream modes(w.at("modes").get<std::string>());
    for (std::string mode; std::getline(modes, mode, ',');) {
      mode.erase(std::remove_if(mode.begin(), mode.end(),
                                [](char c) { return c == ' '; }),
                 mode.end());
      if (mode == "ALL" || mode == row.mode) return true;
    }
  }
  return false;
}

Json Identity(const Json& cfg) {
  auto sign = cfg.at("sign_in").get<std::string>();
  if (sign.empty()) throw std::runtime_error("RBN sign_in required");
  return {{"t", "id"},
          {"aggVersion", "SkimMaster " SKIM_VERSION},
          {"skimVersion", SKIM_VERSION},
          {"SkimPort", ""},
          {"skimCall", cfg.at("callsign")},
          {"skimGrid", cfg.at("grid")},
          {"skimName", cfg.value("name", "")},
          {"skimQth", cfg.value("qth", "")},
          {"skimSignIn", sign},
          {"shortHash", ShortMd5("hello" + sign + "world")},
          {"showIP", "0"},
          {"ClockNtp", ClockSynchronized()},
          {"ClockNtpDiff", 0},
          {"fingerPrint", "Undefined"},
          {"bandLimits", cfg.at("band_limits")},
          {"cWBandLimits", ""},
          {"rTTYBandLimits", ""},
          {"fTBandLimits", ""},
          {"mIXEDBandLimits", ""},
          {"justCQ", "0"},
          {"masterSCPFilter", "0"},
          {"skimValLevel", "0"}};
}

std::string Post(httplib::Client& client, const char* route, const Json& data) {
  auto response = client.Post(route, data.dump(), "application/json");
  if (!response) throw std::runtime_error(httplib::to_string(response.error()));
  if (response->status < 200 || response->status >= 300)
    throw std::runtime_error("HTTP " + std::to_string(response->status));
  return response->body;
}

Json RbnSettings(Json settings) {
  if (!settings.is_object())
    throw std::runtime_error("invalid RBN handshake settings");
  const auto integer_field = [&](const char* name) {
    if (!settings.contains(name)) return;
    const auto& value = settings.at(name);
    if (!value.is_number_integer())
      throw std::runtime_error("invalid RBN handshake settings");
    if (value.is_number_unsigned()) {
      if (value.get<uint64_t>() >
          static_cast<uint64_t>(std::numeric_limits<int>::max()))
        throw std::runtime_error("invalid RBN handshake settings");
    } else {
      const auto number = value.get<int64_t>();
      if (number < std::numeric_limits<int>::min() ||
          number > std::numeric_limits<int>::max())
        throw std::runtime_error("invalid RBN handshake settings");
    }
  };
  for (const auto* name :
       {"serverUploadInterval", "Limit3Min", "statusFeedbackHttpStatusInterval",
        "statusFeedbackAggSpotsMissingDB"})
    integer_field(name);
  for (const auto* name : {"spotSettings", "spotSettings2"})
    if (settings.contains(name) && !settings.at(name).is_array())
      throw std::runtime_error("invalid RBN handshake settings");
  if (!settings.contains("serverUploadInterval"))
    settings["serverUploadInterval"] = -1;
  if (!settings.contains("spotSettings"))
    settings["spotSettings"] = Json::array();
  if (!settings.contains("spotSettings2"))
    settings["spotSettings2"] = Json::array();
  return settings;
}

std::string Scalar(const Json& v) {
  return v.is_string() ? v.get<std::string>() : v.dump();
}
std::string FeedbackKey(const Json& freq, const Json& call, const Json& snr,
                        const Json& speed) {
  return std::to_string(std::llround(std::stod(Scalar(freq)) * 100)) + "|" +
         Scalar(call) + "|" + Scalar(snr) + "|" + Scalar(speed);
}
class SmRbnFeedback {
 public:
  void Sent(const Json& spots) {
    for (const auto& spot : spots) {
      if (pending_.size() == kMaxPending) pending_.pop_front();
      pending_.push_back({Epoch(), spot});
    }
  }
  void Failed(size_t count, int status) {
    if (errors_.size() < 3000)
      errors_.push_back({std::to_string(Epoch()), std::to_string(count),
                         std::to_string(status)});
  }
  void Poll(const Json& cfg, const Json& identity, const Json& settings) {
    auto interval = settings.value("statusFeedbackHttpStatusInterval", 0);
    if (interval <= 0 || !settings.contains("nodeId")) {
      pending_.clear();
      return;
    }
    auto node = Scalar(settings.at("nodeId"));
    if (node != node_) {
      last_id_ = -1;
      node_ = node;
    }
    if (Clock::now() >= next_poll_) {
      next_poll_ = Clock::now() + std::chrono::seconds(30);
      httplib::Client client(
          cfg.value("verification_endpoint", "https://www.reversebeacon.net"));
      client.set_connection_timeout(5);
      client.set_read_timeout(15);
      httplib::Params p = {{"n", node}};
      if (last_id_ >= 0) p.emplace("l", std::to_string(last_id_));
      auto response = client.Get("/fb0.php", p, httplib::Headers{});
      if (response && response->status == 200) {
        for (const auto& row : Json::parse(response->body)) {
          if (!row.is_array() || row.size() < 5) continue;
          auto key = FeedbackKey(row[2], row[1], row[3], row[4]);
          auto match = std::find_if(
              pending_.begin(), pending_.end(), [&](const auto& p) {
                const auto& s = p.second;
                return FeedbackKey(s[0], s[1], s[3], s[4]) == key;
              });
          if (match != pending_.end()) pending_.erase(match);
          last_id_ = std::max(last_id_,
                              static_cast<int64_t>(std::stoll(Scalar(row[0]))));
        }
      }
    }
    if (Clock::now() < next_status_) return;
    next_status_ = Clock::now() + std::chrono::seconds(interval);
    auto host = settings.value("statusFeedbackServer", ""),
         route = settings.value("statusFeedbackLocation", "");
    if (host.empty() || route.empty()) return;
    httplib::Client client(host);
    client.set_connection_timeout(5);
    client.set_read_timeout(15);
    auto send = [&](const std::string& type, Json payload) {
      auto call = identity.at("skimCall").get<std::string>();
      payload.update(Json{{"type", type},
                          {"call", call},
                          {"hash", ShortMd5(type + call)},
                          {"nTP", ClockSynchronized()},
                          {"diff", 0},
                          {"nodeId", node}});
      Post(client, route.c_str(), payload);
    };
    send("HttpStatus", {{"data", errors_}});
    errors_ = Json::array();
    auto cutoff =
        Epoch() -
        std::max(0, settings.value("statusFeedbackAggSpotsMissingDB", 300));
    Json missing = Json::array();
    for (const auto& [when, spot] : pending_)
      if (when <= cutoff) {
        auto detail = spot;
        std::time_t t = static_cast<std::time_t>(when);
        std::ostringstream date;
        date << std::put_time(std::gmtime(&t), "%m/%d/%Y %H:%M:%S") << ".000";
        detail.push_back(date.str());
        detail.push_back(std::to_string(when));
        missing.push_back(detail);
      }
    if (!missing.empty()) {
      send("AggSpotsMissingDB", {{"msDB", missing}});
      pending_.erase(
          std::remove_if(pending_.begin(), pending_.end(),
                         [&](const auto& p) { return p.first <= cutoff; }),
          pending_.end());
    }
    if (settings.value("statusFeedbackFullInfo", false))
      send("FullInfo", {{"aggToServerIdentData", identity},
                        {"serverToAggIdentReply", settings}});
  }

 private:
  Clock::time_point next_poll_ = Clock::time_point::min(),
                    next_status_ = Clock::time_point::min();
  int64_t last_id_ = -1;
  std::string node_;
  Json errors_ = Json::array();
  static constexpr size_t kMaxPending = 3000;
  std::deque<std::pair<double, Json>> pending_;
};
void Rbn(const Json& cfg, SmOutbox& box) {
  auto identity = Identity(cfg);
  auto host = cfg.value("host", "x.reversebeacon.net");
  int primary = cfg.value("port", 88), backup = cfg.value("backup_port", 80);
  int failures = 0;
  bool use_backup = false;
  SmRbnFeedback feedback;
  auto handshake = Clock::time_point::min();
  auto next_upload = Clock::time_point::min();
  Json settings;
  for (;;) {
    httplib::Client client(host, use_backup ? backup : primary);
    client.set_connection_timeout(10);
    client.set_read_timeout(15);
    client.set_write_timeout(15);
    try {
      if (Clock::now() >= handshake) {
        settings =
            RbnSettings(Json::parse(Post(client, "/rx/6/id.php", identity)));
        handshake = Clock::now() + std::chrono::minutes(5);
      }
      try {
        feedback.Poll(cfg, identity, settings);
      } catch (const std::exception& e) {
        std::cerr << "RBN feedback: " << e.what() << std::endl;
      }
      int interval = settings.at("serverUploadInterval");
      int quota = std::clamp(settings.value("Limit3Min", 3000), 0, 3000) -
                  box.Recent("rbn");
      if (interval >= 0 && quota > 0 && Clock::now() >= next_upload) {
        Json spots = Json::array();
        std::vector<int64_t> ids;
        std::string checksum;
        for (const auto& row : box.Pending("rbn")) {
          static const std::map<std::string, std::string> codes = {
              {"CW", "1"}, {"RTTY", "11"}, {"FT8", "34"}, {"FT4", "45"}};
          auto mode = codes.find(row.mode);
          if (mode == codes.end()) continue;
          bool beacon = row.call.size() > 2 &&
                        row.call.substr(row.call.size() - 2) == "/B";
          bool cq = row.message.rfind("CQ ", 0) == 0;
          if (!Allowed(settings.at("spotSettings"), row,
                       beacon ? "bcn"
                       : cq   ? "cq"
                              : "de"))
            continue;
          std::ostringstream freq;
          freq << std::fixed << std::setprecision(2) << row.hz / 1000.;
          spots.push_back({freq.str(), row.call,
                           beacon ? "4"
                           : cq   ? "1"
                                  : "2",
                           row.snr, row.speed, "0", mode->second, row.time,
                           "D:"});
          std::string call = row.call;
          std::transform(call.begin() + 1, call.end(), call.begin() + 1,
                         [](unsigned char c) { return std::tolower(c); });
          checksum += call;
          ids.push_back(row.id);
          if (!interval || static_cast<int>(ids.size()) >= quota) break;
        }
        if (!ids.empty()) {
          try {
            Post(client, "/rx/6/s.php",
                 {{"agg", identity["aggVersion"]},
                  {"e", identity["skimCall"]},
                  {"fp", identity["fingerPrint"]},
                  {"h", ShortMd5(checksum)},
                  {"nTP", ClockSynchronized()},
                  {"s", spots},
                  {"t", "s"},
                  {"tm", Epoch()}});
          } catch (const std::exception&) {
            feedback.Failed(ids.size(), 0);
            throw;
          }
          feedback.Sent(spots);
          box.Accept("rbn", ids);
          next_upload =
              Clock::now() + std::chrono::seconds(std::max(1, interval));
        }
      }
      failures = 0;
      // Recheck the queue without resetting the server's upload cadence.
      std::this_thread::sleep_until(
          std::min(handshake, Clock::now() + std::chrono::seconds(1)));
    } catch (const std::exception& e) {
      std::cerr << "RBN: " << e.what() << std::endl;
      if (++failures >= 4) {
        use_backup = !use_backup;
        failures = 0;
      }
      handshake = Clock::time_point::min();
      std::this_thread::sleep_for(std::chrono::minutes(2));
    }
  }
}
using Bytes = std::vector<uint8_t>;
void Integer(Bytes& data, uint64_t number, int width) {
  for (int i = width - 1; i >= 0; --i)
    data.push_back(static_cast<uint8_t>(number >> (8 * i)));
}
void Text(Bytes& data, const std::string& s) {
  if (s.size() > 254) throw std::runtime_error("oversized IPFIX field");
  data.push_back(s.size());
  data.insert(data.end(), s.begin(), s.end());
}
void Set(Bytes& packet, int id, Bytes fields) {
  while (fields.size() % 4) fields.push_back(0);
  Integer(packet, id, 2);
  Integer(packet, fields.size() + 4, 2);
  packet.insert(packet.end(), fields.begin(), fields.end());
}
void Psk(const Json& cfg, SmOutbox& box) {
  using Udp = boost::asio::ip::udp;
  boost::asio::io_context io;
  Udp::resolver resolve(io);
  Udp::socket socket(io, Udp::v4());
  uint32_t sequence = 0, domain = std::random_device{}();
  for (;;) {
    try {
      Bytes receiver;
      Text(receiver, cfg.at("callsign"));
      Text(receiver, cfg.at("grid"));
      Text(receiver, "SkimMaster " SKIM_VERSION);
      Bytes records;
      std::vector<int64_t> ids;
      auto flush = [&] {
        if (ids.empty()) return;
        Bytes p;
        Integer(p, 10, 2);
        Integer(p, 0, 2);
        Integer(p, static_cast<uint32_t>(Epoch()), 4);
        Integer(p, sequence, 4);
        Integer(p, domain, 4);
        Bytes options;
        Integer(options, 0x9992, 2);
        Integer(options, 3, 2);
        Integer(options, 1, 2);
        auto field = [](Bytes& b, int id, int length) {
          Integer(b, id | 0x8000, 2);
          Integer(b, length, 2);
          Integer(b, 30351, 4);
        };
        for (int id : {2, 4, 8}) field(options, id, 65535);
        Set(p, 3, options);
        Bytes descriptor;
        Integer(descriptor, 0x9993, 2);
        Integer(descriptor, 6, 2);
        field(descriptor, 1, 65535);
        field(descriptor, 5, 5);
        field(descriptor, 10, 65535);
        field(descriptor, 11, 1);
        field(descriptor, 3, 65535);
        Integer(descriptor, 150, 2);
        Integer(descriptor, 4, 2);
        Set(p, 2, descriptor);
        Set(p, 0x9992, receiver);
        Set(p, 0x9993, records);
        p[2] = p.size() >> 8;
        p[3] = p.size() & 255;
        auto dest = resolve.resolve(
            Udp::v4(), cfg.value("host", "report.pskreporter.info"),
            std::to_string(cfg.value("port", 4739)));
        socket.send_to(boost::asio::buffer(p), dest.begin()->endpoint());
        sequence += ids.size();
        box.Accept("psk", ids);
        ids.clear();
        records.clear();
      };
      for (const auto& row : box.Pending("psk", 900)) {
        if (row.mode != "FT8" && row.mode != "FT4") continue;
        if (records.size() > 1100) flush();
        Text(records, row.call);
        Integer(records, row.hz, 5);
        Text(records, row.mode);
        Integer(records, 1, 1);
        Text(records, row.grid);
        Integer(records, row.utc / 1000, 4);
        ids.push_back(row.id);
      }
      flush();
    } catch (const std::exception& e) {
      std::cerr << "PSKReporter: " << e.what() << std::endl;
    }
    std::this_thread::sleep_for(std::chrono::minutes(5));
  }
}
void Wsprnet(const Json& cfg, SmOutbox& box) {
  httplib::Client client(cfg.value("endpoint", "https://wsprnet.org"));
  client.set_connection_timeout(10);
  client.set_read_timeout(15);
  client.set_write_timeout(15);
  auto decimal = [](double value, int digits) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(digits) << value;
    return out.str();
  };
  static const std::regex accepted(R"(\b[1-9][0-9]* spot\(s\) added\b)");
  for (;;) {
    try {
      for (const auto& row : box.Pending("wsprnet")) {
        if (row.mode != "WSPR" || row.snr.empty() || row.grid.empty() ||
            row.power.empty() || row.dial <= 0)
          continue;
        httplib::Params fields = {{"function", "wspr"},
                                  {"rcall", cfg.at("callsign")},
                                  {"rgrid", cfg.at("grid")},
                                  {"rqrg", decimal(row.dial / 1e6, 6)},
                                  {"tqrg", decimal(row.hz / 1e6, 6)},
                                  {"date", row.date},
                                  {"time", row.time.substr(0, 4)},
                                  {"sig", row.snr},
                                  {"dt", decimal(row.dt, 1)},
                                  {"tcall", row.call},
                                  {"tgrid", row.grid},
                                  {"dbm", row.power},
                                  {"version", "SkimMaster-" SKIM_VERSION},
                                  {"mode", "2"}};
        auto res = client.Get("/post", fields, httplib::Headers{});
        if (!res || res->status / 100 != 2)
          throw std::runtime_error("upload failed");
        if (!std::regex_search(res->body, accepted)) {
          std::cerr << "WSPRnet did not accept spot " << row.id << '\n';
          continue;
        }
        box.Accept("wsprnet", {row.id});
      }
    } catch (const std::exception& e) {
      std::cerr << "WSPRnet: " << e.what() << std::endl;
    }
    std::this_thread::sleep_for(std::chrono::seconds(30));
  }
}
}  // namespace

void RunReporter(const char* feed, const char* database, const char* config) {
  std::ifstream file(config);
  if (!file) throw std::runtime_error("cannot open reporter configuration");
  RunReporter(feed, database, Json::parse(file));
}
void RunReporter(const char* feed, const char* database, const Json& cfg) {
  if (!std::regex_match(cfg.at("callsign").get<std::string>(),
                        std::regex("[A-Z0-9/-]{2,16}")) ||
      !std::regex_match(cfg.at("grid").get<std::string>(),
                        std::regex("[A-Ra-r]{2}[0-9]{2}([A-Xa-x]{2})?")))
    throw std::runtime_error("invalid station callsign or locator");
  const std::string service(feed);
  if (service != "rbn" && service != "psk" && service != "wsprnet")
    throw std::runtime_error("unknown feed");
  auto lock_path = std::filesystem::weakly_canonical(database).string() + "." +
                   service + ".lock";
  std::ofstream(lock_path, std::ios::app).close();
  boost::interprocess::file_lock lock(lock_path.c_str());
  if (!lock.try_lock()) throw std::runtime_error("reporter already running");
  SmOutbox box(database);
  if (service == "rbn")
    Rbn(cfg, box);
  else if (service == "psk")
    Psk(cfg, box);
  else
    Wsprnet(cfg, box);
}
}  // namespace skim
