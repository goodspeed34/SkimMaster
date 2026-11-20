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
#include "app/diagnostics.h"
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
  server.Post("/restart-station", [&](const httplib::Request& req,
                                      httplib::Response& res) {
    if (!ctx.ValidateForm(req, res)) return;
    try {
      RequestFullRestart(ctx.database);
      // The listener may move, so do not redirect to this address.
      res.set_content(
          "Station restart requested. The panel will return on the listener "
          "configured in the station TOML.",
          "text/plain; charset=utf-8");
    } catch (const std::exception& error) {
      res.status = 500;
      res.set_content(error.what(), "text/plain; charset=utf-8");
    }
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
  server.Post(
      "/backup", [&](const httplib::Request& req, httplib::Response& res) {
        if (!ctx.ValidateForm(req, res)) return;
        try {
          SendTemporaryFile(res, TemporarySnapshot(ctx.database, ctx.gallery),
                            "station-snapshot.zip");
        } catch (const std::exception& error) {
          res.status = 500;
          res.set_content(error.what(), "text/plain");
        }
      });
  server.Post(
      "/diagnostics", [&](const httplib::Request& req, httplib::Response& res) {
        if (!ctx.ValidateForm(req, res)) return;
        try {
          SendTemporaryFile(
              res, TemporaryDiagnostics(ctx.config, ctx.database, ctx.gallery),
              "skimmaster-diagnostics.zip");
        } catch (const std::exception& error) {
          res.status = 500;
          res.set_content(error.what(), "text/plain");
        }
      });
  server.Post("/restore", [&](const httplib::Request& req,
                              httplib::Response& res,
                              const httplib::ContentReader& reader) {
    res.set_header("Cache-Control", "no-store");
    const auto database = std::filesystem::path(ctx.database);
    const auto pending = PendingRestorePath(database);
    const auto upload = pending.string() + ".upload";
    auto fail = [&](int status, const std::string& message) {
      std::error_code ignored;
      std::filesystem::remove(upload, ignored);
      res.status = status;
      res.set_content(message, "text/plain; charset=utf-8");
    };
    if (!req.is_multipart_form_data())
      return fail(400, "Upload the snapshot as a form file.");
    std::unique_lock<std::mutex> lock(ctx.maintenance, std::try_to_lock);
    if (!lock) return fail(409, "Another restore or reset is in progress.");
    std::string csrf, part;
    std::ofstream out;
    bool forged = false, received = false;
    // The CSRF field precedes the file in the form, so a forged upload is
    // refused before anything is written.
    const bool read = reader(
        [&](const httplib::MultipartFormData& field) {
          part = field.name;
          if (part != "snapshot") return true;
          if (csrf != ctx.token) {
            forged = true;
            return false;
          }
          out.open(upload, std::ios::binary | std::ios::trunc);
          received = static_cast<bool>(out);
          return received;
        },
        [&](const char* data, size_t length) {
          if (part == "csrf") {
            csrf.append(data, length);
            return csrf.size() <= ctx.token.size();
          }
          if (part == "snapshot") out.write(data, length);
          return part != "snapshot" || static_cast<bool>(out);
        });
    out.close();
    if (forged || (!received && csrf != ctx.token))
      return fail(403, "This form expired. Reload the page and try again.");
    if (!read || !received || !out)
      return fail(400, "The snapshot upload did not complete.");
    try {
      if (!SupervisorRunning(ctx))
        throw std::runtime_error(
            "No station supervisor is running. Restore offline with "
            "SkimMasterTools --restore.");
      ValidateSnapshot(upload.c_str());
      std::filesystem::rename(upload, pending);
      RequestFullRestart(ctx.database);
    } catch (const std::exception& error) {
      return fail(400, std::string("Snapshot rejected: ") + error.what());
    }
    res.set_content(
        "Snapshot validated. The station is restarting to restore it; a "
        "snapshot of the current data is kept beside the database. Check the "
        "System Log for the result.",
        "text/plain; charset=utf-8");
  });
  server.Post("/factory-reset", [&](const httplib::Request& req,
                                    httplib::Response& res) {
    if (!ctx.ValidateForm(req, res)) return;
    try {
      if (req.get_param_value("confirm") != "RESET")
        throw std::runtime_error("Type RESET to confirm the factory reset.");
      if (!SupervisorRunning(ctx))
        throw std::runtime_error("No station supervisor is running.");
      std::lock_guard<std::mutex> lock(ctx.maintenance);
      const auto marker = PendingResetPath(std::filesystem::path(ctx.database));
      std::ofstream(marker, std::ios::trunc)
          << Json{{"configuration", req.has_param("configuration")}}.dump();
      RequestFullRestart(ctx.database);
      res.set_content(
          "Factory reset requested. The station saves a snapshot beside the "
          "database, clears its data and restarts.",
          "text/plain; charset=utf-8");
    } catch (const std::exception& error) {
      res.status = 400;
      res.set_content(error.what(), "text/plain; charset=utf-8");
    }
  });
}
}  // namespace web
}  // namespace skim
