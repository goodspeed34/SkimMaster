#include "app/diagnostics.h"

#include <sqlite3.h>

#include <chrono>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <stdexcept>
#include <string>

#include "app/archive.h"
#include "app/config.h"
#include "core/clock_health.h"
#include "core/database.h"
#include "core/plugin_modules.h"
#include "core/schema.h"
#include "receiver/receiver.h"
#include "source_offer.h"

namespace skim {
namespace {
namespace fs = std::filesystem;
using Json = nlohmann::json;

// Reads rows as JSON objects keyed by column name.
Json Rows(sqlite3* db, const char* sql) {
  sqlite3_stmt* query = nullptr;
  if (sqlite3_prepare_v2(db, sql, -1, &query, nullptr) != SQLITE_OK)
    throw std::runtime_error(sqlite3_errmsg(db));
  std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> stmt(
      query, sqlite3_finalize);
  auto rows = Json::array();
  int rc;
  while ((rc = sqlite3_step(query)) == SQLITE_ROW) {
    auto row = Json::object();
    for (int i = 0; i < sqlite3_column_count(query); ++i) {
      const char* name = sqlite3_column_name(query, i);
      switch (sqlite3_column_type(query, i)) {
        case SQLITE_INTEGER:
          row[name] = sqlite3_column_int64(query, i);
          break;
        case SQLITE_FLOAT:
          row[name] = sqlite3_column_double(query, i);
          break;
        case SQLITE_NULL:
          row[name] = nullptr;
          break;
        default:
          row[name] =
              reinterpret_cast<const char*>(sqlite3_column_text(query, i));
      }
    }
    rows.push_back(std::move(row));
  }
  if (rc != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(db));
  return rows;
}

std::string Platform() {
#if defined(_WIN32)
  return "windows";
#elif defined(__linux__)
  return "linux";
#else
  return "other";
#endif
}

std::string Compiler() {
#if defined(__clang__)
  return "clang " __clang_version__;
#elif defined(__GNUC__)
  return "gcc " __VERSION__;
#else
  return "unknown";
#endif
}

// Each section is written independently so one failure does not hide the
// rest of the bundle; the failure is recorded in errors.json instead.
template <typename Producer>
void Section(zip_t* zip, Json& errors, const std::string& name,
             Producer&& produce) {
  try {
    AddZipText(zip, name, produce());
  } catch (const std::exception& error) {
    errors[name] = error.what();
  }
}
}  // namespace

std::shared_ptr<const fs::path> TemporaryDiagnostics(const char* config,
                                                     const char* database,
                                                     const char* gallery) {
  auto archive = TemporaryFile("skimmaster-diagnostics.zip");
  auto zip = OpenZip(*archive, ZIP_CREATE | ZIP_EXCL);
  auto errors = Json::object();
  const auto created = std::chrono::duration_cast<std::chrono::seconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
  AddZipText(zip.get(), "manifest.json",
             Json{{"format", "SkimMasterDiagnostics"},
                  {"version", 1},
                  {"created_utc", created}}
                 .dump(2));
  Section(zip.get(), errors, "station.toml", [&] {
    std::ifstream in(config, std::ios::binary);
    if (!in) throw std::runtime_error("cannot read station TOML");
    std::ostringstream source;
    source << in.rdbuf();
    return RedactedSettings(source.str());
  });

  SmDatabase db(database);
  Section(zip.get(), errors, "about.json", [&] {
    auto receivers = Json::array();
    for (const auto& driver : ReceiverDrivers())
      receivers.push_back({{"id", driver.id},
                           {"prefix", driver.prefix},
                           {"module", driver.module_path.filename().string()}});
    auto decoders = Json::array();
    for (const auto& path : PluginModules("libdecoder"))
      decoders.push_back(path.filename().string());
    size_t images = 0;
    std::error_code ignored;
    for (const auto& entry : fs::directory_iterator(gallery, ignored))
      if (entry.path().extension() == ".png") ++images;
    return Json{{"version", SKIM_VERSION},
                {"source_url", SKIM_SOURCE_URL},
                {"schema_version", StationSchemaVersion(db.get())},
                {"platform", Platform()},
                {"compiler", Compiler()},
                {"clock_synchronized", ClockSynchronized()},
                {"receiver_plugins", std::move(receivers)},
                {"decoder_plugins", std::move(decoders)},
                {"gallery_images", images}}
        .dump(2);
  });
  Section(zip.get(), errors, "workers.json", [&] {
    auto workers = Rows(db.get(),
                        "SELECT id,kind,state,status,pid,started_at,restarts,"
                        "cpu_percent,rss_bytes,plan FROM workers ORDER BY id");
    for (auto& worker : workers)
      worker["plan"] = RedactedJson(
          Json::parse(worker.at("plan").get<std::string>(), nullptr, false));
    return workers.dump(2);
  });
  Section(zip.get(), errors, "system_log.json", [&] {
    return Rows(db.get(),
                "SELECT datetime(utc,'unixepoch') AS utc,component,message "
                "FROM (SELECT rowid,* FROM system_log ORDER BY rowid DESC "
                "LIMIT 5000) ORDER BY rowid")
        .dump(2);
  });
  Section(zip.get(), errors, "spectra.json", [&] {
    return Rows(db.get(),
                "SELECT receiver,utc,center_hz,rate,bins FROM spectra s "
                "WHERE utc=(SELECT max(utc) FROM spectra WHERE "
                "receiver=s.receiver) ORDER BY receiver")
        .dump(2);
  });
  Section(zip.get(), errors, "activity.json", [&] {
    return Json{{"spots_24h",
                 Rows(db.get(),
                      "SELECT receiver,mode,count(*) AS spots,"
                      "max(utc_ms) AS last_utc_ms FROM spots WHERE utc_ms >= "
                      "(unixepoch()-86400)*1000 GROUP BY receiver,mode")},
                {"receipts_24h",
                 Rows(db.get(),
                      "SELECT feed,count(*) AS receipts,max(sent_at) AS "
                      "last_sent FROM report_receipts WHERE sent_at >= "
                      "unixepoch()-86400 GROUP BY feed")},
                {"overrides", Rows(db.get(),
                                   "SELECT receiver,expires FROM "
                                   "schedule_overrides")}}
        .dump(2);
  });
  AddZipText(zip.get(), "errors.json", errors.dump(2));
  CloseZip(zip);
  return archive;
}
}  // namespace skim
