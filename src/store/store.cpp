#include "store/store.h"

#include <sqlite3.h>

#include <memory>
#include <stdexcept>

#include "core/schema.h"

namespace skim {
void SaveSlot(const char* database, const char* receiver, const char* mode,
              int64_t utc_ms, const std::vector<SmSpot>& spots, bool live) {
  sqlite3* handle = nullptr;
  int rc = sqlite3_open(database, &handle);
  std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db(handle, sqlite3_close);
  auto check = [&](int result) {
    if (result != SQLITE_OK) throw std::runtime_error(sqlite3_errmsg(handle));
  };
  check(rc);
  check(sqlite3_busy_timeout(handle, 5000));
  auto exec = [&](const char* sql) {
    check(sqlite3_exec(handle, sql, nullptr, nullptr, nullptr));
  };
  MigrateStationSchema(handle);
  exec("BEGIN IMMEDIATE");
  sqlite3_stmt* details = nullptr;
  check(sqlite3_prepare_v2(handle, "INSERT INTO wspr_details VALUES(?,?,?,?)",
                           -1, &details, nullptr));
  std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> detail_stmt(
      details, sqlite3_finalize);
  sqlite3_stmt* live_query = nullptr;
  check(sqlite3_prepare_v2(handle, "INSERT INTO live_receptions VALUES(?,?,?)",
                           -1, &live_query, nullptr));
  std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> record(
      live_query, sqlite3_finalize);
  sqlite3_stmt* insert = nullptr;
  check(sqlite3_prepare_v2(
      handle,
      "INSERT INTO "
      "spots(receiver,utc_ms,mode,frequency_hz,message,callsign,grid)"
      " VALUES(?,?,?,?,?,NULLIF(?,''),NULLIF(?,'')) ON "
      "CONFLICT(receiver,utc_ms,mode,frequency_hz,message) DO NOTHING",
      -1, &insert, nullptr));
  std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(
      insert, sqlite3_finalize);
  auto bind = [&](int index, const char* text) {
    check(sqlite3_bind_text(insert, index, text, -1, SQLITE_TRANSIENT));
  };
  for (const auto& spot : spots) {
    bind(1, receiver);
    check(sqlite3_bind_int64(insert, 2, utc_ms));
    bind(3, mode);
    check(sqlite3_bind_int64(insert, 4, spot.frequency));
    bind(5, spot.message.c_str());
    bind(6, spot.callsign.c_str());
    bind(7, spot.grid.c_str());
    if (sqlite3_step(insert) != SQLITE_DONE)
      throw std::runtime_error(sqlite3_errmsg(handle));
    auto id = sqlite3_last_insert_rowid(handle);
    bool inserted = sqlite3_changes(handle) == 1;
    if (inserted && spot.wspr) {
      check(sqlite3_bind_int64(details, 1, id));
      check(sqlite3_bind_int64(details, 2, spot.wspr->dial));
      check(sqlite3_bind_double(details, 3, spot.wspr->dt));
      check(sqlite3_bind_text(details, 4, spot.wspr->power.c_str(), -1,
                              SQLITE_TRANSIENT));
      if (sqlite3_step(details) != SQLITE_DONE)
        throw std::runtime_error(sqlite3_errmsg(handle));
      check(sqlite3_reset(details));
    }
    if (live && inserted) {
      check(sqlite3_bind_int64(live_query, 1, id));
      if (spot.snr) check(sqlite3_bind_double(live_query, 2, *spot.snr));
      if (spot.speed) check(sqlite3_bind_double(live_query, 3, *spot.speed));
      if (sqlite3_step(live_query) != SQLITE_DONE)
        throw std::runtime_error(sqlite3_errmsg(handle));
      check(sqlite3_reset(live_query));
      check(sqlite3_clear_bindings(live_query));
    }
    check(sqlite3_reset(insert));
  }
  exec("COMMIT");
}
}  // namespace skim
