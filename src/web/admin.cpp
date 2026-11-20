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
#include "app/station_lock.h"
#include "source_offer.h"
#include "web/common.h"
#include "web/panel_assets.h"

namespace skim {
namespace web {
namespace {
void RenderGeneralSettings(SmWebContext& ctx, const httplib::Request& req,
                           httplib::Response& res,
                           const std::string& error = "",
                           const std::string& field = "") {
  const bool advanced = req.path == "/settings/toml";
  const bool draft = req.method == "POST";
  Json model{{"admin", true},
             {"empty", false},
             {"gallery", false},
             {"csrf", ctx.token},
             {"settings_error", Escape(error.c_str())},
             {"settings_error_field", field},
             {"settings_load_error", ""},
             {"settings_advanced", advanced},
             {"settings_saved", !draft && req.get_param_value("saved") == "1"},
             {"restart_pending", Json::array()},
             {"identity_services", Json::array()},
             {"settings_retry", ""},
             {"revision", ""},
             {"config", ""},
             {"settings_available", false}};
  try {
    // Read the revision first so a draft for recovery can never overwrite a
    // different file that arrived while parsing failed.
    model["revision"] = SettingsRevision(ctx.config);
    const auto settings = LoadSettings(ctx.config);
    model["revision"] = settings.revision;
    model["settings_available"] = true;
    model["config"] = Escape(settings.source.c_str());
    const auto identity = settings.identity.value_or(SmStationIdentity{});
    for (const auto& [key, value] : {std::pair{"callsign", identity.callsign},
                                     {"grid", identity.grid},
                                     {"name", identity.name},
                                     {"email", identity.email},
                                     {"listen_address", settings.address},
                                     {"port", std::to_string(settings.port)},
                                     {"database", settings.database_setting},
                                     {"gallery", settings.gallery_setting}}) {
      const auto shown = draft && !advanced ? req.get_param_value(key) : value;
      model[key] = Escape(shown.c_str());
    }
    model["first_run_wizard"] = draft && !advanced
                                    ? req.has_param("first_run_wizard")
                                    : settings.first_run_wizard;
    if (settings.address != ctx.address || settings.port != ctx.port)
      model["restart_pending"].push_back("web listener");
    if (OperationalDatabasePath(settings.database) != ctx.database)
      model["restart_pending"].push_back("database path");
    if (std::filesystem::path(settings.gallery) != ctx.gallery)
      model["restart_pending"].push_back("gallery path");
    for (const auto& [key, label] : {std::pair{"cluster", "Local DX Cluster"},
                                     {"psk", "PSK Reporter"},
                                     {"wsprnet", "WSPRnet"},
                                     {"rbn", "Reverse Beacon Network"}}) {
      const auto options = settings.services.value(key, Json::object());
      if (!options.is_object() || !options.value("enabled", false)) continue;
      Json fields = Json::array();
      for (const auto& [name, display, fallback] :
           {std::tuple{"callsign", "Callsign", identity.callsign},
            std::tuple{"grid", "Grid", identity.grid},
            std::tuple{"sign_in", "Sign-in", identity.email},
            std::tuple{"name", "Operator name", identity.name}}) {
        if ((std::string_view(name) == "sign_in" ||
             std::string_view(name) == "name") &&
            std::string_view(key) != "rbn")
          continue;
        const auto value = options.value(name, fallback);
        fields.push_back({{"label", display},
                          {"value", Escape(value.c_str())},
                          {"source", options.contains(name) ? "Override"
                                     : value.empty()        ? "Unset"
                                                     : "Station identity"}});
      }
      model["identity_services"].push_back(
          {{"label", label}, {"fields", fields}});
    }
  } catch (const std::exception& e) {
    model["settings_load_error"] = Escape(e.what());
    model["settings_available"] = false;
  }
  if (draft) {
    const auto revision = req.has_param("retry_revision")
                              ? req.get_param_value("retry_revision")
                              : req.get_param_value("revision");
    if (res.status == 409) model["settings_retry"] = model["revision"];
    model["revision"] = Escape(revision.c_str());
    if (advanced)
      model["config"] = Escape(req.get_param_value("config").c_str());
  }
  ctx.Render(*FindPage("general-settings"), std::move(model), res);
}

std::string Trim(std::string value) {
  const auto first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return {};
  return value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
}

void Uppercase(std::string& value) {
  for (auto& ch : value)
    ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
}
}  // namespace

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
      R"(/(settings|general-settings|settings/toml|backup-restore|restart-daemon|wizard|wizard\.html))",
      [&](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Cache-Control", "no-store");
        if (req.path == "/wizard" || req.path == "/wizard.html") {
          RenderWizard(ctx, req, res);
          return;
        }
        if (req.path == "/settings" || req.path == "/general-settings" ||
            req.path == "/settings/toml") {
          RenderGeneralSettings(ctx, req, res);
          return;
        }
        Json model = {{"admin", true},
                      {"gallery", false},
                      {"csrf", ctx.token},
                      {"empty", false}};
        ctx.Render(*FindPage(PanelSlug(req.path)), model, res);
      });
  server.Post(
      R"(/(settings|general-settings|settings/toml))",
      [&](const httplib::Request& req, httplib::Response& res) {
        const bool advanced = req.path == "/settings/toml";
        const bool legacy = req.path == "/settings" && req.has_param("config");
        if (!ctx.ValidateForm(req, res)) {
          RenderGeneralSettings(ctx, req, res,
                                "This form expired. Your entries are still "
                                "here. Review them and save again.");
          return;
        }
        std::string field;
        try {
          std::lock_guard<std::mutex> lock(ctx.config_writes);
          const auto revision = req.has_param("retry_revision")
                                    ? req.get_param_value("retry_revision")
                                    : req.get_param_value("revision");
          if (SettingsRevision(ctx.config) != revision) {
            res.status = legacy ? 400 : 409;
            throw std::runtime_error(
                "Settings changed since you opened this page. Your draft is "
                "still here. Review the saved settings before applying it.");
          }
          if (advanced || legacy) {
            SaveSettings(ctx.config, req.get_param_value("config"), revision);
          } else {
            SmGeneralSettings settings{};
            settings.identity = {Trim(req.get_param_value("callsign")),
                                 Trim(req.get_param_value("email")),
                                 Trim(req.get_param_value("name")),
                                 Trim(req.get_param_value("grid"))};
            Uppercase(settings.identity.callsign);
            Uppercase(settings.identity.grid);
            field = "listen_address";
            settings.address = Trim(req.get_param_value("listen_address"));
            if (settings.address.empty() ||
                settings.address.find_first_of(" \t\r\n") != std::string::npos)
              throw std::runtime_error("Enter a hostname or an IP address.");
            field = "port";
            settings.port =
                static_cast<int>(*FormInteger(req, "port", 1, 65535));
            field = "database";
            settings.database = req.get_param_value("database");
            if (Trim(settings.database).empty())
              throw std::runtime_error("Enter the database path.");
            field = "gallery";
            settings.gallery = req.get_param_value("gallery");
            if (Trim(settings.gallery).empty())
              throw std::runtime_error("Enter the gallery path.");
            settings.first_run_wizard = req.has_param("first_run_wizard");
            field.clear();
            UpdateGeneralSettings(ctx.config, revision, settings);
          }
          res.set_redirect("/general-settings?saved=1", 303);
        } catch (const std::exception& e) {
          if (res.status != 409) res.status = 400;
          std::string message = e.what();
          if (message == "configuration changed; reload and retry" && !legacy) {
            res.status = 409;
            message =
                "Settings changed while saving. Review the current settings "
                "before applying your draft.";
          }
          if (!advanced && !legacy && message.find("invalid station ") == 0) {
            if (message == "invalid station callsign") {
              field = "callsign";
              message =
                  "Enter a callsign of 2 to 16 characters using letters, "
                  "digits, / or -.";
            } else if (message == "invalid station Maidenhead grid") {
              field = "grid";
              message =
                  "Enter a four- or six-character Maidenhead grid, such as "
                  "FN42 or FN42AB.";
            } else if (message == "invalid station email") {
              field = "email";
              message =
                  "Enter a valid email address of at most 254 characters.";
            } else if (message == "invalid station name") {
              field = "name";
              message = "Enter an operator name of at most 128 bytes.";
            }
          }
          RenderGeneralSettings(ctx, req, res, message, field);
        }
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
  server.Post(R"(/(wizard|restart))", [&](const httplib::Request& req,
                                          httplib::Response& res) {
    if (!ctx.ValidateForm(req, res)) return;
    try {
      if (req.path == "/restart")
        RestartStation(ctx.database);
      else {
        std::lock_guard<std::mutex> lock(ctx.config_writes);
        auto revision = req.get_param_value("revision");
        SmStationIdentity identity{
            req.get_param_value("callsign"), req.get_param_value("email"),
            req.get_param_value("name"), req.get_param_value("grid")};
        Uppercase(identity.callsign);
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
        res.set_redirect(
            "/receivers?setup=" + httplib::detail::encode_query_param(
                                      req.get_param_value("receiver")),
            303);
        return;
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
