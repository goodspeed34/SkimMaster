// Statistics pages: band and mode activity, noise-floor analysis.

#include <array>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "core/bands.h"
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

constexpr const char* kActivityModes[] = {"CW", "RTTY", "FT8", "FT4", "WSPR"};
constexpr char kOutOfBand[] = "Out of band";

// The heat scale of the reference design: 0, 1-99, 100-999, 1000-4999 and
// 5000 or more decodes.
int Heat(int64_t count) {
  return count == 0     ? 0
         : count < 100  ? 1
         : count < 1000 ? 2
         : count < 5000 ? 3
                        : 4;
}

std::string Grouped(int64_t value) {
  auto digits = std::to_string(value);
  for (int i = static_cast<int>(digits.size()) - 3; i > 0; i -= 3)
    digits.insert(static_cast<size_t>(i), ",");
  return digits;
}

std::string Percent(int64_t part, int64_t whole) {
  if (whole == 0) return "0.0%";
  char text[16];
  std::snprintf(text, sizeof(text), "%.1f%%", 100.0 * part / whole);
  return text;
}

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

void RenderBandActivity(SmWebContext& ctx, const SmReferencePage& page,
                        const httplib::Request& req, httplib::Response& res,
                        Json model) {
  static const std::map<std::string, std::pair<const char*, int64_t>> periods{
      {"1h", {"1 hour", 3600}},
      {"24h", {"24 hours", 86400}},
      {"7d", {"7 days", 7 * 86400}},
      {"30d", {"30 days", 30 * 86400}}};
  auto period = req.get_param_value("period");
  if (!periods.count(period)) period = "24h";
  const auto& [period_label, seconds] = periods.at(period);
  auto normalize = req.get_param_value("normalize");
  if (normalize != "band" && normalize != "mode") normalize = "count";
  const bool numeric = req.get_param_value("display") == "numeric";
  const auto receiver = req.get_param_value("receiver");
  // Before the form is applied every mode is included.
  const bool applied = req.has_param("applied");
  std::vector<std::string> modes;
  for (const auto* mode : kActivityModes)
    if (!applied || req.has_param(std::string("include_") + mode))
      modes.push_back(mode);

  // band -> mode -> count, band -> hour -> count, mode -> hour -> count.
  std::map<std::string, std::map<std::string, int64_t>> matrix;
  std::map<std::string, std::array<int64_t, 24>> hours;
  std::map<std::string, std::array<int64_t, 24>> mode_hours;
  const auto rows = Query(
      ctx.database,
      "SELECT coalesce(band(frequency_hz),?3),mode,"
      "CAST(strftime('%H',utc_ms/1000,'unixepoch') AS INTEGER),count(*) "
      "FROM spots WHERE utc_ms>=(unixepoch()-CAST(?1 AS INTEGER))*1000 AND "
      "(?2='' OR receiver=?2) GROUP BY 1,2,3",
      {std::to_string(seconds), receiver, kOutOfBand}, false);
  for (const auto& row : rows) {
    const auto mode = row[1].get<std::string>();
    if (std::find(modes.begin(), modes.end(), mode) == modes.end()) continue;
    const auto band = row[0].get<std::string>();
    const int hour = std::stoi(row[2].get<std::string>());
    const int64_t count = std::stoll(row[3].get<std::string>());
    matrix[band][mode] += count;
    hours[band].at(hour) += count;
    mode_hours[mode].at(hour) += count;
  }
  std::vector<std::string> bands;
  for (const auto& band : kAmateurBands)
    if (matrix.count(std::string(band.name))) bands.emplace_back(band.name);
  if (matrix.count(kOutOfBand)) bands.emplace_back(kOutOfBand);

  std::map<std::string, int64_t> mode_totals;
  int64_t total = 0;
  for (const auto& [band, counts] : matrix)
    for (const auto& [mode, count] : counts) {
      mode_totals[mode] += count;
      total += count;
    }

  if (req.get_param_value("format") == "csv") {
    Json columns = Json::array({"Band"});
    for (const auto& mode : modes) columns.push_back(mode);
    columns.push_back("Total");
    Json csv_rows = Json::array();
    for (const auto& band : bands) {
      Json line = Json::array({band});
      int64_t sum = 0;
      for (const auto& mode : modes) {
        const auto count = matrix[band][mode];
        line.push_back(std::to_string(count));
        sum += count;
      }
      line.push_back(std::to_string(sum));
      csv_rows.push_back(std::move(line));
    }
    res.set_header("Content-Disposition",
                   "attachment; filename=band-mode-activity.csv");
    res.set_content(CsvDocument(columns, csv_rows), "text/csv; charset=utf-8");
    return;
  }

  auto cell = [&](int64_t count, int64_t band_total, int64_t mode_total) {
    const std::string text = normalize == "band"   ? Percent(count, band_total)
                             : normalize == "mode" ? Percent(count, mode_total)
                                                   : Grouped(count);
    return Json{{"text", text}, {"heat", numeric ? -1 : Heat(count)}};
  };
  Json matrix_rows = Json::array();
  for (const auto& band : bands) {
    int64_t band_total = 0;
    for (const auto& mode : modes) band_total += matrix[band][mode];
    Json cells = Json::array();
    for (const auto& mode : modes)
      cells.push_back(cell(matrix[band][mode], band_total, mode_totals[mode]));
    matrix_rows.push_back({{"band", Escape(band.c_str())},
                           {"cells", std::move(cells)},
                           {"total", Grouped(band_total)}});
  }
  Json all_cells = Json::array();
  for (const auto& mode : modes)
    all_cells.push_back(cell(mode_totals[mode], total, mode_totals[mode]));
  model["matrix_rows"] = std::move(matrix_rows);
  model["matrix_total"] = {{"cells", std::move(all_cells)},
                           {"total", Grouped(total)}};

  Json hour_rows = Json::array();
  for (const auto& band : bands) {
    Json cells = Json::array();
    for (int hour = 0; hour < 24; ++hour) {
      const auto count = hours[band][hour];
      cells.push_back({{"count", numeric ? Grouped(count) : ""},
                       {"title", Grouped(count) + " decodes"},
                       {"heat", numeric ? -1 : Heat(count)}});
    }
    hour_rows.push_back(
        {{"band", Escape(band.c_str())}, {"cells", std::move(cells)}});
  }
  model["hour_rows"] = std::move(hour_rows);

  std::map<std::string, std::string> unique;
  for (const auto& row :
       Query(ctx.database,
             "SELECT mode,count(DISTINCT callsign) FROM spots WHERE "
             "utc_ms>=(unixepoch()-CAST(?1 AS INTEGER))*1000 AND "
             "(?2='' OR receiver=?2) GROUP BY mode",
             {std::to_string(seconds), receiver}, false))
    unique[row[0].get<std::string>()] = row[1].get<std::string>();
  int64_t largest = 0;
  for (const auto& mode : modes) largest = std::max(largest, mode_totals[mode]);
  Json mode_rows = Json::array();
  for (const auto& mode : modes) {
    const auto count = mode_totals[mode];
    const auto& by_hour = mode_hours[mode];
    const auto peak = std::max_element(by_hour.begin(), by_hour.end());
    char peak_label[16];
    std::snprintf(peak_label, sizeof(peak_label), "%02d:00 UTC",
                  static_cast<int>(peak - by_hour.begin()));
    const double share = total ? 100.0 * count / total : 0;
    char width[16];
    std::snprintf(width, sizeof(width), "%.1f%%", share);
    mode_rows.push_back(
        {{"mode", mode},
         {"spots", Grouped(count)},
         {"share", Percent(count, total)},
         {"width", width},
         {"bar", count > 0 && count == largest ? "orange" : ""},
         {"unique",
          unique.count(mode) ? Grouped(std::stoll(unique[mode])) : "0"},
         {"peak", count ? std::string(peak_label) : "—"}});
  }
  model["mode_rows"] = std::move(mode_rows);

  model["activity_modes"] = Json::array();
  for (const auto* mode : kActivityModes)
    model["activity_modes"].push_back(
        {{"name", mode},
         {"checked",
          std::find(modes.begin(), modes.end(), mode) != modes.end()}});
  model["included_modes"] = modes;
  model["period"] = period;
  model["period_label"] = period_label;
  model["normalize"] = normalize;
  model["numeric"] = numeric;
  model["selected_receiver"] = Escape(receiver.c_str());
  model["activity_sources"] = Json::array();
  for (const auto& row :
       Query(ctx.database,
             "SELECT DISTINCT receiver FROM spots ORDER BY receiver LIMIT 200"))
    model["activity_sources"].push_back(row[0]);
  ctx.Render(page, std::move(model), res);
}

void RegisterAnalysisRoutes(httplib::Server& server, SmWebContext& ctx) {
  // Passbands of the running slices on a receiver, for spectrum markers:
  // mode, capture center (Hz) and width (Hz).
  server.Get("/data/slices", [&](const httplib::Request& req,
                                 httplib::Response& res) {
    res.set_header("Cache-Control", "no-store");
    res.set_content(
        Query(ctx.database,
              "SELECT json_extract(s.value,'$.mode'),"
              "CASE WHEN json_extract(s.value,'$.mode') IN ('CW','RTTY') THEN "
              "coalesce(json_extract(s.value,'$.center_hz'),"
              "json_extract(s.value,'$.dial_hz'),"
              "json_extract(w.plan,'$.center_hz')) "
              "ELSE json_extract(s.value,'$.dial_hz')+1500 END,"
              "CASE json_extract(s.value,'$.mode') WHEN 'CW' THEN 48000 "
              "WHEN 'RTTY' THEN 12000 ELSE 3000 END "
              "FROM workers w,json_each(w.plan,'$.slices') s "
              "WHERE w.kind='receiver' AND w.state='running' AND w.id=?1 "
              "ORDER BY s.key",
              {req.get_param_value("receiver")}, false)
            .dump(),
        "application/json");
  });
  server.Post("/spectrum-tune", [&](const httplib::Request& req,
                                    httplib::Response& res) {
    if (!ctx.ValidateForm(req, res)) return;
    try {
      const auto receiver = req.get_param_value("receiver");
      const auto mode = req.get_param_value("mode");
      if (mode != "CW" && mode != "RTTY" && mode != "FT8" && mode != "FT4" &&
          mode != "WSPR")
        throw std::runtime_error("select a decoder mode");
      const auto frequency = *FormInteger(req, "frequency", 1, kMaxFrequency);
      std::lock_guard<std::mutex> lock(ctx.config_writes);
      AddSlice(ctx.config, req.get_param_value("revision"), receiver,
               {mode, frequency});
      res.set_redirect("/spectrum-waterfall?receiver=" +
                           httplib::detail::encode_query_param(receiver) +
                           "&tuned=" + mode +
                           "&hz=" + std::to_string(frequency),
                       303);
    } catch (const std::exception& error) {
      res.status = 400;
      res.set_content(std::string("Could not add the slice: ") + error.what(),
                      "text/plain; charset=utf-8");
    }
  });
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
