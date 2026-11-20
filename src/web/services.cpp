#include <httplib.h>
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

#include "app/station.h"
#include "web/common.h"
#include "web/panel_assets.h"

namespace skim {
namespace web {
void RenderServices(SmWebContext& ctx, const std::string& slug,
                    const httplib::Request& req, httplib::Response& res,
                    const std::string& error) {
  const auto settings = LoadSettings(ctx.config);
  const std::vector<std::string> services =
      slug == "dx-cluster" ? std::vector<std::string>{"cluster"}
      : slug == "rbn"      ? std::vector<std::string>{"rbn"}
                           : std::vector<std::string>{"psk", "wsprnet"};
  const auto submitted = req.method == "POST";
  const auto submitted_service = req.get_param_value("service");
  auto cards = Json::array();
  for (const auto& name : services) {
    const auto found = settings.services.find(name);
    const Json options = found != settings.services.end() && found->is_object()
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
    auto worker = Query(ctx.database, "SELECT status FROM workers WHERE id=?1",
                        {worker_id}, false);
    const auto worker_status =
        !enabled         ? std::string("Disabled")
        : worker.empty() ? std::string("Worker pending")
                         : Escape(worker[0][0].get<std::string>().c_str());
    std::string receipts = "—", last_receipt = "—";
    if (name != "cluster") {
      auto receipt = Query(
          ctx.database,
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
    auto effective_grid = explicit_grid.empty() ? identity.grid : explicit_grid;
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
      const auto endpoint_display = name == "cluster" && submitted_port.empty()
                                        ? std::string("7300")
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
           {"backup_port", Escape(req.get_param_value("backup_port").c_str())},
           {"endpoint", Escape(req.get_param_value("endpoint").c_str())},
           {"sign_in", Escape(req.get_param_value("sign_in").c_str())},
           {"sign_in_fallback", Escape(identity.email.c_str())},
           {"band_limits", Escape(req.get_param_value("band_limits").c_str())},
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
                {"csrf", ctx.token},
                {"revision", settings.revision},
                {"service_cards", std::move(cards)},
                {"service_error", Escape(error.c_str())},
                {"service_notice", ""}};
  if (req.get_param_value("saved") == "1")
    model["service_notice"] = "Service settings saved.";
  else if (req.get_param_value("restarted") == "1")
    model["service_notice"] = "Local service worker restart requested.";
  ctx.Render(*FindPage(slug), std::move(model), res);
}

void RegisterServiceRoutes(httplib::Server& server, SmWebContext& ctx) {
  server.Post("/service", [&](const auto& req, auto& res) {
    if (!ctx.ValidateForm(req, res)) return;
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
      std::lock_guard<std::mutex> lock(ctx.config_writes);
      UpdateReportingService(ctx.config, req.get_param_value("revision"),
                             service);
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
      RenderServices(ctx, page, req, res, error.what());
    }
  });
  server.Post("/service-restart", [&](const auto& req, auto& res) {
    if (!ctx.ValidateForm(req, res)) return;
    try {
      const auto service = req.get_param_value("service");
      const auto settings = LoadSettings(ctx.config);
      const auto options = settings.services.find(service);
      if (options == settings.services.end() || !options->is_object() ||
          !options->value("enabled", false))
        throw std::runtime_error("enable this service before restarting it");
      RestartServiceWorker(ctx.database, service);
      const std::string page = service == "cluster" ? "/dx-cluster"
                               : service == "rbn"   ? "/rbn"
                                                    : "/psk-reporter";
      res.set_redirect(page + "?restarted=1", 303);
    } catch (const std::exception& error) {
      res.status = 400;
      res.set_content(error.what(), "text/plain; charset=utf-8");
    }
  });
}
}  // namespace web
}  // namespace skim
