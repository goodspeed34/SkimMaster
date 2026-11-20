#include "report/cluster.h"

#include <sqlite3.h>

#include <boost/asio.hpp>
#include <chrono>
#include <iomanip>
#include <memory>
#include <regex>
#include <sstream>
#include <thread>
#include <vector>

#include "core/schema.h"
#include "source_offer.h"

namespace skim {
namespace {
using Tcp = boost::asio::ip::tcp;
using Error = boost::system::error_code;
bool WouldBlock(const Error& e) {
  return e == boost::asio::error::would_block ||
         e == boost::asio::error::try_again;
}

struct SmClient {
  Tcp::socket socket;
  std::string pending = "Skim Master\r\nPlease enter your call: ";
  std::string input;
  bool logged_in = false;
  bool Service(const std::string& station, const std::string& grid) {
    if (pending.size() > 32768) return false;
    Error error;
    if (!pending.empty()) {
      auto sent = socket.write_some(boost::asio::buffer(pending), error);
      if (error && !WouldBlock(error)) return false;
      pending.erase(0, sent);
    }
    char bytes[512];
    auto count = socket.read_some(boost::asio::buffer(bytes), error);
    if (error) return WouldBlock(error);
    input.append(bytes, count);
    if (input.size() > 2048) return false;
    for (size_t end; (end = input.find('\n')) != std::string::npos;) {
      auto command = input.substr(0, end);
      input.erase(0, end + 1);
      if (!command.empty() && command.back() == '\r') command.pop_back();
      if (!logged_in) {
        if (!std::regex_match(command, std::regex("[A-Za-z0-9/-]{2,16}")))
          return false;
        logged_in = true;
        pending += "Connected to Skim Master\r\n";
      } else {
        for (char& c : command) c = std::toupper(static_cast<unsigned char>(c));
        if (command == "BYE" || command == "QUIT") return false;
        if (command == "SKIMMER/SETT" || command == "SKIMMER/SETTINGS")
          pending += "Callsign=" + station + "\r\nGrid=" + grid + "\r\n";
      }
    }
    return true;
  }
};
}  // namespace
void ServeCluster(const char* database, const char* call, const char* grid,
                  int port) {
  if (!std::regex_match(call, std::regex("[A-Z0-9/-]{2,16}")) ||
      !std::regex_match(grid, std::regex("[A-Ra-r]{2}[0-9]{2}([A-Xa-x]{2})?")))
    throw std::runtime_error("invalid station callsign or locator");
  InitStationSchema(database);
  sqlite3* handle = nullptr;
  int rc = sqlite3_open_v2(database, &handle, SQLITE_OPEN_READONLY, nullptr);
  std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db(handle, sqlite3_close);
  if (rc != SQLITE_OK) throw std::runtime_error(sqlite3_errmsg(handle));
  sqlite3_busy_timeout(handle, 3000);
  auto prepare = [&](const char* sql) {
    sqlite3_stmt* query = nullptr;
    if (sqlite3_prepare_v2(handle, sql, -1, &query, nullptr) != SQLITE_OK)
      throw std::runtime_error(sqlite3_errmsg(handle));
    return std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>(
        query, sqlite3_finalize);
  };
  auto last = prepare("SELECT coalesce(max(rowid),0) FROM spots");
  if (sqlite3_step(last.get()) != SQLITE_ROW)
    throw std::runtime_error(sqlite3_errmsg(handle));
  auto cursor = sqlite3_column_int64(last.get(), 0);
  last.reset();
  auto query = prepare(
      "SELECT "
      "live.spot_id,s.frequency_hz,s.callsign,s.mode,"
      "strftime('%H%M',s.utc_ms/1000,'unixepoch') FROM live_receptions live "
      "JOIN spots s ON s.rowid=live.spot_id WHERE live.spot_id>? "
      "ORDER BY live.spot_id LIMIT 128");
  boost::asio::io_context io;
  Tcp::acceptor listener(io);
  listener.open(Tcp::v4());
#ifndef _WIN32
  listener.set_option(Tcp::acceptor::reuse_address(true));
#endif
  listener.bind(Tcp::endpoint(boost::asio::ip::address_v4::loopback(), port));
  listener.listen();
  listener.non_blocking(true);
  std::vector<SmClient> clients;
  static const std::regex sender_pattern("[A-Z0-9/-]{2,16}");
  static const std::regex mode_pattern("CW|RTTY|FT8|FT4|WSPR");
  for (;;) {
    Error error;
    Tcp::socket socket(io);
    listener.accept(socket, error);
    if (!error && clients.size() < 32) {
      socket.non_blocking(true);
      clients.push_back(SmClient{std::move(socket),
                                 "Skim Master\r\nSource code: " SKIM_SOURCE_URL
                                 "\r\nPlease enter your call: ",
                                 {},
                                 false});
    } else if (error && !WouldBlock(error))
      throw boost::system::system_error(error);
    clients.erase(
        std::remove_if(clients.begin(), clients.end(),
                       [&](auto& c) { return !c.Service(call, grid); }),
        clients.end());
    sqlite3_bind_int64(query.get(), 1, cursor);
    while ((rc = sqlite3_step(query.get())) == SQLITE_ROW) {
      cursor = sqlite3_column_int64(query.get(), 0);
      auto sender = sqlite3_column_text(query.get(), 2);
      const auto mode = sqlite3_column_text(query.get(), 3);
      const auto time = sqlite3_column_text(query.get(), 4);
      const auto frequency = sqlite3_column_int64(query.get(), 1);
      if (!sender || !mode || !time || frequency < 1 ||
          !std::regex_match(reinterpret_cast<const char*>(sender),
                            sender_pattern) ||
          !std::regex_match(reinterpret_cast<const char*>(mode), mode_pattern))
        continue;
      std::ostringstream line;
      line << "DX de " << call << ": " << std::fixed << std::setprecision(1)
           << frequency / 1000.0 << "  " << sender << "  " << mode << "  "
           << time << "Z\r\n";
      for (auto& c : clients)
        if (c.logged_in) c.pending += line.str();
    }
    if (rc != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(handle));
    sqlite3_reset(query.get());
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
}
}  // namespace skim
