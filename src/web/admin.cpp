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

#include "app/backup.h"
#include "app/slice_tuning.h"
#include "app/station.h"
#include "source_offer.h"
#include "web/common.h"
#include "web/panel_assets.h"

namespace skim {
namespace web {
void RenderWizard(SmWebContext& ctx, const httplib::Request& req,
                  httplib::Response& res, const std::string& error) {
  Json model{{"csrf", ctx.token},
             {"source_url", Escape(SKIM_SOURCE_URL)},
             {"error", Escape(error.c_str())},
             {"initial_step", error.empty() ? 0 : 2},
             {"mode", "FT8"},
             {"rate", "192000"},
             {"drivers", DriverOptions()},
             {"probe_driver", ""},
             {"dial_hz", "14074000"},
             {"repeat", true}};
  if (error.empty()) {
    auto settings = LoadSettings(ctx.config);
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
         {"revision", "callsign", "email", "name", "grid", "receiver", "device",
          "rate", "dial_hz", "center_hz", "utc_ms", "mode"})
      model[field] = Escape(req.get_param_value(field).c_str());
    model["repeat"] = req.get_param_value("repeat") == "on";
  }
  res.set_content(inja::render(kWizard, model), "text/html; charset=utf-8");
}

void RegisterAdminRoutes(httplib::Server& server, SmWebContext& ctx) {
  server.Get(
      R"(/(settings|general-settings|backup-restore|restart-daemon|wizard|wizard\.html))",
      [&](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Cache-Control", "no-store");
        if (req.path == "/wizard" || req.path == "/wizard.html") {
          RenderWizard(ctx, req, res);
          return;
        }
        auto settings = LoadSettings(ctx.config);
        Json model = {{"admin", true},
                      {"gallery", false},
                      {"csrf", ctx.token},
                      {"revision", settings.revision},
                      {"config", Escape(settings.source.c_str())},
                      {"empty", false}};
        ctx.Render(*FindPage(PanelSlug(req.path)), model, res);
      });
  server.Post(R"(/(settings|wizard|restart))", [&](const httplib::Request& req,
                                                   httplib::Response& res) {
    if (!ctx.ValidateForm(req, res)) return;
    try {
      if (req.path == "/restart")
        RestartStation(ctx.database);
      else {
        std::lock_guard<std::mutex> lock(ctx.config_writes);
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
              ctx.config, revision, identity, req.get_param_value("receiver"),
              req.get_param_value("device"),
              static_cast<int>(*FormInteger(req, "rate", 12000, 6144000)),
              IsSkimmer(mode) ? std::nullopt
                              : FormInteger(req, "dial_hz", 1, 1000000000000LL),
              mode, FormInteger(req, "center_hz", 1, 1000000000000LL, true),
              FormInteger(req, "utc_ms", 0, 100000000000000LL, true),
              req.get_param_value("repeat") == "on");
        } else {
          SaveSettings(ctx.config, req.get_param_value("config"), revision);
        }
      }
      res.set_redirect("/receivers", 303);
    } catch (const std::exception& e) {
      res.status = 400;
      if (req.path == "/wizard")
        RenderWizard(ctx, req, res, e.what());
      else
        res.set_content(e.what(), "text/plain");
    }
  });
  server.Post("/backup", [&](const httplib::Request& req,
                             httplib::Response& res) {
    if (!ctx.ValidateForm(req, res)) return;
    try {
      auto archive = TemporarySnapshot(ctx.database, ctx.gallery);
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
}
}  // namespace web
}  // namespace skim
