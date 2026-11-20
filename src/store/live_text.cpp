#include "store/live_text.h"

#include <iostream>
#include <stdexcept>
#include <utility>

#include "core/schema.h"

namespace skim {
namespace {
using Statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;

Statement Prepare(sqlite3* db, const char* sql) {
  sqlite3_stmt* statement = nullptr;
  if (sqlite3_prepare_v2(db, sql, -1, &statement, nullptr) != SQLITE_OK)
    throw std::runtime_error(sqlite3_errmsg(db));
  return {statement, sqlite3_finalize};
}

void Exec(sqlite3* db, const char* sql) {
  if (sqlite3_exec(db, sql, nullptr, nullptr, nullptr) != SQLITE_OK)
    throw std::runtime_error(sqlite3_errmsg(db));
}

void Step(sqlite3* db, sqlite3_stmt* statement) {
  if (sqlite3_step(statement) != SQLITE_DONE)
    throw std::runtime_error(sqlite3_errmsg(db));
  sqlite3_reset(statement);
}
}  // namespace

SmLiveTextWriter::SmLiveTextWriter(const char* database, std::string slice_id,
                                   std::string receiver, std::string mode)
    : slice_id_(std::move(slice_id)),
      receiver_(std::move(receiver)),
      mode_(std::move(mode)) {
  sqlite3* handle = nullptr;
  const int rc = sqlite3_open(database, &handle);
  db_.reset(handle);
  if (rc != SQLITE_OK) throw std::runtime_error(sqlite3_errmsg(handle));
  sqlite3_busy_timeout(handle, 5000);
  MigrateStationSchema(handle);
  Clear();
}

SmLiveTextWriter::~SmLiveTextWriter() {
  try {
    Clear();
  } catch (const std::exception& error) {
    std::cerr << "Could not clear live text: " << error.what() << '\n';
  }
}

void SmLiveTextWriter::Clear() {
  auto remove = Prepare(db_.get(), "DELETE FROM live_text WHERE slice_id=?");
  sqlite3_bind_text(remove.get(), 1, slice_id_.c_str(), -1, SQLITE_STATIC);
  Step(db_.get(), remove.get());
}

void SmLiveTextWriter::Replace(int64_t utc_ms,
                               const std::vector<SmTextChannel>& channels) {
  sqlite3* db = db_.get();
  Exec(db, "BEGIN IMMEDIATE");
  try {
    Clear();
    auto insert = Prepare(db,
                          "INSERT OR REPLACE INTO live_text VALUES"
                          "(?,?,?,?,?,?,?,?)");
    for (const auto& channel : channels) {
      auto* s = insert.get();
      sqlite3_bind_text(s, 1, slice_id_.c_str(), -1, SQLITE_STATIC);
      sqlite3_bind_text(s, 2, receiver_.c_str(), -1, SQLITE_STATIC);
      sqlite3_bind_text(s, 3, mode_.c_str(), -1, SQLITE_STATIC);
      sqlite3_bind_int64(s, 4, channel.frequency);
      sqlite3_bind_text(s, 5, channel.text.c_str(), -1, SQLITE_STATIC);
      if (channel.snr)
        sqlite3_bind_double(s, 6, *channel.snr);
      else
        sqlite3_bind_null(s, 6);
      if (channel.speed)
        sqlite3_bind_double(s, 7, *channel.speed);
      else
        sqlite3_bind_null(s, 7);
      sqlite3_bind_int64(s, 8, utc_ms);
      Step(db, s);
    }
    Exec(db, "COMMIT");
  } catch (...) {
    sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
    throw;
  }
}
}  // namespace skim
