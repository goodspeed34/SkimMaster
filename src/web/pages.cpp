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

#include "receiver/receiver.h"
#include "web/common.h"
#include "web/panel_assets.h"

namespace skim {
namespace web {
namespace {
void RenderDecoderPage(SmWebContext& ctx, const std::string& slug,
                       const SmReferencePage& page, const httplib::Request& req,
                       httplib::Response& res, Json model) {
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
  if (slug == "cw-skimmer") query += "AND json_extract(s.value,'$.mode')='CW' ";
  if (slug == "rtty-skimmer")
    query += "AND json_extract(s.value,'$.mode')='RTTY' ";
  if (slug == "digital-decoders")
    query += "AND json_extract(s.value,'$.mode') IN ('FT8','FT4','WSPR') ";
  query += "ORDER BY w.id,s.key";
  if (slug == "slice-allocation") {
    model["rows"] = Query(ctx.database, query.c_str());
    model["empty"] = model["rows"].empty();
    auto sources = Json::array();
    const auto settings = LoadSettings(ctx.config);
    for (const auto& receiver : settings.topology.at("receivers"))
      sources.push_back(DisplayField(receiver, "receiver"));
    model["allocation_sources"] = std::move(sources);
    ctx.Render(page, model, res);
    return;
  }

  const auto settings = LoadSettings(ctx.config);
  std::map<std::string, std::string> receiver_options;
  for (const auto& receiver : settings.topology.at("receivers"))
    receiver_options.emplace(receiver.at("receiver").get<std::string>(),
                             DisplayField(receiver, "receiver"));
  for (const auto& row : Query(ctx.database,
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
  auto allocations = Query(ctx.database, query.c_str(), {}, false);
  for (const auto& row : allocations) {
    const auto& receiver = row.at(0).get_ref<const std::string&>();
    model["allocation_rows"].push_back(
        {{"receiver", Escape(receiver.c_str())},
         {"mode", Escape(row.at(1).get_ref<const std::string&>().c_str())},
         {"frequency", Escape(row.at(2).get_ref<const std::string&>().c_str())},
         {"status", Escape(row.at(3).get_ref<const std::string&>().c_str())},
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
  model["recent_rows"] = Query(ctx.database, recent_sql.c_str(),
                               {receiver, selected_mode}, !export_csv);
  model["recent_empty"] = model["recent_rows"].empty();
  if (export_csv) {
    res.set_header("Content-Disposition",
                   "attachment; filename=" + slug + ".csv");
    res.set_content(CsvDocument(model.at("columns"), model.at("recent_rows")),
                    "text/csv; charset=utf-8");
    return;
  }
  if (!model["paused"].get<bool>()) res.set_header("Refresh", "10");
  ctx.Render(page, model, res);
  return;
}

void RenderReceiverStatus(SmWebContext& ctx, const SmReferencePage& page,
                          const SmSettingsSnapshot& settings_view,
                          httplib::Response& res, Json model) {
  model["table_title"] = "IQ Source Pool";
  model["columns"] = {
      "Receiver",  "Driver / Device", "RF Center (Hz)", "Sample Rate (Hz)",
      "Gain (dB)", "Slices",          "Last IQ (UTC)",  "State"};
  Json workers = Json::array();
  try {
    workers =
        Query(ctx.database,
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
      runtime =
          Query(ctx.database,
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
      model["rows"].push_back({worker[0], driver + " · " + device, worker[2],
                               worker[3], worker[4], worker[5], worker[6],
                               worker[7]});
    }
    model["empty"] = model["rows"].empty();
    ctx.Render(page, model, res);
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
    const char* idle = !enabled                         ? "Disabled"
                       : receiver.contains("slices")    ? "Waiting for worker"
                       : receiver.contains("schedules") ? "Standby"
                                                        : "Unallocated";
    model["rows"].push_back(
        {name,
         std::string(source->driver()->label) + " · " + Escape(device.c_str()),
         worker ? (*worker)[2] : Json(center), source->rate(),
         DisplayField(receiver, "gain_db", "—"),
         worker ? (*worker)[3] : Json("—"), worker ? (*worker)[4] : Json("—"),
         worker ? (*worker)[1] : Json(idle)});
  }
  if (model["rows"].is_null()) model["rows"] = Json::array();
  model["empty"] = model["rows"].empty();
  ctx.Render(page, model, res);
  return;
}

void RenderTablePage(SmWebContext& ctx, const std::string& slug,
                     const SmReferencePage& page, const httplib::Request& req,
                     httplib::Response& res, Json model) {
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
      query = "SELECT receiver,mode,count(*),count(DISTINCT callsign)" + where +
              "GROUP BY receiver,mode ORDER BY receiver,mode";
    } else {
      model["columns"] = {"UTC", "Receiver", "Mode", "RF (Hz)", "Message"};
      query =
          "SELECT "
          "datetime(utc_ms/"
          "1000,'unixepoch'),receiver,mode,frequency_hz,message" +
          where + "ORDER BY utc_ms DESC,rowid DESC LIMIT 200";
    }
  }
  const bool export_csv = req.get_param_value("format") == "csv";
  model["rows"] = Query(ctx.database, query.c_str(), params, !export_csv);
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
  ctx.Render(page, model, res);
  return;
}
}  // namespace

void RegisterPageRoutes(httplib::Server& server, SmWebContext& ctx) {
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
    if (read_only_station_page) settings_view = ctx.settings_cache.Read();
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
      RenderGallery(ctx, *page, req, res, std::move(model));
      return;
    } else {
      const char* sql;
      if (slug == "spectrum-waterfall" || slug == "live-dx-map") {
        model["spectrum_receivers"] =
            Query(ctx.database,
                  "SELECT DISTINCT receiver FROM spectra ORDER BY receiver");
        model["selected_receiver"] =
            Escape(req.get_param_value("receiver").c_str());
        model["chart"] = slug == "spectrum-waterfall" ? "spectrum" : "map";
        model["columns"] = Json::array();
        model["rows"] = Json::array();
        model["empty"] = false;
        ctx.Render(*page, model, res);
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
              Query(ctx.database,
                    "SELECT json_extract(plan,'$.options.callsign'),"
                    "json_extract(plan,'$.options.grid') FROM workers "
                    "WHERE id LIKE '@%' ORDER BY id LIMIT 1");
          if (!identity.empty()) {
            model["callsign"] = identity[0][0];
            model["grid"] = identity[0][1];
          }
        }
        model["receiver_status"] =
            Query(ctx.database,
                  "SELECT w.id,json_extract(w.plan,'$.device'),"
                  "printf('%.3f MHz',json_extract(w.plan,'$.center_hz')/1e6),"
                  "printf('%g kHz',json_extract(w.plan,'$.rate')/1e3),"
                  "json_array_length(w.plan,'$.slices'),"
                  "'—',w.status "
                  "FROM workers w WHERE w.id NOT LIKE '@%' ORDER BY w.id");
        model["iq_throughput"] = Query(
            ctx.database,
            "SELECT printf('%.3f "
            "MSPS',coalesce(sum(json_extract(plan,'$.rate')),0)/1e6) "
            "FROM workers WHERE id NOT LIKE '@%' AND status='Worker launched'")
            [0][0];
        model["decoder_activity"] = Query(
            ctx.database,
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
            ctx.database,
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
        RenderServices(ctx, slug, req, res);
        return;
      } else if (slug == "slice-allocation" || slug == "cw-skimmer" ||
                 slug == "rtty-skimmer" || slug == "digital-decoders") {
        RenderDecoderPage(ctx, slug, *page, req, res, std::move(model));
        return;
      } else if (slug == "noise-floor") {
        ctx.Render(*page, model, res);
        return;
      } else if (slug == "receiver-status") {
        RenderReceiverStatus(ctx, *page, settings_view, res, std::move(model));
        return;
      } else if (slug == "live-spot-stream" || slug == "heard-stations" ||
                 slug == "system-log" || slug == "band-mode-activity") {
        RenderTablePage(ctx, slug, *page, req, res, std::move(model));
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
      model["rows"] = Query(ctx.database, sql);
      model["empty"] = model["rows"].empty();
    }
    ctx.Render(*page, model, res);
  });
}
}  // namespace web
}  // namespace skim
