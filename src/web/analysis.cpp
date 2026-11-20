// Statistics pages: noise-floor analysis.

#include <cmath>
#include <set>
#include <stdexcept>
#include <string>

#include "web/common.h"
#include "web/panel_assets.h"

namespace skim {
namespace web {
namespace {
constexpr char kNoiseBandSql[] =
    "WITH recent AS (SELECT receiver,coalesce(band(center_hz),'Out of band') "
    "AS band,utc,floor_db FROM noise_floor WHERE utc>=unixepoch()-86400 AND "
    "(?1='' OR receiver=?1)),"
    "stats AS (SELECT band,receiver,min(floor_db) low,avg(floor_db) mean,"
    "max(floor_db) high,"
    "avg(floor_db) FILTER(WHERE utc>=unixepoch()-21600)-"
    "avg(floor_db) FILTER(WHERE utc<unixepoch()-21600 AND "
    "utc>=unixepoch()-43200) AS delta,count(*) minutes,"
    "count(*) FILTER(WHERE ?2<>'' AND floor_db>CAST(?2 AS REAL)) above "
    "FROM recent GROUP BY band,receiver) "
    "SELECT band,receiver,printf('%.1f',low),printf('%.1f',mean),"
    "printf('%.1f',high),"
    "CASE WHEN delta IS NULL THEN 'Collecting' WHEN abs(delta)<1 THEN "
    "'Stable' ELSE printf('%+.1f dB / 6h',delta) END,"
    "CASE WHEN delta IS NULL THEN 'status-idle' WHEN delta>=1 THEN "
    "'status-warn' ELSE 'status-ok' END,minutes,above "
    "FROM stats ORDER BY band,receiver";

constexpr char kNoiseHistorySql[] =
    "SELECT receiver,utc/(?2*60)*(?2*60) AS bucket,"
    "round(avg(floor_db),2) FROM noise_floor WHERE utc>=unixepoch()-86400 "
    "AND (?1='' OR receiver=?1) GROUP BY receiver,bucket "
    "ORDER BY receiver,bucket";

std::string Smoothing(const httplib::Request& req) {
  const auto value = req.get_param_value("smoothing");
  return value == "1" || value == "15" ? value : "5";
}

std::string Threshold(const httplib::Request& req) {
  const auto value = FormDecimal(req, "threshold");
  if (!value) return "";
  if (*value < -250 || *value > 50)
    throw std::runtime_error("enter an alert threshold in dBFS/Hz");
  return std::to_string(*value);
}
}  // namespace

const char kNoiseFloorSql[] =
    "coalesce((SELECT printf('%.1f dBFS/Hz',n.floor_db) FROM noise_floor n "
    "WHERE n.receiver=w.id AND n.utc>=unixepoch()-180 ORDER BY n.utc DESC "
    "LIMIT 1),'—')";

void RenderNoiseFloor(SmWebContext& ctx, const SmReferencePage& page,
                      const httplib::Request& req, httplib::Response& res,
                      Json model) {
  const auto receiver = req.get_param_value("receiver");
  const auto smoothing = Smoothing(req);
  std::string threshold, error;
  try {
    threshold = Threshold(req);
  } catch (const std::exception& e) {
    error = e.what();
  }
  if (req.get_param_value("format") == "csv") {
    const Json columns = {"UTC",
                          "Receiver",
                          "Band",
                          "RF Center (Hz)",
                          "Sample Rate (Hz)",
                          "Noise Floor (dBFS/Hz)",
                          "Seconds Averaged"};
    auto rows = Query(ctx.database,
                      "SELECT datetime(utc,'unixepoch'),receiver,"
                      "coalesce(band(center_hz),''),center_hz,rate,"
                      "printf('%.2f',floor_db),samples FROM noise_floor "
                      "WHERE utc>=unixepoch()-86400 AND "
                      "(?1='' OR receiver=?1) ORDER BY utc,receiver",
                      {receiver}, false);
    res.set_header("Content-Disposition",
                   "attachment; filename=noise-floor.csv");
    res.set_content(CsvDocument(columns, rows), "text/csv; charset=utf-8");
    return;
  }
  std::set<std::string> sources;
  for (const auto& row :
       Query(ctx.database,
             "SELECT DISTINCT receiver FROM noise_floor ORDER BY receiver", {},
             false))
    sources.insert(row[0].get<std::string>());
  try {
    for (const auto* source :
         SmInstance::FromSettings(LoadSettings(ctx.config))->receivers())
      sources.insert(source->id());
  } catch (const std::exception&) {
    // Show recorded history even when the TOML cannot be loaded.
  }
  model["noise_sources"] = Json::array();
  for (const auto& source : sources)
    model["noise_sources"].push_back(Escape(source.c_str()));
  model["selected_receiver"] = Escape(receiver.c_str());
  model["smoothing"] = smoothing;
  model["threshold"] = Escape(req.get_param_value("threshold").c_str());
  model["noise_error"] = Escape(error.c_str());
  model["chart"] = "noise";
  model["noise_rows"] =
      Query(ctx.database, kNoiseBandSql, {receiver, threshold});
  model["noise_alerts"] = Json::array();
  for (const auto& row : model["noise_rows"])
    if (!threshold.empty() && row[8] != "0")
      model["noise_alerts"].push_back(Escape(
          (row[1].get<std::string>() + " on " + row[0].get<std::string>() +
           " exceeded the threshold in " + row[8].get<std::string>() + " of " +
           row[7].get<std::string>() + " minutes.")
              .c_str()));
  ctx.Render(page, std::move(model), res);
}

void RegisterAnalysisRoutes(httplib::Server& server, SmWebContext& ctx) {
  server.Get(
      "/data/noise", [&](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Cache-Control", "no-store");
        res.set_content(
            Query(ctx.database, kNoiseHistorySql,
                  {req.get_param_value("receiver"), Smoothing(req)}, false)
                .dump(),
            "application/json");
      });
}
}  // namespace web
}  // namespace skim
