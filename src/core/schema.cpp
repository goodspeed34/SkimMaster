#include "core/schema.h"

#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "core/database.h"

namespace skim {
namespace {
// Version 1 is the schema that shipped before versioning. Databases from that
// era record user_version 0 and may already hold some of these tables.
constexpr const char* kVersion1 = R"sql(
CREATE TABLE IF NOT EXISTS station_runtime(
  id INTEGER PRIMARY KEY CHECK(id=1),
  generation INTEGER NOT NULL);
INSERT OR IGNORE INTO station_runtime VALUES(1,0);
CREATE TABLE IF NOT EXISTS workers(
  id TEXT PRIMARY KEY,
  plan TEXT NOT NULL,
  status TEXT NOT NULL);
CREATE TABLE IF NOT EXISTS system_log(
  utc INTEGER NOT NULL,
  component TEXT NOT NULL,
  message TEXT NOT NULL);
CREATE TABLE IF NOT EXISTS schedule_overrides(
  receiver TEXT PRIMARY KEY,
  schedule TEXT NOT NULL,
  expires INTEGER NOT NULL);
CREATE TABLE IF NOT EXISTS worker_restarts(
  id TEXT PRIMARY KEY,
  generation INTEGER NOT NULL);
CREATE TABLE IF NOT EXISTS spots(
  receiver TEXT NOT NULL,
  utc_ms INTEGER NOT NULL,
  mode TEXT NOT NULL,
  frequency_hz INTEGER NOT NULL,
  message TEXT NOT NULL,
  callsign TEXT,
  grid TEXT,
  UNIQUE(receiver,utc_ms,mode,frequency_hz,message));
CREATE INDEX IF NOT EXISTS spots_time ON spots(utc_ms);
CREATE TABLE IF NOT EXISTS live_receptions(
  spot_id INTEGER PRIMARY KEY,
  snr REAL,
  speed REAL);
CREATE TABLE IF NOT EXISTS report_receipts(
  feed TEXT NOT NULL,
  spot_id INTEGER NOT NULL,
  sent_at INTEGER NOT NULL,
  PRIMARY KEY(feed,spot_id));
CREATE TABLE IF NOT EXISTS wspr_details(
  spot_id INTEGER PRIMARY KEY,
  dial INTEGER,
  dt REAL,
  power TEXT);
CREATE TABLE IF NOT EXISTS spectra(
  receiver TEXT,
  utc INTEGER,
  center_hz INTEGER,
  rate INTEGER,
  bins TEXT,
  PRIMARY KEY(receiver,utc));
)sql";

// Version 2 gives workers a kind (receiver, service, panel or controller) and
// a machine-readable lifecycle state next to the operator-facing status text,
// plus the process data the panel reports. A full restart request gets its
// own generation counter.
constexpr const char* kVersion2 = R"sql(
ALTER TABLE workers ADD COLUMN kind TEXT NOT NULL DEFAULT 'receiver';
UPDATE workers SET kind='service' WHERE id LIKE '@%';
ALTER TABLE workers ADD COLUMN state TEXT NOT NULL DEFAULT 'starting';
ALTER TABLE workers ADD COLUMN pid INTEGER;
ALTER TABLE workers ADD COLUMN started_at INTEGER;
ALTER TABLE workers ADD COLUMN restarts INTEGER NOT NULL DEFAULT 0;
ALTER TABLE workers ADD COLUMN cpu_percent REAL;
ALTER TABLE workers ADD COLUMN rss_bytes INTEGER;
ALTER TABLE station_runtime ADD COLUMN exec_generation INTEGER NOT NULL
  DEFAULT 0;
)sql";

// Version 3: the text each CW and RTTY channel is decoding, replaced by the
// receiver worker about once a second.
constexpr char kVersion3[] = R"sql(
CREATE TABLE IF NOT EXISTS live_text(
  slice_id TEXT NOT NULL,
  receiver TEXT NOT NULL,
  mode TEXT NOT NULL,
  frequency_hz INTEGER NOT NULL,
  text TEXT NOT NULL,
  snr REAL,
  speed REAL,
  updated_ms INTEGER NOT NULL,
  PRIMARY KEY(slice_id, frequency_hz));
)sql";

// Version 4: one-minute noise-floor averages per receiver, in dBFS/Hz.
constexpr char kVersion4[] = R"sql(
CREATE TABLE IF NOT EXISTS noise_floor(
  receiver TEXT NOT NULL,
  utc INTEGER NOT NULL,
  center_hz INTEGER NOT NULL,
  rate INTEGER NOT NULL,
  floor_db REAL NOT NULL,
  samples INTEGER NOT NULL,
  PRIMARY KEY(receiver, utc));
)sql";

constexpr const char* kSteps[] = {kVersion1, kVersion2, kVersion3, kVersion4};
static_assert(sizeof(kSteps) / sizeof(kSteps[0]) == kStationSchemaVersion,
              "one migration step per schema version");
}  // namespace

int StationSchemaVersion(sqlite3* db) {
  sqlite3_stmt* query = nullptr;
  if (sqlite3_prepare_v2(db, "PRAGMA user_version", -1, &query, nullptr) !=
      SQLITE_OK)
    throw std::runtime_error(sqlite3_errmsg(db));
  std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> stmt(
      query, sqlite3_finalize);
  if (sqlite3_step(query) != SQLITE_ROW)
    throw std::runtime_error(sqlite3_errmsg(db));
  return sqlite3_column_int(query, 0);
}

void MigrateStationSchema(sqlite3* db) {
  auto check_version = [](int version) {
    if (version > kStationSchemaVersion)
      throw std::runtime_error(
          "the station database was written by a newer Skim Master (schema " +
          std::to_string(version) + ")");
  };
  const int seen = StationSchemaVersion(db);
  check_version(seen);
  if (seen == kStationSchemaVersion) return;
  auto exec = [&](const char* sql) {
    char* error = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &error) != SQLITE_OK) {
      std::string message = error ? error : sqlite3_errmsg(db);
      sqlite3_free(error);
      throw std::runtime_error("schema migration failed: " + message);
    }
  };
  exec("BEGIN IMMEDIATE");
  try {
    // Another process may have migrated while this one waited for the lock.
    int version = StationSchemaVersion(db);
    check_version(version);
    for (; version < kStationSchemaVersion; ++version) exec(kSteps[version]);
    exec(("PRAGMA user_version=" + std::to_string(version)).c_str());
    exec("COMMIT");
  } catch (...) {
    sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
    throw;
  }
}

namespace {
using SmRows = std::vector<std::vector<std::string>>;

SmRows Rows(sqlite3* db, const std::string& sql) {
  sqlite3_stmt* query = nullptr;
  if (sqlite3_prepare_v2(db, sql.c_str(), -1, &query, nullptr) != SQLITE_OK)
    throw std::runtime_error(sqlite3_errmsg(db));
  std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> stmt(
      query, sqlite3_finalize);
  SmRows rows;
  int rc;
  while ((rc = sqlite3_step(query)) == SQLITE_ROW) {
    auto& row = rows.emplace_back();
    for (int i = 0; i < sqlite3_column_count(query); ++i) {
      const auto* text =
          reinterpret_cast<const char*>(sqlite3_column_text(query, i));
      row.emplace_back(text ? text : "");
    }
  }
  if (rc != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(db));
  return rows;
}

std::string Quote(const std::string& name) {
  std::string quoted = "'";
  for (char c : name)
    quoted += c == '\'' ? std::string("''") : std::string(1, c);
  return quoted + "'";
}

// Each table's columns (name, type, not null, primary key position) and the
// column lists of its unique indexes, including those behind constraints.
std::map<std::string, std::pair<SmRows, std::set<SmRows>>> Shape(sqlite3* db) {
  std::map<std::string, std::pair<SmRows, std::set<SmRows>>> shape;
  for (const auto& table :
       Rows(db,
            "SELECT name FROM sqlite_schema WHERE type='table' AND "
            "name NOT LIKE 'sqlite_%'")) {
    const auto name = Quote(table[0]);
    auto& entry = shape[table[0]];
    for (const auto& column :
         Rows(db,
              "SELECT name,upper(type),\"notnull\",pk FROM "
              "pragma_table_info(" +
                  name + ")"))
      entry.first.push_back(column);
    for (const auto& index : Rows(db, "SELECT name FROM pragma_index_list(" +
                                          name + ") WHERE \"unique\"=1"))
      entry.second.insert(Rows(
          db, "SELECT name FROM pragma_index_info(" + Quote(index[0]) + ")"));
  }
  return shape;
}
}  // namespace

void VerifyStationSchema(sqlite3* db) {
  sqlite3* handle = nullptr;
  int rc = sqlite3_open(":memory:", &handle);
  std::unique_ptr<sqlite3, decltype(&sqlite3_close)> reference(handle,
                                                               sqlite3_close);
  if (rc != SQLITE_OK) throw std::runtime_error(sqlite3_errmsg(handle));
  MigrateStationSchema(handle);
  const auto actual = Shape(db);
  for (const auto& [table, expected] : Shape(handle)) {
    const auto found = actual.find(table);
    if (found == actual.end())
      throw std::runtime_error("station database is missing table " + table);
    if (found->second.first != expected.first)
      throw std::runtime_error("station table " + table +
                               " has unexpected columns");
    for (const auto& unique : expected.second)
      if (!found->second.second.count(unique))
        throw std::runtime_error("station table " + table +
                                 " is missing a unique constraint");
  }
}

void InitStationSchema(const char* path) {
  SmDatabase db(path);
  MigrateStationSchema(db.get());
}
}  // namespace skim
