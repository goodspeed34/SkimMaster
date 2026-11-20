#include "web/common.h"

#include <httplib.h>
#include <sqlite3.h>

#include <algorithm>
#include <array>
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

#include "app/slice_tuning.h"
#include "core/bands.h"
#include "receiver/receiver.h"
#include "source_offer.h"
#include "web/panel_assets.h"

namespace skim {
namespace web {
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
                                   int64_t min, int64_t max, bool optional) {
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
    options.push_back({{"id", Escape(driver.id.c_str())},
                       {"label", Escape(driver.label.c_str())},
                       {"prefix", Escape(driver.prefix.c_str())},
                       {"hint", Escape(driver.probe_hint.c_str())},
                       {"recording", driver.recording},
                       {"gain", driver.adjustable_gain},
                       {"probe_from_device", driver.probe_from_device}});
  return options;
}
std::string DisplayField(const Json& row, const char* key,
                         const char* fallback) {
  auto field = row.find(key);
  if (field == row.end()) return fallback;
  return field->is_string()
             ? Escape(field->get_ref<const std::string&>().c_str())
             : field->dump();
}

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
const char kSliceFrequencySql[] =
    "CASE WHEN json_extract(s.value,'$.mode') IN ('CW','RTTY') THEN "
    "coalesce(json_extract(s.value,'$.center_hz'),"
    "json_extract(s.value,'$.dial_hz'),json_extract(w.plan,'$.center_hz')) "
    "ELSE json_extract(s.value,'$.dial_hz') END";
const char kLiveTextSql[] =
    "SELECT receiver,frequency_hz,CASE WHEN speed IS NULL THEN '' WHEN "
    "mode='CW' THEN printf('%.0f WPM',speed) ELSE printf('%g Bd',speed) END,"
    "CASE WHEN snr IS NULL THEN '' ELSE printf('%.1f dB',snr) END,text "
    "FROM live_text WHERE mode=?1 AND (?2='' OR receiver=?2) AND "
    "updated_ms>CAST(strftime('%s','now') AS INTEGER)*1000-60000 "
    "ORDER BY receiver,frequency_hz LIMIT 64";

const char kWorkerUsageSql[] =
    "CASE WHEN w.cpu_percent IS NULL AND w.rss_bytes IS NULL THEN '—' ELSE "
    "coalesce(printf('%.1f%%',w.cpu_percent),'—')||' · '||"
    "coalesce(printf('%.1f MB',w.rss_bytes/1048576.0),'—') END";

namespace {
void BandFunction(sqlite3_context* context, int, sqlite3_value** values) {
  if (sqlite3_value_type(values[0]) == SQLITE_NULL) {
    sqlite3_result_null(context);
    return;
  }
  const auto band = BandName(sqlite3_value_int64(values[0]));
  if (band.empty())
    sqlite3_result_null(context);
  else
    sqlite3_result_text(context, band.data(), static_cast<int>(band.size()),
                        SQLITE_STATIC);
}
}  // namespace

Json Query(const char* database, const char* sql,
           const std::vector<std::string>& parameters, bool html) {
  sqlite3* handle = nullptr;
  int rc = sqlite3_open_v2(database, &handle, SQLITE_OPEN_READONLY, nullptr);
  std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db(handle, sqlite3_close);
  if (rc != SQLITE_OK) throw std::runtime_error(sqlite3_errmsg(handle));
  sqlite3_busy_timeout(handle, 5000);
  if (sqlite3_create_function(handle, "band", 1,
                              SQLITE_UTF8 | SQLITE_DETERMINISTIC, nullptr,
                              BandFunction, nullptr, nullptr) != SQLITE_OK)
    throw std::runtime_error(sqlite3_errmsg(handle));
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
      // Neutralize spreadsheet formulas, but keep negative numbers numeric.
      static const std::regex number(R"(-?[0-9]+(\.[0-9]+)?)");
      if (!value.empty() &&
          std::string_view("=+-@\t\r").find(value.front()) !=
              std::string_view::npos &&
          !std::regex_match(value, number))
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

void SendTemporaryFile(httplib::Response& res,
                       std::shared_ptr<const std::filesystem::path> file,
                       const std::string& filename) {
  struct SmDownload {
    std::shared_ptr<const std::filesystem::path> file;
    std::ifstream stream;
  };
  auto download = std::make_shared<SmDownload>(
      SmDownload{file, std::ifstream(*file, std::ios::binary)});
  if (!download->stream)
    throw std::runtime_error("cannot open " + filename + " for download");
  res.set_header("Content-Disposition", "attachment; filename=" + filename);
  res.set_content_provider(
      std::filesystem::file_size(*file), "application/zip",
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
}

bool SupervisorRunning(const SmWebContext& ctx) {
  return !Query(ctx.database,
                "SELECT 1 FROM workers WHERE id='@controller' AND "
                "state='running'")
              .empty();
}

SmSettingsSnapshot SmSettingsCache::Read() {
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

bool SmWebContext::ValidateForm(const httplib::Request& req,
                                httplib::Response& res) const {
  res.set_header("Cache-Control", "no-store");
  // Multipart forms carry their fields as parts rather than parameters.
  const auto csrf = req.is_multipart_form_data()
                        ? req.get_file_value("csrf").content
                        : req.get_param_value("csrf");
  if (csrf != token) {
    res.status = 403;
    res.set_content("This form expired. Reload the page and try again.",
                    "text/plain");
    return false;
  }
  return true;
}

void SmWebContext::Render(const SmReferencePage& page, Json model,
                          httplib::Response& res) const {
  model["page"] = page.slug;
  model["title"] = page.title;
  model["reference_help"] = page.help;
  if (!model.contains("config_error")) model["config_error"] = "";
  if (!model.contains("config_recovery_guidance"))
    model["config_recovery_guidance"] = "";
  auto live =
      Query(this->database,
            "SELECT id,printf('%.3f',json_extract(plan,'$.center_hz')/1e6),"
            "printf('%g',json_extract(plan,'$.rate')/1e3),"
            "json_array_length(plan,'$.slices') FROM workers "
            "WHERE kind='receiver' AND state='running' ORDER BY id");
  model["receivers"] = live;
  int slices = 0;
  for (const auto& receiver : live)
    slices += std::stoi(receiver[3].get<std::string>());
  model["slice_count"] = slices;
  const auto bands =
      Query(this->database,
            (std::string("SELECT coalesce(group_concat(band,', '),'—') FROM "
                         "(SELECT DISTINCT band(") +
             kSliceFrequencySql +
             ") AS band FROM workers w,json_each(w.plan,'$.slices') s "
             "WHERE w.kind='receiver' AND w.state='running' AND band IS NOT "
             "NULL ORDER BY 1)")
                .c_str());
  model["bands"] = bands[0][0];
  const auto usage = Query(
      this->database,
      "SELECT CASE WHEN count(cpu_percent)=0 AND count(rss_bytes)=0 THEN '—' "
      "ELSE coalesce(printf('%.1f%%',sum(cpu_percent)),'—')||' · '||"
      "coalesce(printf('%.1f MB',sum(rss_bytes)/1048576.0),'—') END,"
      "(SELECT unixepoch()-started_at FROM workers WHERE id='@controller') "
      "FROM workers WHERE state='running'");
  model["cpu_load"] = usage[0][0];
  model["version"] = SKIM_VERSION;
  model["source_url"] = Escape(SKIM_SOURCE_URL);
  // The supervisor's start time is the station uptime. A standalone panel
  // has no supervisor, so fall back to the panel's own uptime.
  const auto& station_uptime = usage[0][1].get_ref<const std::string&>();
  auto count = !station_uptime.empty()
                   ? std::max<int64_t>(0, std::stoll(station_uptime))
                   : std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::steady_clock::now() - this->panel_start)
                         .count();
  model["uptime"] = count;
  std::ostringstream uptime;
  uptime << count / 86400 << "d " << std::setfill('0') << std::setw(2)
         << (count / 3600) % 24 << "h " << std::setw(2) << (count / 60) % 60
         << 'm';
  model["uptime_label"] = uptime.str();
  model["utc"] = Query(this->database, "SELECT datetime('now')")[0][0];
  res.set_content(inja::render(kTemplate, model), "text/html; charset=utf-8");
}
}  // namespace web
}  // namespace skim
