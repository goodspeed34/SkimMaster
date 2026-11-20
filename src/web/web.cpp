#include "web/web.h"

#include <httplib.h>
#include <openssl/rand.h>
#include <sqlite3.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <inja/inja.hpp>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <regex>
#include <stdexcept>

#include "app/backup.h"
#include "app/config.h"
#include "app/instance.h"
#include "app/slice_tuning.h"
#include "app/station.h"
#include "app/station_lock.h"
#include "dsp/spectrum.h"
#include "receiver/receiver.h"
#include "store/store.h"
#include "web/panel_assets.h"

namespace skim {
namespace {
using Json = nlohmann::json;
std::string Escape(const char* value) {
  std::string out;
  for (char c : std::string_view(value)) {
    switch (c) {
      case '&':
        out += "&amp;";
        break;
      case '<':
        out += "&lt;";
        break;
      case '>':
        out += "&gt;";
        break;
      case '"':
        out += "&quot;";
        break;
      default:
        out += c;
    }
  }
  return out;
}
std::optional<int64_t> FormInteger(const httplib::Request& req, const char* key,
                                   int64_t min, int64_t max,
                                   bool optional = false) {
  auto value = req.get_param_value(key);
  if (optional && value.empty()) return std::nullopt;
  int64_t number = 0;
  auto end = value.data() + value.size();
  auto parsed = std::from_chars(value.data(), end, number);
  if (parsed.ec != std::errc{} || parsed.ptr != end || number < min ||
      number > max)
    throw std::runtime_error(std::string("invalid ") + key);
  return number;
}
std::optional<double> FormDecimal(const httplib::Request& req,
                                  const char* key) {
  auto value = req.get_param_value(key);
  if (value.empty()) return std::nullopt;
  size_t end = 0;
  double result = 0;
  try {
    result = std::stod(value, &end);
  } catch (const std::exception&) {
    throw std::runtime_error(std::string("invalid ") + key);
  }
  if (end != value.size() || !std::isfinite(result))
    throw std::runtime_error(std::string("invalid ") + key);
  return result;
}
Json DriverOptions() {
  auto options = Json::array();
  for (const auto& driver : ReceiverDrivers())
    options.push_back({driver.id, driver.label});
  return options;
}
std::string DisplayField(const Json& row, const char* key,
                         const char* fallback = "") {
  auto field = row.find(key);
  if (field == row.end()) return fallback;
  return field->is_string()
             ? Escape(field->get_ref<const std::string&>().c_str())
             : field->dump();
}
constexpr int64_t kMaxFrequency = 1000000000000LL;
constexpr int64_t kMaxIndex = 1024 * 1024;

std::optional<SmSliceSettings> FormSlice(const httplib::Request& req) {
  auto mode = req.get_param_value("mode");
  if (mode.empty()) return std::nullopt;
  const bool skimmer = IsSkimmer(mode);
  auto tuning = FormInteger(req, skimmer ? "slice_center" : "dial_hz", 1,
                            kMaxFrequency, skimmer);
  return SmSliceSettings{std::move(mode), tuning};
}

std::string EditReceiverUrl(const std::string& receiver) {
  return "/receiver-edit?edit=" + httplib::detail::encode_query_param(receiver);
}

std::string UtcMinute(int minute) {
  std::ostringstream out;
  out << std::setfill('0') << std::setw(2) << minute / 60 << ':' << std::setw(2)
      << minute % 60;
  return out.str();
}

int FormTime(const httplib::Request& req, const char* key) {
  const auto value = req.get_param_value(key);
  if (!std::regex_match(value, std::regex("[0-2][0-9]:[0-5][0-9]")) ||
      std::stoi(value.substr(0, 2)) >= 24)
    throw std::runtime_error(std::string("invalid UTC ") + key);
  return std::stoi(value.substr(0, 2)) * 60 + std::stoi(value.substr(3));
}
Json Query(const char* database, const char* sql,
           const std::vector<std::string>& parameters = {}, bool html = true) {
  sqlite3* handle = nullptr;
  int rc = sqlite3_open_v2(database, &handle, SQLITE_OPEN_READONLY, nullptr);
  std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db(handle, sqlite3_close);
  if (rc != SQLITE_OK) throw std::runtime_error(sqlite3_errmsg(handle));
  sqlite3_busy_timeout(handle, 5000);
  sqlite3_stmt* query = nullptr;
  if (sqlite3_prepare_v2(handle, sql, -1, &query, nullptr) != SQLITE_OK)
    throw std::runtime_error(sqlite3_errmsg(handle));
  std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> stmt(
      query, sqlite3_finalize);
  for (size_t i = 0; i < parameters.size(); ++i) {
    const auto& value = parameters[i];
    if (sqlite3_bind_text(query, i + 1, value.data(), value.size(),
                          SQLITE_TRANSIENT) != SQLITE_OK)
      throw std::runtime_error(sqlite3_errmsg(handle));
  }
  Json rows = Json::array();
  while ((rc = sqlite3_step(query)) == SQLITE_ROW) {
    Json row = Json::array();
    for (int i = 0; i < sqlite3_column_count(query); ++i) {
      auto cell = sqlite3_column_text(query, i);
      const auto* value = cell ? reinterpret_cast<const char*>(cell) : "";
      row.push_back(html ? Escape(value) : std::string(value));
    }
    rows.push_back(std::move(row));
  }
  if (rc != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(handle));
  return rows;
}

std::string CsvDocument(const Json& columns, const Json& rows) {
  std::ostringstream csv;
  auto write_row = [&](const Json& row) {
    bool first = true;
    for (const auto& cell : row) {
      if (!first) csv << ',';
      first = false;
      csv << '"';
      const auto& value = cell.get_ref<const std::string&>();
      if (!value.empty() && std::string_view("=+-@\t\r").find(value.front()) !=
                                std::string_view::npos)
        csv << '\'';
      for (char c : value) {
        if (c == '"') csv << '"';
        csv << c;
      }
      csv << '"';
    }
    csv << "\r\n";
  };
  write_row(columns);
  for (const auto& row : rows) write_row(row);
  return csv.str();
}

int64_t DaysFromCivil(int year, unsigned month, unsigned day) {
  year -= month <= 2;
  const int era = (year >= 0 ? year : year - 399) / 400;
  const unsigned year_of_era = year - era * 400;
  const unsigned adjusted_month = month > 2 ? month - 3 : month + 9;
  const unsigned day_of_year = (153 * adjusted_month + 2) / 5 + day - 1;
  const unsigned day_of_era =
      year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
  return era * 146097LL + day_of_era - 719468LL;
}

std::optional<int64_t> GalleryDateStart(const std::string& value,
                                        const char* field) {
  if (value.empty()) return std::nullopt;
  static const std::regex date_pattern(R"([0-9]{4}-[0-9]{2}-[0-9]{2})");
  if (!std::regex_match(value, date_pattern))
    throw std::runtime_error(std::string("invalid gallery ") + field + " date");
  const int year = std::stoi(value.substr(0, 4));
  const unsigned month = std::stoul(value.substr(5, 2));
  const unsigned day = std::stoul(value.substr(8, 2));
  if (year < 1970 || month < 1 || month > 12)
    throw std::runtime_error(std::string("invalid gallery ") + field + " date");
  constexpr unsigned month_days[] = {31, 28, 31, 30, 31, 30,
                                     31, 31, 30, 31, 30, 31};
  const bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
  const auto days = month_days[month - 1] + (month == 2 && leap ? 1 : 0);
  if (day < 1 || day > days)
    throw std::runtime_error(std::string("invalid gallery ") + field + " date");
  return DaysFromCivil(year, month, day) * 86400LL * 1000000LL;
}

std::string GalleryUtc(int64_t timestamp_us) {
  const std::time_t timestamp =
      static_cast<std::time_t>(timestamp_us / 1000000);
  std::tm utc{};
#ifdef _WIN32
  if (gmtime_s(&utc, &timestamp))
    throw std::runtime_error("cannot format gallery image timestamp");
#else
  if (!gmtime_r(&timestamp, &utc))
    throw std::runtime_error("cannot format gallery image timestamp");
#endif
  std::ostringstream formatted;
  formatted << std::put_time(&utc, "%Y-%m-%d %H:%M:%S");
  return formatted.str();
}

const SmReferencePage* FindPage(const std::string& slug) {
  for (const auto& page : kReferencePages)
    if (slug == page.slug) return &page;
  return nullptr;
}

std::string PanelSlug(const std::string& path) {
  static const std::map<std::string, std::string> aliases{
      {"/", "system-summary"},           {"/index.html", "system-summary"},
      {"/summary", "system-summary"},    {"/spectrum", "spectrum-waterfall"},
      {"/map", "live-dx-map"},           {"/receivers", "receiver-status"},
      {"/stations", "heard-stations"},   {"/activity", "band-mode-activity"},
      {"/noise", "noise-floor"},         {"/gallery", "sstv-gallery"},
      {"/settings", "general-settings"}, {"/log", "system-log"},
      {"/network", "dx-cluster"},
  };
  auto alias = aliases.find(path);
  return alias != aliases.end() ? alias->second : path.substr(1);
}

struct SmSettingsSnapshot {
  std::optional<SmInstanceSettings> settings;
  std::shared_ptr<const SmInstance> instance;
  std::string error;
};

class SmSettingsCache {
 public:
  explicit SmSettingsCache(std::filesystem::path path)
      : path_(std::move(path)) {
    Read();
  }

  SmSettingsSnapshot Read() {
    std::lock_guard<std::mutex> lock(mutex_);
    try {
      auto settings = LoadSettings(path_);
      if (!settings_ || settings_->revision != settings.revision) {
        auto instance = SmInstance::FromSettings(settings);
        settings_ = std::move(settings);
        instance_ = std::move(instance);
      }
      error_.clear();
    } catch (const std::exception& error) {
      error_ = error.what();
    }
    return {settings_, instance_, error_};
  }

 private:
  std::filesystem::path path_;
  std::mutex mutex_;
  std::optional<SmInstanceSettings> settings_;
  std::shared_ptr<const SmInstance> instance_;
  std::string error_;
};
}  // namespace
void Serve(const char* config) {
  const auto startup = LoadSettings(config);
  Serve(config, startup.address.c_str(), startup.port, startup.database.c_str(),
        startup.gallery.c_str());
}

void Serve(const char* config, const char* address, int port,
           const char* database, const char* gallery) {
  const auto database_path = OperationalDatabasePath(database);
  const auto database_name = database_path.string();
  database = database_name.c_str();
  SaveSlot(database, "", "", 0, {});  // Initialize the store for a new station.
  std::filesystem::create_directories(gallery);
  SmSettingsCache settings_cache(config);
  httplib::Server server;
  server.new_task_queue = [] { return new httplib::ThreadPool(4); };
  InitSpectrum(database);
  server.set_payload_max_length(1024 * 1024);
  std::array<unsigned char, 32> random{};
  if (RAND_bytes(random.data(), random.size()) != 1)
    throw std::runtime_error("could not initialize session token");
  std::ostringstream token;
  for (auto byte : random)
    token << std::hex << std::setw(2) << std::setfill('0') << int(byte);
  auto validate_form = [&](const httplib::Request& req,
                           httplib::Response& res) {
    res.set_header("Cache-Control", "no-store");
    if (req.get_param_value("csrf") != token.str()) {
      res.status = 403;
      res.set_content("This form expired. Reload the page and try again.",
                      "text/plain");
      return false;
    }
    return true;
  };
  std::mutex config_writes;
  const auto panel_start = std::chrono::steady_clock::now();
  auto render = [&](const SmReferencePage& page, Json model,
                    httplib::Response& res) {
    model["page"] = page.slug;
    model["title"] = page.title;
    model["reference_help"] = page.help;
    if (!model.contains("config_error")) model["config_error"] = "";
    if (!model.contains("config_recovery_guidance"))
      model["config_recovery_guidance"] = "";
    auto live = Query(
        database,
        "SELECT id,printf('%.3f',json_extract(plan,'$.center_hz')/1e6),"
        "printf('%g',json_extract(plan,'$.rate')/1e3),"
        "json_array_length(plan,'$.slices') FROM workers "
        "WHERE id NOT LIKE '@%' AND status='Worker launched' ORDER BY id");
    model["receivers"] = live;
    int slices = 0;
    for (const auto& receiver : live)
      slices += std::stoi(receiver[3].get<std::string>());
    model["slice_count"] = slices;
    model["bands"] = "—";
    model["cpu_load"] = "—";
    model["version"] = SKIM_VERSION;
    auto count = std::chrono::duration_cast<std::chrono::seconds>(
                     std::chrono::steady_clock::now() - panel_start)
                     .count();
    model["uptime"] = count;
    std::ostringstream uptime;
    uptime << count / 86400 << "d " << std::setfill('0') << std::setw(2)
           << (count / 3600) % 24 << "h " << std::setw(2) << (count / 60) % 60
           << 'm';
    model["uptime_label"] = uptime.str();
    model["utc"] = Query(database, "SELECT datetime('now')")[0][0];
    res.set_content(inja::render(kTemplate, model), "text/html; charset=utf-8");
  };
  auto render_services = [&](const std::string& slug,
                             const httplib::Request& req,
                             httplib::Response& res,
                             const std::string& error = "") {
    const auto settings = LoadSettings(config);
    const std::vector<std::string> services =
        slug == "dx-cluster" ? std::vector<std::string>{"cluster"}
        : slug == "rbn"      ? std::vector<std::string>{"rbn"}
                             : std::vector<std::string>{"psk", "wsprnet"};
    const auto submitted = req.method == "POST";
    const auto submitted_service = req.get_param_value("service");
    auto cards = Json::array();
    for (const auto& name : services) {
      const auto found = settings.services.find(name);
      const Json options =
          found != settings.services.end() && found->is_object()
              ? *found
              : Json::object();
      const bool enabled = options.value("enabled", false);
      const auto get_string = [&](const char* key, const char* fallback = "") {
        return options.value(key, std::string(fallback));
      };
      std::string label = name == "cluster" ? "Local DX Cluster"
                          : name == "psk"   ? "PSK Reporter"
                          : name == "rbn"   ? "Reverse Beacon Network"
                                            : "WSPRnet";
      const auto worker_id = "@" + name;
      auto worker = Query(database, "SELECT status FROM workers WHERE id=?1",
                          {worker_id}, false);
      const auto worker_status =
          !enabled         ? std::string("Disabled")
          : worker.empty() ? std::string("Worker pending")
                           : Escape(worker[0][0].get<std::string>().c_str());
      std::string receipts = "—", last_receipt = "—";
      if (name != "cluster") {
        auto receipt = Query(
            database,
            "SELECT count(*),coalesce(datetime(max(sent_at),'unixepoch'),'—') "
            "FROM report_receipts WHERE feed=?1",
            {name}, false);
        receipts = receipt[0][0].get<std::string>();
        last_receipt = receipt[0][1].get<std::string>();
      }
      const auto identity = settings.identity.value_or(SmStationIdentity{});
      auto explicit_call = get_string("callsign");
      auto explicit_grid = get_string("grid");
      auto effective_call =
          explicit_call.empty() ? identity.callsign : explicit_call;
      auto effective_grid =
          explicit_grid.empty() ? identity.grid : explicit_grid;
      auto host = get_string("host", name == "rbn"   ? "x.reversebeacon.net"
                                     : name == "psk" ? "report.pskreporter.info"
                                                     : "");
      const auto default_port = name == "cluster" ? 7300
                                : name == "rbn"   ? 88
                                                  : 4739;
      const auto port = std::to_string(options.value("port", default_port));
      const auto backup_port = std::to_string(options.value("backup_port", 80));
      const auto endpoint = get_string("endpoint", "https://wsprnet.org");
      const auto sign_in = get_string("sign_in", identity.email.c_str());
      const auto band_limits = get_string("band_limits");
      if (submitted && submitted_service == name) {
        explicit_call = req.get_param_value("callsign");
        explicit_grid = req.get_param_value("grid");
        const auto submitted_port = req.get_param_value("port");
        const auto endpoint_display =
            name == "cluster" && submitted_port.empty() ? std::string("7300")
                                                        : submitted_port;
        cards.push_back(
            {{"service", name},
             {"label", label},
             {"enabled", req.has_param("enabled")},
             {"worker_status", worker_status},
             {"receipts", receipts},
             {"last_receipt", last_receipt},
             {"callsign", Escape(explicit_call.c_str())},
             {"grid", Escape(explicit_grid.c_str())},
             {"effective_call", Escape(effective_call.c_str())},
             {"effective_grid", Escape(effective_grid.c_str())},
             {"call_fallback", Escape(identity.callsign.c_str())},
             {"grid_fallback", Escape(identity.grid.c_str())},
             {"host", Escape(req.get_param_value("host").c_str())},
             {"port", Escape(req.get_param_value("port").c_str())},
             {"endpoint_display", Escape(endpoint_display.c_str())},
             {"backup_port",
              Escape(req.get_param_value("backup_port").c_str())},
             {"endpoint", Escape(req.get_param_value("endpoint").c_str())},
             {"sign_in", Escape(req.get_param_value("sign_in").c_str())},
             {"sign_in_fallback", Escape(identity.email.c_str())},
             {"band_limits",
              Escape(req.get_param_value("band_limits").c_str())},
             {"kind", name}});
        continue;
      }
      cards.push_back({{"service", name},
                       {"label", label},
                       {"enabled", enabled},
                       {"worker_status", worker_status},
                       {"receipts", receipts},
                       {"last_receipt", last_receipt},
                       {"callsign", Escape(explicit_call.c_str())},
                       {"grid", Escape(explicit_grid.c_str())},
                       {"effective_call", Escape(effective_call.c_str())},
                       {"effective_grid", Escape(effective_grid.c_str())},
                       {"call_fallback", Escape(identity.callsign.c_str())},
                       {"grid_fallback", Escape(identity.grid.c_str())},
                       {"host", Escape(host.c_str())},
                       {"port", Escape(port.c_str())},
                       {"endpoint_display", Escape(port.c_str())},
                       {"backup_port", Escape(backup_port.c_str())},
                       {"endpoint", Escape(endpoint.c_str())},
                       {"sign_in", Escape(get_string("sign_in").c_str())},
                       {"sign_in_fallback", Escape(identity.email.c_str())},
                       {"band_limits", Escape(band_limits.c_str())},
                       {"kind", name}});
    }
    Json model = {{"admin", false},
                  {"gallery", false},
                  {"empty", false},
                  {"csrf", token.str()},
                  {"revision", settings.revision},
                  {"service_cards", std::move(cards)},
                  {"service_error", Escape(error.c_str())},
                  {"service_notice", ""}};
    if (req.get_param_value("saved") == "1")
      model["service_notice"] = "Service settings saved.";
    else if (req.get_param_value("restarted") == "1")
      model["service_notice"] = "Local service worker restart requested.";
    render(*FindPage(slug), std::move(model), res);
  };
  auto render_wizard = [&](const httplib::Request& req, httplib::Response& res,
                           const std::string& error = "") {
    Json model{{"csrf", token.str()},
               {"error", Escape(error.c_str())},
               {"initial_step", error.empty() ? 0 : 2},
               {"mode", "FT8"},
               {"rate", "192000"},
               {"drivers", DriverOptions()},
               {"probe_driver", "soapy"},
               {"dial_hz", "14074000"},
               {"repeat", true}};
    if (error.empty()) {
      auto settings = LoadSettings(config);
      auto identity = settings.identity.value_or(SmStationIdentity{});
      model["revision"] = settings.revision;
      model["callsign"] = Escape(identity.callsign.c_str());
      model["email"] = Escape(identity.email.c_str());
      model["name"] = Escape(identity.name.c_str());
      model["grid"] = Escape(identity.grid.c_str());
      for (auto field : {"receiver", "device", "center_hz", "utc_ms"})
        model[field] = "";
    } else {
      for (auto field :
           {"revision", "callsign", "email", "name", "grid", "receiver",
            "device", "rate", "dial_hz", "center_hz", "utc_ms", "mode"})
        model[field] = Escape(req.get_param_value(field).c_str());
      model["repeat"] = req.get_param_value("repeat") == "on";
    }
    res.set_content(inja::render(kWizard, model), "text/html; charset=utf-8");
  };
  auto render_receiver_editor = [&](const httplib::Request& req,
                                    httplib::Response& res,
                                    const std::string& error = "") {
    res.set_header("Cache-Control", "no-store");
    auto settings = LoadSettings(config);
    auto receivers = Json::array();
    auto fields = Json{{"enabled", true}, {"repeat", true}, {"rate", "192000"}};
    for (auto key : {"receiver", "device", "center_hz", "gain_db", "ppm",
                     "utc_ms", "mode", "dial_hz", "slice_center"})
      fields[key] = "";
    std::string action, original;
    if (req.method == "POST") {
      action = req.get_param_value("action");
      original = req.get_param_value("original");
    } else if (req.has_param("delete")) {
      original = req.get_param_value("delete");
      action = "delete";
    } else if (req.has_param("edit") || req.has_param("add")) {
      original = req.get_param_value("edit");
      action = "save";
    }
    if (req.method == "GET" && req.has_param("slice_index"))
      action = "slice-save";
    const auto group = req.get_param_value("slice_group");
    const auto slice_index = req.get_param_value("slice_index");
    const auto schedule_index = req.get_param_value("schedule_index");
    auto attached = Json::array();
    auto slice_fields =
        Json{{"mode", ""}, {"slice_center", ""}, {"dial_hz", ""}};
    bool found = original.empty() && action != "delete";
    std::string probe_driver = "soapy";
    const auto instance = SmInstance::FromSettings(settings);
    for (const auto* source : instance->receivers()) {
      const auto& receiver = source->settings();
      const auto& name = source->id();
      const auto& device = source->device();
      const auto& driver = *source->driver();
      receivers.push_back({{"name", Escape(name.c_str())},
                           {"device", Escape(device.c_str())},
                           {"driver", driver.label},
                           {"enabled", source->enabled() ? "Yes" : "No"},
                           {"rate", source->rate()}});
      if (name != original) continue;
      std::optional<int64_t> tuned;
      for (const auto& row : Query(database,
                                   "SELECT id,json_extract(plan,'$.center_hz') "
                                   "FROM workers WHERE id NOT LIKE '@%'"))
        if (row[0] == Escape(name.c_str()) && row[1] != "")
          tuned = std::stoll(row[1].get<std::string>());
      if (!tuned) tuned = source->configured_center();
      const auto schedules = receiver.value("schedules", Json::array());
      for (const auto* slice : source->slices()) {
        const char* key = "slices";
        std::string label = "Manual", parent;
        auto center = tuned;
        if (slice->group() == SmSliceGroup::Attached) {
          key = "attached_slices";
          label = "Attached";
        } else if (slice->group() == SmSliceGroup::Scheduled) {
          const auto index = slice->schedule_index();
          const auto& schedule = schedules.at(index);
          label = schedule.value("name", "");
          if (label.empty()) label = "Schedule " + std::to_string(index + 1);
          label = Escape(label.c_str());
          parent = std::to_string(index);
          center = schedule.at("center_hz").get<int64_t>();
        }
        const auto mode = ModeName(slice->mode());
        const auto frequency = slice->configured_reference();
        const bool skimmer = IsSkimmer(mode);
        const auto reference = frequency
                                   ? std::to_string(*frequency) +
                                         (skimmer ? " Hz center" : " Hz dial")
                                   : "Receiver center";
        const auto coverage = !center                ? "Awaiting tuning"
                              : slice->fits(*center) ? "In bandwidth"
                                                     : "Outside bandwidth";
        attached.push_back({{"mode", mode},
                            {"reference", reference},
                            {"coverage", coverage},
                            {"group", key},
                            {"label", label},
                            {"index", slice->index()},
                            {"schedule", parent}});
        if (group == key && schedule_index == parent &&
            slice_index == std::to_string(slice->index())) {
          slice_fields["mode"] = mode;
          if (frequency)
            slice_fields[skimmer ? "slice_center" : "dial_hz"] =
                std::to_string(*frequency);
        }
      }
      found = true;
      probe_driver = driver.id;
      for (auto key : {"receiver", "device", "rate", "center_hz", "gain_db",
                       "ppm", "utc_ms"})
        fields[key] = DisplayField(receiver, key);
      fields["enabled"] = receiver.value("enabled", true);
      fields["repeat"] = receiver.value("repeat", false);
    }
    if (!found && error.empty()) {
      res.status = 404;
      action.clear();
    }
    bool slice_missing = action == "slice-save" &&
                         slice_fields.at("mode") == "" && error.empty();
    if (slice_missing) {
      res.status = 404;
      action.clear();
    }
    if (req.method == "POST" && action == "save") {
      for (auto key : {"receiver", "device", "rate", "center_hz", "gain_db",
                       "ppm", "utc_ms", "mode", "dial_hz", "slice_center"})
        fields[key] = Escape(req.get_param_value(key).c_str());
      fields["enabled"] = req.get_param_value("enabled") == "on";
      fields["repeat"] = req.get_param_value("repeat") == "on";
    }
    if (req.method == "POST" && action == "slice-save")
      for (auto key : {"mode", "slice_center", "dial_hz"})
        slice_fields[key] = Escape(req.get_param_value(key).c_str());
    Json model = {
        {"admin", true},
        {"empty", false},
        {"csrf", token.str()},
        {"revision", req.method == "POST"
                         ? Escape(req.get_param_value("revision").c_str())
                         : settings.revision},
        {"attached", std::move(attached)},
        {"receiver_list", std::move(receivers)},
        {"receiver_action", action},
        {"receiver_error", slice_missing             ? "Slice not found."
                           : found || !error.empty() ? Escape(error.c_str())
                                                     : "Receiver not found."},
        {"original", Escape(original.c_str())},
        {"fields", std::move(fields)},
        {"slice_fields", std::move(slice_fields)},
        {"slice_group", Escape(group.c_str())},
        {"slice_index", Escape(slice_index.c_str())},
        {"schedule_index", Escape(schedule_index.c_str())},
        {"receiver_modes", {"CW", "RTTY", "FT8", "FT4", "WSPR", "SSTV"}},
        {"drivers", DriverOptions()},
        {"probe_driver", probe_driver},
    };
    render(*FindPage("receiver-edit"), model, res);
  };
  server.Get("/receiver-edit", [&](const auto& req, auto& res) {
    render_receiver_editor(req, res);
  });
  server.Post("/receiver-edit", [&](const auto& req, auto& res) {
    if (!validate_form(req, res)) return;
    try {
      std::optional<SmReceiverSettings> receiver;
      const auto action = req.get_param_value("action");
      if (action == "slice-save" || action == "slice-delete") {
        auto slice = action == "slice-save" ? FormSlice(req) : std::nullopt;
        if (action == "slice-save" && !slice)
          throw std::runtime_error("select a decoder mode");
        auto index = *FormInteger(req, "slice_index", 0, kMaxIndex);
        auto schedule = FormInteger(req, "schedule_index", 0, kMaxIndex, true);
        auto original = req.get_param_value("original");
        std::lock_guard<std::mutex> lock(config_writes);
        UpdateSlice(config, req.get_param_value("revision"), original,
                    req.get_param_value("slice_group"), schedule, index, slice);
        res.set_redirect(EditReceiverUrl(original), 303);
        return;
      }
      if (action == "save") {
        auto& r = receiver.emplace();
        r.name = req.get_param_value("receiver");
        r.device = req.get_param_value("device");
        r.rate = *FormInteger(req, "rate", 12000, 6144000);
        r.enabled = req.get_param_value("enabled") == "on";
        r.center = FormInteger(req, "center_hz", 1, 1000000000000LL, true);
        r.gain = FormDecimal(req, "gain_db");
        r.ppm = FormDecimal(req, "ppm");
        r.repeat = req.get_param_value("repeat") == "on";
        r.utc = FormInteger(req, "utc_ms", 0, 100000000000000LL, true);
        r.slice_center =
            FormInteger(req, "slice_center", 1, 1000000000000LL, true);
        r.mode = req.get_param_value("mode");
        if (!r.mode.empty() && !IsSkimmer(r.mode))
          r.dial = FormInteger(req, "dial_hz", 1, 1000000000000LL, true);
      } else if (action != "delete") {
        throw std::runtime_error("invalid receiver action");
      }
      std::lock_guard<std::mutex> lock(config_writes);
      UpdateReceiver(config, req.get_param_value("revision"),
                     req.get_param_value("original"), receiver);
      res.set_redirect("/receiver-edit", 303);
    } catch (const std::exception& error) {
      res.status = 400;
      render_receiver_editor(req, res, error.what());
    }
  });
  std::mutex probe_mutex;
  server.Post("/receiver-test", [&](const auto& req, auto& res) {
    if (!validate_form(req, res)) return;
    std::unique_lock<std::mutex> lock(probe_mutex, std::try_to_lock);
    if (!lock) {
      res.status = 409;
      res.set_content("A receiver scan or test is already running.",
                      "text/plain");
      return;
    }
    try {
      auto device = req.get_param_value("device");
      if (device.empty() || device == "-")
        throw std::runtime_error("Enter a device parameter.");
      auto center = *FormInteger(req, "center_hz", 1, kMaxFrequency);
      auto rate = *FormInteger(req, "rate", 12000, 6144000);
      auto gain = FormDecimal(req, "gain_db");
      auto ppm = FormDecimal(req, "ppm");
      if ((gain && std::abs(*gain) > 200) || (ppm && std::abs(*ppm) > 1000))
        throw std::runtime_error("Invalid gain or frequency correction.");
      if (IsWavReceiver(device)) {
        device =
            "wav:" + ResolveWavPath(std::string_view(device).substr(4),
                                    std::filesystem::path(config).parent_path())
                         .string();
      } else {
        for (const auto& row : Query(database,
                                     "SELECT json_extract(plan,'$.device') "
                                     "FROM workers WHERE id NOT LIKE '@%'"))
          if (row[0] == Escape(device.c_str()))
            throw std::runtime_error(
                "Disable this receiver and wait for its allocation to stop "
                "before testing.");
      }
      Json settings{{"device", device}, {"center_hz", center}, {"rate", rate}};
      if (gain) settings["gain_db"] = *gain;
      if (ppm) settings["ppm"] = *ppm;
      TestReceiverCapture(settings);
      res.set_content("IQ samples received with these settings.", "text/plain");
    } catch (const std::exception& error) {
      res.status = 400;
      res.set_content(error.what(), "text/plain");
    }
  });
  server.Post("/receiver-probe", [&](const auto& req, auto& res) {
    if (!validate_form(req, res)) return;
    std::unique_lock<std::mutex> lock(probe_mutex, std::try_to_lock);
    if (!lock) {
      res.status = 409;
      res.set_content("A receiver scan is already running. Try again shortly.",
                      "text/plain");
      return;
    }
    try {
      auto devices = ProbeReceiver(req.get_param_value("driver"),
                                   req.get_param_value("target"),
                                   std::filesystem::path(config).parent_path());
      auto rows = Json::array();
      for (auto& device : devices)
        rows.push_back({{"device", device.device},
                        {"label", device.label},
                        {"rate", device.rate}});
      res.set_content(rows.dump(-1, ' ', false, Json::error_handler_t::replace),
                      "application/json");
    } catch (const std::exception& error) {
      res.status = 400;
      res.set_content(error.what(), "text/plain");
    }
  });
  auto render_schedules = [&](const httplib::Request& req,
                              httplib::Response& res,
                              const std::string& error = "") {
    res.set_header("Cache-Control", "no-store");
    const auto settings = LoadSettings(config);
    const auto overrides = ScheduleOverrides(database);
    const auto selected = req.get_param_value("receiver");
    const auto index = req.get_param_value("index");
    const char* days[] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
    auto rows = Json::array();
    auto sources = Json::array();
    auto weekdays = Json::array();
    auto fields =
        Json{{"name", ""},      {"start", "00:00"}, {"end", "00:00"},
             {"priority", "1"}, {"center_hz", ""},  {"enabled", true},
             {"mode", ""},      {"dial_hz", ""},    {"slice_center", ""}};
    bool editing = false;
    int day_mask = 127;
    for (const auto& receiver : settings.topology.at("receivers")) {
      const auto name = receiver.at("receiver").get<std::string>();
      const auto escaped = Escape(name.c_str());
      sources.push_back(escaped);
      const auto schedules = receiver.value("schedules", Json::array());
      if (selected == name && index.empty()) {
        editing = true;
        fields["center_hz"] = DisplayField(receiver, "center_hz");
      }
      bool override_shown = false;
      for (size_t i = 0; i < schedules.size(); ++i) {
        const auto& schedule = schedules.at(i);
        const int mask = schedule.value("days", 127);
        std::string day_list;
        for (int day = 0; day < 7; ++day) {
          if (!(mask & (1 << day))) continue;
          if (!day_list.empty()) day_list += ", ";
          day_list += days[day];
        }
        auto name_label = DisplayField(schedule, "name");
        if (name_label.empty())
          name_label = "Schedule " + std::to_string(i + 1);
        const auto start = UtcMinute(schedule.at("start_minute"));
        const auto end = UtcMinute(schedule.at("end_minute"));
        const bool enabled = schedule.value("enabled", true);
        const bool can_run = receiver.value("enabled", true);
        const bool overridden = can_run && !override_shown &&
                                overrides.contains(name) &&
                                overrides.at(name).at("schedule") == schedule;
        override_shown = override_shown || overridden;
        rows.push_back(
            {{"receiver", escaped},
             {"index", i},
             {"name", name_label},
             {"start", start},
             {"end", end},
             {"days", mask == 127 ? "Daily" : day_list},
             {"priority", schedule.at("priority")},
             {"center_hz", schedule.at("center_hz")},
             {"enabled", enabled},
             {"can_run", can_run},
             {"overridden", overridden},
             {"slice_count", schedule.value("slices", Json::array()).size()}});
        if (selected == name && index == std::to_string(i)) {
          editing = true;
          fields["name"] = DisplayField(schedule, "name");
          fields["start"] = start;
          fields["end"] = end;
          fields["center_hz"] = DisplayField(schedule, "center_hz");
          fields["priority"] = DisplayField(schedule, "priority");
          fields["enabled"] = enabled;
          day_mask = mask;
        }
      }
    }
    if (!selected.empty() && !editing && error.empty()) res.status = 404;
    const bool submitted =
        req.method == "POST" && req.get_param_value("action") == "save";
    if (submitted) {
      for (auto key : {"name", "start", "end", "center_hz", "priority", "mode",
                       "dial_hz", "slice_center"})
        fields[key] = Escape(req.get_param_value(key).c_str());
      fields["enabled"] = req.has_param("enabled");
    }
    for (int day = 0; day < 7; ++day) {
      const auto key = "day" + std::to_string(day);
      const bool checked = submitted ? req.has_param(key)
                                     : static_cast<bool>(day_mask & (1 << day));
      weekdays.push_back(
          {{"key", key}, {"label", days[day]}, {"checked", checked}});
    }
    auto revision = submitted ? Escape(req.get_param_value("revision").c_str())
                              : settings.revision;
    Json model = {
        {"admin", true},
        {"empty", false},
        {"csrf", token.str()},
        {"revision", std::move(revision)},
        {"schedule_rows", std::move(rows)},
        {"sources", std::move(sources)},
        {"selected", Escape(selected.c_str())},
        {"manage_slices_url", Escape(EditReceiverUrl(selected).c_str())},
        {"schedule_index", Escape(index.c_str())},
        {"editing_schedule", editing},
        {"schedule_error",
         res.status == 404 ? "Schedule not found." : Escape(error.c_str())},
        {"schedule_fields", std::move(fields)},
        {"weekdays", std::move(weekdays)},
        {"receiver_modes", {"CW", "RTTY", "FT8", "FT4", "WSPR", "SSTV"}}};
    render(*FindPage("band-schedules"), model, res);
  };
  server.Get("/band-schedules",
             [&](const auto& req, auto& res) { render_schedules(req, res); });
  server.Post("/band-schedules", [&](const auto& req, auto& res) {
    if (!validate_form(req, res)) return;
    try {
      const auto action = req.get_param_value("action");
      const auto receiver = req.get_param_value("receiver");
      const auto revision = req.get_param_value("revision");
      const auto index = FormInteger(req, "index", 0, kMaxIndex, true);
      std::lock_guard<std::mutex> lock(config_writes);
      if (action == "run" || action == "clear") {
        const auto settings = LoadSettings(config);
        if (settings.revision != revision)
          throw std::runtime_error("configuration changed; reload and retry");
        if (action == "clear") {
          SetScheduleOverride(database, receiver, Json(), 0);
        } else {
          const auto& sources = settings.topology.at("receivers");
          const auto found = std::find_if(
              sources.begin(), sources.end(), [&](const Json& source) {
                return source.at("receiver") == receiver;
              });
          if (found == sources.end() || !found->value("enabled", true))
            throw std::runtime_error(
                "enable the receiver before running a schedule");
          auto schedules = found->value("schedules", Json::array());
          if (!index || static_cast<size_t>(*index) >= schedules.size())
            throw std::runtime_error("select a valid schedule");
          SetScheduleOverride(database, receiver, schedules.at(*index), 30);
        }
      } else {
        std::optional<SmScheduleSettings> schedule;
        if (action == "save") {
          int days = 0;
          for (int i = 0; i < 7; ++i)
            if (req.has_param("day" + std::to_string(i))) days |= 1 << i;
          auto name = req.get_param_value("name");
          if (name.size() > 128 ||
              name.find_first_of("\r\n") != std::string::npos)
            throw std::runtime_error("invalid schedule name");
          schedule = SmScheduleSettings{
              name,
              FormTime(req, "start"),
              FormTime(req, "end"),
              static_cast<int>(*FormInteger(req, "priority", INT_MIN, INT_MAX)),
              *FormInteger(req, "center_hz", 1, kMaxFrequency),
              days,
              req.has_param("enabled"),
              FormSlice(req)};
        } else if (action != "delete" && action != "duplicate") {
          throw std::runtime_error("invalid schedule action");
        }
        UpdateSchedule(config, revision, receiver, index, action == "duplicate",
                       schedule);
      }
      res.set_redirect("/band-schedules", 303);
    } catch (const std::exception& error) {
      res.status = 400;
      render_schedules(req, res, error.what());
    }
  });
  server.Post("/service", [&](const auto& req, auto& res) {
    if (!validate_form(req, res)) return;
    try {
      SmReportingServiceSettings service;
      service.service = req.get_param_value("service");
      service.enabled = req.has_param("enabled");
      auto callsign = req.get_param_value("callsign");
      auto grid = req.get_param_value("grid");
      std::transform(callsign.begin(), callsign.end(), callsign.begin(),
                     [](unsigned char c) { return std::toupper(c); });
      std::transform(grid.begin(), grid.end(), grid.begin(),
                     [](unsigned char c) { return std::toupper(c); });
      if (!callsign.empty() &&
          !std::regex_match(callsign, std::regex("[A-Z0-9/-]{2,16}")))
        throw std::runtime_error("enter a valid service callsign override");
      if (!grid.empty() &&
          !std::regex_match(grid, std::regex("[A-R]{2}[0-9]{2}([A-X]{2})?")))
        throw std::runtime_error("enter a valid service grid override");
      if (!callsign.empty()) service.callsign = callsign;
      if (!grid.empty()) service.grid = grid;
      auto field = [&](const char* key, size_t max_length) {
        auto value = req.get_param_value(key);
        if (value.size() > max_length ||
            value.find_first_of("\r\n\t ") != std::string::npos)
          throw std::runtime_error(std::string("invalid ") + key);
        return value;
      };
      if (service.service == "cluster") {
        auto port = FormInteger(req, "port", 1, 65535, true);
        if (port) service.port = static_cast<int>(*port);
      } else if (service.service == "psk") {
        auto host = field("host", 253);
        if (!host.empty()) service.host = std::move(host);
        auto port = FormInteger(req, "port", 1, 65535, true);
        if (port) service.port = static_cast<int>(*port);
      } else if (service.service == "rbn") {
        auto host = field("host", 253);
        if (!host.empty()) service.host = std::move(host);
        auto port = FormInteger(req, "port", 1, 65535, true);
        auto backup = FormInteger(req, "backup_port", 1, 65535, true);
        if (port) service.port = static_cast<int>(*port);
        if (backup) service.backup_port = static_cast<int>(*backup);
        auto sign_in = req.get_param_value("sign_in");
        auto band_limits = req.get_param_value("band_limits");
        if (sign_in.size() > 254 ||
            sign_in.find_first_of("\r\n") != std::string::npos)
          throw std::runtime_error("invalid RBN sign-in");
        if (band_limits.size() > 512 ||
            band_limits.find_first_of("\r\n") != std::string::npos)
          throw std::runtime_error("invalid RBN band limits");
        if (!sign_in.empty()) service.sign_in = std::move(sign_in);
        if (!band_limits.empty()) service.band_limits = std::move(band_limits);
      } else if (service.service == "wsprnet") {
        auto endpoint = req.get_param_value("endpoint");
        if (endpoint.size() > 512 ||
            endpoint.find_first_of("\r\n\t ") != std::string::npos ||
            (!endpoint.empty() && endpoint.rfind("http://", 0) != 0 &&
             endpoint.rfind("https://", 0) != 0))
          throw std::runtime_error("enter a valid WSPRnet HTTP endpoint");
        if (!endpoint.empty()) service.endpoint = std::move(endpoint);
      } else {
        throw std::runtime_error("select a valid reporting service");
      }
      std::lock_guard<std::mutex> lock(config_writes);
      UpdateReportingService(config, req.get_param_value("revision"), service);
      const std::string page = service.service == "cluster" ? "/dx-cluster"
                               : service.service == "rbn"   ? "/rbn"
                                                            : "/psk-reporter";
      res.set_redirect(page + "?saved=1", 303);
    } catch (const std::exception& error) {
      res.status = 400;
      const auto name = req.get_param_value("service");
      const std::string page = name == "cluster" ? "dx-cluster"
                               : name == "rbn"   ? "rbn"
                                                 : "psk-reporter";
      render_services(page, req, res, error.what());
    }
  });
  server.Post("/service-restart", [&](const auto& req, auto& res) {
    if (!validate_form(req, res)) return;
    try {
      const auto service = req.get_param_value("service");
      const auto settings = LoadSettings(config);
      const auto options = settings.services.find(service);
      if (options == settings.services.end() || !options->is_object() ||
          !options->value("enabled", false))
        throw std::runtime_error("enable this service before restarting it");
      RestartServiceWorker(database, service);
      const std::string page = service == "cluster" ? "/dx-cluster"
                               : service == "rbn"   ? "/rbn"
                                                    : "/psk-reporter";
      res.set_redirect(page + "?restarted=1", 303);
    } catch (const std::exception& error) {
      res.status = 400;
      res.set_content(error.what(), "text/plain; charset=utf-8");
    }
  });
  server.Get(
      R"(/(settings|general-settings|backup-restore|restart-daemon|wizard|wizard\.html))",
      [&](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Cache-Control", "no-store");
        if (req.path == "/wizard" || req.path == "/wizard.html") {
          render_wizard(req, res);
          return;
        }
        auto settings = LoadSettings(config);
        Json model = {{"admin", true},
                      {"gallery", false},
                      {"csrf", token.str()},
                      {"revision", settings.revision},
                      {"config", Escape(settings.source.c_str())},
                      {"empty", false}};
        render(*FindPage(PanelSlug(req.path)), model, res);
      });
  server.Post(R"(/(settings|wizard|restart))", [&](const httplib::Request& req,
                                                   httplib::Response& res) {
    if (!validate_form(req, res)) return;
    try {
      if (req.path == "/restart")
        RestartStation(database);
      else {
        std::lock_guard<std::mutex> lock(config_writes);
        auto revision = req.get_param_value("revision");
        if (req.path == "/wizard") {
          SmStationIdentity identity{
              req.get_param_value("callsign"), req.get_param_value("email"),
              req.get_param_value("name"), req.get_param_value("grid")};
          for (char& ch : identity.callsign)
            ch =
                static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
          auto mode = req.get_param_value("mode");
          SaveWizard(
              config, revision, identity, req.get_param_value("receiver"),
              req.get_param_value("device"),
              static_cast<int>(*FormInteger(req, "rate", 12000, 6144000)),
              IsSkimmer(mode) ? std::nullopt
                              : FormInteger(req, "dial_hz", 1, 1000000000000LL),
              mode, FormInteger(req, "center_hz", 1, 1000000000000LL, true),
              FormInteger(req, "utc_ms", 0, 100000000000000LL, true),
              req.get_param_value("repeat") == "on");
        } else {
          SaveSettings(config, req.get_param_value("config"), revision);
        }
      }
      res.set_redirect("/receivers", 303);
    } catch (const std::exception& e) {
      res.status = 400;
      if (req.path == "/wizard")
        render_wizard(req, res, e.what());
      else
        res.set_content(e.what(), "text/plain");
    }
  });
  server.Post("/backup", [&](const httplib::Request& req,
                             httplib::Response& res) {
    if (!validate_form(req, res)) return;
    try {
      auto archive = TemporarySnapshot(database, gallery);
      struct SmDownload {
        std::shared_ptr<const std::filesystem::path> archive;
        std::ifstream stream;
      };
      auto download = std::make_shared<SmDownload>(
          SmDownload{archive, std::ifstream(*archive, std::ios::binary)});
      if (!download->stream)
        throw std::runtime_error("cannot open snapshot for download");
      res.set_header("Content-Disposition",
                     "attachment; filename=station-snapshot.zip");
      res.set_content_provider(
          std::filesystem::file_size(*archive), "application/zip",
          [download](size_t offset, size_t length, httplib::DataSink& sink) {
            auto& stream = download->stream;
            stream.clear();
            stream.seekg(offset);
            std::array<char, 65536> buffer;
            while (length) {
              auto count = std::min(length, buffer.size());
              if (!stream.read(buffer.data(), count) ||
                  !sink.write(buffer.data(), count))
                return false;
              length -= count;
            }
            return true;
          });
    } catch (const std::exception& error) {
      res.status = 500;
      res.set_content(error.what(), "text/plain");
    }
  });
  server.Get(R"(/data/(spectrum|map))", [&](const httplib::Request& req,
                                            httplib::Response& res) {
    Json rows;
    if (req.path == "/data/map") {
      rows =
          Query(database,
                "SELECT grid,count(*),max(utc_ms) FROM spots WHERE grid IS NOT "
                "NULL GROUP BY grid ORDER BY max(utc_ms) DESC LIMIT 1000",
                {}, false);
    } else {
      rows = Query(
          database,
          "SELECT receiver,utc,center_hz,rate,bins FROM spectra WHERE "
          "receiver=coalesce(nullif(?1,''),(SELECT receiver FROM spectra "
          "ORDER BY utc DESC,receiver LIMIT 1)) ORDER BY utc DESC LIMIT 80",
          {req.get_param_value("receiver")}, false);
    }
    res.set_content(rows.dump(), "application/json");
  });
  server.Get("/wizard.js", [](auto&, auto& res) {
    res.set_content(kWizardScript, "text/javascript");
  });
  server.Get("/receivers.js", [](auto&, auto& res) {
    res.set_content(kReceiversScript, "text/javascript");
  });
  server.Get("/chrome.js", [](auto&, auto& res) {
    res.set_content(kChrome, "text/javascript");
  });
  server.Get("/charts.js", [](auto&, auto& res) {
    res.set_content(kCharts, "text/javascript");
  });
  server.Get("/style.css",
             [](auto&, auto& res) { res.set_content(kStyle, "text/css"); });
  std::string image_pattern =
      R"(([0-9]+-(?:Martin[12]|Scottie[12]|Robot36|Robot72)\.png))";
  std::regex filename(image_pattern);
  server.Get("/image/" + image_pattern, [&](const httplib::Request& req,
                                            httplib::Response& res) {
    auto path = std::filesystem::path(gallery) / req.matches[1].str();
    if (!std::filesystem::is_regular_file(
            std::filesystem::symlink_status(path))) {
      res.status = 404;
      return;
    }
    res.set_file_content(path.string(), "image/png");
  });
  server.Post("/gallery-delete", [&](const httplib::Request& req,
                                     httplib::Response& res) {
    if (!validate_form(req, res)) return;
    try {
      const auto name = req.get_param_value("filename");
      if (!std::regex_match(name, filename))
        throw std::runtime_error("select a valid SSTV image");
      const auto path = std::filesystem::path(gallery) / name;
      std::error_code status_error;
      const auto status = std::filesystem::symlink_status(path, status_error);
      if (status_error || status.type() != std::filesystem::file_type::regular)
        throw std::runtime_error(
            "SSTV image is missing or is not a regular file");
      std::error_code remove_error;
      if (!std::filesystem::remove(path, remove_error) || remove_error)
        throw std::runtime_error("could not delete SSTV image");

      std::string target = "/sstv-gallery?deleted=1";
      auto preserve = [&](const char* key) {
        const auto value = req.get_param_value(key);
        if (!value.empty())
          target += std::string("&") + key + "=" +
                    httplib::detail::encode_query_param(value);
      };
      preserve("mode");
      preserve("from");
      preserve("to");
      res.set_redirect(target, 303);
    } catch (const std::exception& error) {
      res.status = 400;
      res.set_content(error.what(), "text/plain; charset=utf-8");
    }
  });
  server.Get(R"(/([a-z0-9-]*|index\.html))", [&](const httplib::Request& req,
                                                 httplib::Response& res) {
    auto slug = PanelSlug(req.path);
    const auto* page = FindPage(slug);
    if (!page) {
      res.status = 404;
      return;
    }
    Json model;
    SmSettingsSnapshot settings_view;
    const bool read_only_station_page = req.path == "/" ||
                                        slug == "system-summary" ||
                                        slug == "receiver-status";
    if (read_only_station_page) settings_view = settings_cache.Read();
    if (req.path == "/") {
      if (settings_view.error.empty() && settings_view.settings &&
          settings_view.settings->first_run_wizard &&
          settings_view.instance->receivers().empty()) {
        res.set_redirect("/wizard", 302);
        return;
      }
    }
    model["admin"] = false;
    model["gallery"] = slug == "sstv-gallery";
    if (read_only_station_page) {
      const auto diagnostic =
          settings_view.error.empty()
              ? std::string()
              : "Station TOML error: " + settings_view.error;
      model["config_error"] = Escape(diagnostic.c_str());
      model["config_recovery_guidance"] =
          settings_view.error.empty() ? ""
          : settings_view.settings
              ? "Showing the last valid station settings. Repair the "
                "TOML file on the server; valid changes will be picked "
                "up automatically."
              : "No valid settings snapshot is available. Runtime "
                "data is shown where possible. Repair the station TOML "
                "file on the server.";
    }
    if (slug == "sstv-gallery") {
      struct SmImageRecord {
        int64_t timestamp_us;
        std::string name;
        std::string mode;
        std::uintmax_t bytes;
        std::string captured;
      };
      const auto selected_mode = req.get_param_value("mode");
      const auto from_text = req.get_param_value("from");
      const auto to_text = req.get_param_value("to");
      const bool valid_mode =
          selected_mode.empty() || selected_mode == "Martin1" ||
          selected_mode == "Martin2" || selected_mode == "Scottie1" ||
          selected_mode == "Scottie2" || selected_mode == "Robot36" ||
          selected_mode == "Robot72";
      std::optional<int64_t> from_us, to_us;
      std::string gallery_error;
      if (!valid_mode) {
        res.status = 400;
        gallery_error = "Select a valid SSTV mode.";
      }
      try {
        from_us = GalleryDateStart(from_text, "start");
        to_us = GalleryDateStart(to_text, "end");
        if (to_us) *to_us += 86400LL * 1000000LL;
        if (from_us && to_us && *from_us >= *to_us)
          throw std::runtime_error("start date must not be after end date");
      } catch (const std::exception& error) {
        res.status = 400;
        gallery_error = error.what();
      }
      std::vector<SmImageRecord> records;
      for (const auto& entry : std::filesystem::directory_iterator(gallery)) {
        auto name = entry.path().filename().string();
        if (!gallery_error.empty() ||
            !std::filesystem::is_regular_file(entry.symlink_status()) ||
            !std::regex_match(name, filename))
          continue;
        const auto dash = name.find('-');
        int64_t timestamp_us = 0;
        const auto parsed =
            std::from_chars(name.data(), name.data() + dash, timestamp_us);
        if (parsed.ec != std::errc{} || parsed.ptr != name.data() + dash ||
            (from_us && timestamp_us < *from_us) ||
            (to_us && timestamp_us >= *to_us))
          continue;
        auto mode = name.substr(dash + 1, name.size() - dash - 5);
        if (!selected_mode.empty() && mode != selected_mode) continue;
        std::error_code size_error;
        const auto bytes = std::filesystem::file_size(entry.path(), size_error);
        if (size_error) continue;
        std::string captured;
        try {
          captured = GalleryUtc(timestamp_us);
        } catch (const std::runtime_error&) {
          continue;
        }
        records.push_back({timestamp_us, std::move(name), std::move(mode),
                           bytes, std::move(captured)});
      }
      std::sort(records.begin(), records.end(),
                [](const auto& left, const auto& right) {
                  return left.timestamp_us != right.timestamp_us
                             ? left.timestamp_us > right.timestamp_us
                             : left.name > right.name;
                });
      auto gallery_rows = Json::array();
      auto csv_rows = Json::array();
      for (const auto& record : records) {
        csv_rows.push_back({record.name, record.mode, record.captured,
                            std::to_string(record.bytes)});
        if (gallery_rows.size() < 100)
          gallery_rows.push_back({{"name", record.name},
                                  {"mode", record.mode},
                                  {"captured", record.captured},
                                  {"bytes", record.bytes}});
      }
      model["gallery_columns"] = {"Filename", "Mode", "Captured UTC", "Bytes"};
      model["gallery_rows"] = std::move(gallery_rows);
      model["gallery_empty"] = records.empty();
      model["gallery_mode"] = Escape(selected_mode.c_str());
      model["gallery_from"] = Escape(from_text.c_str());
      model["gallery_to"] = Escape(to_text.c_str());
      model["gallery_error"] = Escape(gallery_error.c_str());
      model["gallery_notice"] =
          req.get_param_value("deleted") == "1" ? "SSTV image deleted." : "";
      model["csrf"] = token.str();
      if (req.get_param_value("format") == "csv") {
        if (!gallery_error.empty()) {
          res.set_content(gallery_error, "text/plain; charset=utf-8");
          return;
        }
        res.set_header("Content-Disposition",
                       "attachment; filename=sstv-gallery.csv");
        res.set_content(CsvDocument(model.at("gallery_columns"), csv_rows),
                        "text/csv; charset=utf-8");
        return;
      }
      model["empty"] = false;
    } else {
      const char* sql;
      if (slug == "spectrum-waterfall" || slug == "live-dx-map") {
        model["spectrum_receivers"] =
            Query(database,
                  "SELECT DISTINCT receiver FROM spectra ORDER BY receiver");
        model["selected_receiver"] =
            Escape(req.get_param_value("receiver").c_str());
        model["chart"] = slug == "spectrum-waterfall" ? "spectrum" : "map";
        model["columns"] = Json::array();
        model["rows"] = Json::array();
        model["empty"] = false;
        render(*page, model, res);
        return;
      } else if (slug == "system-summary") {
        model["callsign"] = "—";
        model["grid"] = "—";
        std::optional<SmStationIdentity> configured;
        if (settings_view.settings)
          configured = settings_view.settings->identity;
        if (configured) {
          model["callsign"] = Escape(configured->callsign.c_str());
          model["grid"] = Escape(configured->grid.c_str());
        } else {
          auto identity =
              Query(database,
                    "SELECT json_extract(plan,'$.options.callsign'),"
                    "json_extract(plan,'$.options.grid') FROM workers "
                    "WHERE id LIKE '@%' ORDER BY id LIMIT 1");
          if (!identity.empty()) {
            model["callsign"] = identity[0][0];
            model["grid"] = identity[0][1];
          }
        }
        model["receiver_status"] =
            Query(database,
                  "SELECT w.id,json_extract(w.plan,'$.device'),"
                  "printf('%.3f MHz',json_extract(w.plan,'$.center_hz')/1e6),"
                  "printf('%g kHz',json_extract(w.plan,'$.rate')/1e3),"
                  "json_array_length(w.plan,'$.slices'),"
                  "'—',w.status "
                  "FROM workers w WHERE w.id NOT LIKE '@%' ORDER BY w.id");
        model["iq_throughput"] = Query(
            database,
            "SELECT printf('%.3f "
            "MSPS',coalesce(sum(json_extract(plan,'$.rate')),0)/1e6) "
            "FROM workers WHERE id NOT LIKE '@%' AND status='Worker launched'")
            [0][0];
        model["decoder_activity"] = Query(
            database,
            "WITH modes(mode) AS "
            "(VALUES('CW'),('RTTY'),('FT8'),('FT4'),('WSPR')), "
            "counts AS (SELECT mode,count(*) total,count(DISTINCT callsign) "
            "calls,"
            "count(*) FILTER(WHERE utc_ms >= (unixepoch()-3600)*1000) recent "
            "FROM spots WHERE utc_ms >= (unixepoch()-86400)*1000 GROUP BY "
            "mode) "
            "SELECT m.mode,(SELECT count(*) FROM workers "
            "w,json_each(w.plan,'$.slices') s "
            "WHERE w.status='Worker launched' AND "
            "json_extract(s.value,'$.mode')=m.mode),"
            "coalesce(c.recent,0),coalesce(c.total,0),coalesce(c.calls,0) "
            "FROM modes m LEFT JOIN counts c ON m.mode=c.mode");
        model["feeds"] = Query(
            database,
            "SELECT coalesce((SELECT status FROM workers WHERE "
            "id='@cluster'),'Disabled'),"
            "coalesce((SELECT status FROM workers WHERE id='@rbn'),'Disabled'),"
            "coalesce((SELECT status FROM workers WHERE id='@psk'),'Disabled'),"
            "coalesce((SELECT status FROM workers WHERE "
            "id='@wsprnet'),'Disabled')")[0];
        model["table_title"] = "Recent Decoded Spots (Tail 5)";
        model["columns"] = {"UTC",  "Receiver",    "Freq (kHz)",
                            "Mode", "DX Callsign", "SNR / Info"};
        sql =
            "SELECT strftime('%H:%M:%S',utc_ms/1000,'unixepoch'),receiver,"
            "printf('%.2f',frequency_hz/1000.),mode,callsign,message "
            "FROM spots ORDER BY utc_ms DESC,rowid DESC LIMIT 5";
      } else if (slug == "dx-cluster" || slug == "psk-reporter" ||
                 slug == "rbn") {
        render_services(slug, req, res);
        return;
      } else if (slug == "slice-allocation" || slug == "cw-skimmer" ||
                 slug == "rtty-skimmer" || slug == "digital-decoders") {
        model["columns"] = {"Receiver", "Mode", "Slice Center (Hz)", "Status"};
        const char* allocation =
            "SELECT "
            "w.id,json_extract(s.value,'$.mode'),"
            "CASE WHEN json_extract(s.value,'$.mode') IN ('CW','RTTY') THEN "
            "coalesce(json_extract(s.value,'$.center_hz'),json_extract(s.value,"
            "'$.dial_hz'),json_extract(w.plan,'$.center_hz')) "
            "WHEN json_extract(s.value,'$.mode') IN ('FT8','FT4','WSPR') THEN "
            "json_extract(s.value,'$.dial_hz')+1500 ELSE "
            "json_extract(s.value,'$.dial_hz') END,"
            "w.status FROM workers w,json_each(w.plan,'$.slices') s "
            "WHERE w.id NOT LIKE '@%' AND w.status='Worker launched' ";
        std::string query = allocation;
        if (slug == "cw-skimmer")
          query += "AND json_extract(s.value,'$.mode')='CW' ";
        if (slug == "rtty-skimmer")
          query += "AND json_extract(s.value,'$.mode')='RTTY' ";
        if (slug == "digital-decoders")
          query +=
              "AND json_extract(s.value,'$.mode') IN ('FT8','FT4','WSPR') ";
        query += "ORDER BY w.id,s.key";
        if (slug == "slice-allocation") {
          model["rows"] = Query(database, query.c_str());
          model["empty"] = model["rows"].empty();
          auto sources = Json::array();
          const auto settings = LoadSettings(config);
          for (const auto& receiver : settings.topology.at("receivers"))
            sources.push_back(DisplayField(receiver, "receiver"));
          model["allocation_sources"] = std::move(sources);
          render(*page, model, res);
          return;
        }

        const auto settings = LoadSettings(config);
        std::map<std::string, std::string> receiver_options;
        for (const auto& receiver : settings.topology.at("receivers"))
          receiver_options.emplace(receiver.at("receiver").get<std::string>(),
                                   DisplayField(receiver, "receiver"));
        for (const auto& row : Query(database,
                                     "SELECT DISTINCT receiver FROM spots "
                                     "ORDER BY receiver LIMIT 200",
                                     {}, false)) {
          const auto& name = row.at(0).get_ref<const std::string&>();
          receiver_options.emplace(name, Escape(name.c_str()));
        }
        auto sources = Json::array();
        for (const auto& [name, label] : receiver_options) {
          (void)name;
          sources.push_back(label);
        }
        model["allocation_sources"] = std::move(sources);
        model["allocation_rows"] = Json::array();
        auto allocations = Query(database, query.c_str(), {}, false);
        for (const auto& row : allocations) {
          const auto& receiver = row.at(0).get_ref<const std::string&>();
          model["allocation_rows"].push_back(
              {{"receiver", Escape(receiver.c_str())},
               {"mode",
                Escape(row.at(1).get_ref<const std::string&>().c_str())},
               {"frequency",
                Escape(row.at(2).get_ref<const std::string&>().c_str())},
               {"status",
                Escape(row.at(3).get_ref<const std::string&>().c_str())},
               {"manage_url", EditReceiverUrl(receiver)}});
        }
        model["allocation_empty"] = model["allocation_rows"].empty();

        const auto receiver = req.get_param_value("receiver");
        auto selected_mode = req.get_param_value("mode");
        const bool digital = slug == "digital-decoders";
        const std::string fixed_mode = slug == "cw-skimmer"     ? "CW"
                                       : slug == "rtty-skimmer" ? "RTTY"
                                                                : "";
        if (!digital) selected_mode = fixed_mode;
        if (digital && selected_mode != "" && selected_mode != "FT8" &&
            selected_mode != "FT4" && selected_mode != "WSPR")
          selected_mode.clear();
        model["columns"] = {"UTC",      "Receiver", "Mode",   "RF (Hz)",
                            "Callsign", "Grid",     "Message"};
        model["table_title"] = "Recent Stored Decoder Results (up to 200)";
        model["selected_receiver"] = Escape(receiver.c_str());
        model["selected_mode"] = Escape(selected_mode.c_str());
        model["decoder_mode_filter"] = digital;
        model["paused"] = req.get_param_value("pause") == "on";
        model["decoder_filters"] = true;
        model["empty"] = false;
        std::string mode_predicate = digital ? "(?2='' OR mode=?2) AND mode IN "
                                               "('FT8','FT4','WSPR')"
                                             : "mode=?2";
        const std::string recent_sql =
            "SELECT datetime(utc_ms/1000,'unixepoch'),receiver,mode,"
            "frequency_hz,coalesce(callsign,''),coalesce(grid,''),message "
            "FROM spots WHERE (?1='' OR receiver=?1) AND " +
            mode_predicate + " ORDER BY utc_ms DESC,rowid DESC LIMIT 200";
        const bool export_csv = req.get_param_value("format") == "csv";
        model["recent_rows"] = Query(database, recent_sql.c_str(),
                                     {receiver, selected_mode}, !export_csv);
        model["recent_empty"] = model["recent_rows"].empty();
        if (export_csv) {
          res.set_header("Content-Disposition",
                         "attachment; filename=" + slug + ".csv");
          res.set_content(
              CsvDocument(model.at("columns"), model.at("recent_rows")),
              "text/csv; charset=utf-8");
          return;
        }
        if (!model["paused"].get<bool>()) res.set_header("Refresh", "10");
        render(*page, model, res);
        return;
      } else if (slug == "noise-floor") {
        render(*page, model, res);
        return;
      } else if (slug == "receiver-status") {
        model["table_title"] = "IQ Source Pool";
        model["columns"] = {"Receiver",       "Driver / Device",
                            "RF Center (Hz)", "Sample Rate (Hz)",
                            "Gain (dB)",      "Slices",
                            "Last IQ (UTC)",  "State"};
        Json workers = Json::array();
        try {
          workers = Query(
              database,
              "SELECT w.id,w.status,json_extract(w.plan,'$.center_hz'),"
              "coalesce((SELECT group_concat(mode||' '||frequency||' Hz "
              "'||reference,', ') "
              "FROM (SELECT json_extract(s.value,'$.mode') AS mode,"
              "CASE WHEN json_extract(s.value,'$.mode') IN ('CW','RTTY') "
              "THEN coalesce(json_extract(s.value,'$.center_hz'),"
              "json_extract(s.value,'$.dial_hz'),json_extract(w.plan,'$.center_"
              "hz')) "
              "ELSE json_extract(s.value,'$.dial_hz') END AS frequency,"
              "CASE WHEN json_extract(s.value,'$.mode') IN ('CW','RTTY') "
              "THEN 'center' ELSE 'dial' END AS reference "
              "FROM json_each(w.plan,'$.slices') s ORDER BY s.key)), '—'),"
              "coalesce((SELECT datetime(max(utc),'unixepoch') FROM spectra "
              "WHERE receiver=w.id AND "
              "center_hz=json_extract(w.plan,'$.center_hz') "
              "AND rate=json_extract(w.plan,'$.rate')), '—') "
              "FROM workers w WHERE w.id NOT LIKE '@%'");
        } catch (const std::exception&) {
          // Runtime state may not be initialized when no settings snapshot
          // exists.
        }
        std::map<std::string, const Json*> active;
        for (const auto& worker : workers)
          active.emplace(worker[0].get<std::string>(), &worker);
        if (!settings_view.settings) {
          Json runtime = Json::array();
          try {
            runtime = Query(
                database,
                "SELECT w.id,coalesce(json_extract(w.plan,'$.device'),''),"
                "coalesce(printf('%.0f',json_extract(w.plan,'$.center_hz')),'—'"
                "),"
                "coalesce(printf('%.0f',json_extract(w.plan,'$.rate')),'—'),"
                "coalesce(printf('%.1f',json_extract(w.plan,'$.gain_db')),'—'),"
                "coalesce((SELECT count(*) FROM "
                "json_each(w.plan,'$.slices')),0),"
                "coalesce((SELECT datetime(max(utc),'unixepoch') FROM spectra "
                "WHERE receiver=w.id AND "
                "center_hz=json_extract(w.plan,'$.center_hz') AND "
                "rate=json_extract(w.plan,'$.rate')), '—'),w.status "
                "FROM workers w WHERE w.id NOT LIKE '@%' ORDER BY w.id");
          } catch (const std::exception&) {
            // A standalone panel may have no station worker table yet.
          }
          model["rows"] = Json::array();
          for (const auto& worker : runtime) {
            const auto device = worker[1].get<std::string>();
            std::string driver = "Runtime source";
            try {
              driver = DriverFor(device).label;
            } catch (const std::exception&) {
            }
            model["rows"].push_back({worker[0], driver + " · " + device,
                                     worker[2], worker[3], worker[4], worker[5],
                                     worker[6], worker[7]});
          }
          model["empty"] = model["rows"].empty();
          render(*page, model, res);
          return;
        }
        for (const auto* source : settings_view.instance->receivers()) {
          const auto& receiver = source->settings();
          auto name = Escape(source->id().c_str());
          const auto& device = source->device();
          auto found = active.find(name);
          const bool enabled = source->enabled();
          const auto* worker =
              enabled && found != active.end() ? found->second : nullptr;
          auto center = DisplayField(receiver, "center_hz", "—");
          const char* idle = !enabled ? "Disabled"
                             : receiver.contains("slices")
                                 ? "Waiting for worker"
                             : receiver.contains("schedules") ? "Standby"
                                                              : "Unallocated";
          model["rows"].push_back({name,
                                   std::string(source->driver()->label) +
                                       " · " + Escape(device.c_str()),
                                   worker ? (*worker)[2] : Json(center),
                                   source->rate(),
                                   DisplayField(receiver, "gain_db", "—"),
                                   worker ? (*worker)[3] : Json("—"),
                                   worker ? (*worker)[4] : Json("—"),
                                   worker ? (*worker)[1] : Json(idle)});
        }
        if (model["rows"].is_null()) model["rows"] = Json::array();
        model["empty"] = model["rows"].empty();
        render(*page, model, res);
        return;
      } else if (slug == "live-spot-stream" || slug == "heard-stations" ||
                 slug == "system-log" || slug == "band-mode-activity") {
        const auto mode = req.get_param_value("mode");
        const auto receiver = req.get_param_value("receiver");
        const auto prefix = req.get_param_value("call");
        const bool log = slug == "system-log";
        model["filters"] = true;
        model["selected_mode"] = Escape(mode.c_str());
        model["selected_receiver"] = Escape(receiver.c_str());
        model["call_filter"] = Escape(prefix.c_str());
        model["paused"] = req.get_param_value("pause") == "on";
        model["filter_modes"] = {"CW", "RTTY", "FT8", "FT4", "WSPR"};
        std::string query;
        std::vector<std::string> params;
        if (log) {
          model["columns"] = {"UTC", "Component", "Message"};
          query =
              "SELECT datetime(utc,'unixepoch'),component,message FROM "
              "system_log "
              "WHERE (?1='' OR component=?1) ORDER BY rowid DESC LIMIT 500";
          params = {receiver};
        } else {
          const std::string where =
              " FROM spots WHERE (?1='' OR receiver=?1) AND (?2='' OR mode=?2) "
              "AND (?3='' OR upper(substr(callsign,1,length(?3)))=upper(?3)) ";
          params = {receiver, mode, prefix};
          if (slug == "heard-stations") {
            model["columns"] = {"Callsign", "Decodes", "Last heard (UTC)"};
            query =
                "SELECT "
                "callsign,count(*),datetime(max(utc_ms)/1000,'unixepoch')" +
                where +
                "AND callsign IS NOT NULL GROUP BY callsign ORDER BY "
                "max(utc_ms) DESC,callsign LIMIT 500";
          } else if (slug == "band-mode-activity") {
            model["columns"] = {"Receiver", "Mode", "Decodes", "Stations"};
            query = "SELECT receiver,mode,count(*),count(DISTINCT callsign)" +
                    where + "GROUP BY receiver,mode ORDER BY receiver,mode";
          } else {
            model["columns"] = {"UTC", "Receiver", "Mode", "RF (Hz)",
                                "Message"};
            query =
                "SELECT "
                "datetime(utc_ms/"
                "1000,'unixepoch'),receiver,mode,frequency_hz,message" +
                where + "ORDER BY utc_ms DESC,rowid DESC LIMIT 200";
          }
        }
        const bool export_csv = req.get_param_value("format") == "csv";
        model["rows"] = Query(database, query.c_str(), params, !export_csv);
        if (export_csv) {
          res.set_header("Content-Disposition",
                         "attachment; filename=" + slug + ".csv");
          res.set_content(CsvDocument(model.at("columns"), model.at("rows")),
                          "text/csv; charset=utf-8");
          return;
        }
        if (slug == "live-spot-stream" && !model["paused"].get<bool>())
          res.set_header("Refresh", "10");
        model["empty"] = model["rows"].empty();
        render(*page, model, res);
        return;
      } else {
        model["columns"] = {"UTC", "Receiver", "Mode", "RF (Hz)", "Message"};
        sql =
            "SELECT "
            "datetime(utc_ms/"
            "1000,'unixepoch'),receiver,mode,frequency_hz,message FROM "
            "spots "
            "ORDER BY utc_ms DESC,rowid DESC LIMIT 200";
        res.set_header("Refresh", "10");
      }
      model["rows"] = Query(database, sql);
      model["empty"] = model["rows"].empty();
    }
    render(*page, model, res);
  });
  const std::string host(address);
  const auto endpoint =
      (host.find(':') == std::string::npos ? host : "[" + host + "]") + ":" +
      std::to_string(port);
  if (!server.bind_to_port(host, port))
    throw std::runtime_error("could not bind HTTP listener on " + endpoint);
  std::cerr << "[web] Listening on http://" + endpoint + "\n";
  if (!server.listen_after_bind())
    throw std::runtime_error("HTTP listener failed on " + endpoint);
}
}  // namespace skim
