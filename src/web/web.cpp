#include "web/web.h"

#include <httplib.h>
#include <openssl/rand.h>

#include <array>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>

#include "app/config.h"
#include "app/station_lock.h"
#include "core/schema.h"
#include "web/common.h"
#include "web/panel_assets.h"

namespace skim {
using web::Json;
using web::Query;

void Serve(const char* config) {
  const auto startup = LoadSettings(config);
  Serve(config, startup.address.c_str(), startup.port, startup.database.c_str(),
        startup.gallery.c_str());
}

void Serve(const char* config, const char* address, int port,
           const char* database, const char* gallery) {
  const auto database_path = OperationalDatabasePath(database);
  const auto database_name = database_path.string();
  InitStationSchema(database_name.c_str());
  std::filesystem::create_directories(gallery);
  httplib::Server server;
  server.new_task_queue = [] { return new httplib::ThreadPool(4); };
  // Snapshot uploads stream to disk and may be large. Every other request
  // body is read into memory, so keep those small.
  constexpr size_t kMaxFormBytes = 1024 * 1024;
  constexpr size_t kMaxRecordingBytes = 64 * 1024 * 1024;
  constexpr size_t kMaxUploadBytes = 9ULL << 30;
  server.set_payload_max_length(kMaxUploadBytes);
  server.set_pre_routing_handler(
      [](const httplib::Request& req, httplib::Response& res) {
        if (req.method != "POST" || req.path == "/restore")
          return httplib::Server::HandlerResponse::Unhandled;
        const auto length = req.get_header_value_u64("Content-Length");
        // On-demand decode buffers one recording in memory.
        const size_t limit =
            req.path == "/decode" ? kMaxRecordingBytes : kMaxFormBytes;
        if (req.has_header("Transfer-Encoding") || length > limit) {
          res.status = 413;
          res.set_content("Request body too large.", "text/plain");
          return httplib::Server::HandlerResponse::Handled;
        }
        return httplib::Server::HandlerResponse::Unhandled;
      });
  std::array<unsigned char, 32> random{};
  if (RAND_bytes(random.data(), random.size()) != 1)
    throw std::runtime_error("could not initialize session token");
  std::ostringstream token;
  for (auto byte : random)
    token << std::hex << std::setw(2) << std::setfill('0') << int(byte);
  web::SmWebContext ctx(config, database_name.c_str(), gallery, address, port,
                        token.str());
  web::RegisterReceiverRoutes(server, ctx);
  web::RegisterScheduleRoutes(server, ctx);
  web::RegisterServiceRoutes(server, ctx);
  web::RegisterAdminRoutes(server, ctx);
  web::RegisterDecoderRoutes(server, ctx);
  web::RegisterAnalysisRoutes(server, ctx);
  server.Get(R"(/data/(spectrum|map))", [&](const httplib::Request& req,
                                            httplib::Response& res) {
    Json rows;
    if (req.path == "/data/map") {
      rows =
          Query(ctx.database,
                "SELECT grid,count(*),max(utc_ms) FROM spots WHERE grid IS NOT "
                "NULL GROUP BY grid ORDER BY max(utc_ms) DESC LIMIT 1000",
                {}, false);
    } else {
      res.set_header("Cache-Control", "no-store");
      const auto query =
          std::string("SELECT receiver,utc,center_hz,rate,bins,(") +
          web::kCurrentSpectrumSql +
          "),unixepoch()-utc FROM spectra s "
          "WHERE receiver=coalesce(nullif(?1,''),"
          "(SELECT receiver FROM spectra s ORDER BY (" +
          web::kCurrentSpectrumSql +
          ") DESC,utc DESC,receiver LIMIT 1)) "
          "ORDER BY utc DESC LIMIT 80";
      rows = Query(ctx.database, query.c_str(),
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
  web::RegisterGalleryRoutes(server, ctx);
  web::RegisterPageRoutes(server, ctx);
  const std::string host(address);
  const auto endpoint =
      (host.find(':') == std::string::npos ? host : "[" + host + "]") + ":" +
      std::to_string(port);
  server.set_exception_handler([](const httplib::Request& req,
                                  httplib::Response& res,
                                  std::exception_ptr error) {
    std::string message = "unknown error";
    try {
      std::rethrow_exception(error);
    } catch (const std::exception& e) {
      message = e.what();
    } catch (...) {
    }
    std::cerr << "[web] " << req.method << ' ' << req.path
              << " failed: " << message << '\n';
    res.status = 500;
    res.set_content("Internal error: " + message, "text/plain; charset=utf-8");
  });
  if (!server.bind_to_port(host, port))
    throw std::runtime_error("could not bind HTTP listener on " + endpoint);
  std::cerr << "[web] Listening on http://" + endpoint + "\n";
  if (!server.listen_after_bind())
    throw std::runtime_error("HTTP listener failed on " + endpoint);
}
}  // namespace skim
