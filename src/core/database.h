#pragma once
#include <sqlite3.h>

#include <memory>
#include <stdexcept>
#include <string>

namespace skim {
class Database {
 public:
  explicit Database(const char* file) {
    sqlite3* handle = nullptr;
    int rc = sqlite3_open(file, &handle);
    db_.reset(handle);
    Check(rc);
    Check(sqlite3_busy_timeout(handle, 5000));
  }
  sqlite3* get() const { return db_.get(); }
  void Check(int rc) const {
    if (rc != SQLITE_OK) throw std::runtime_error(sqlite3_errmsg(get()));
  }
  void Exec(const char* sql) {
    Check(sqlite3_exec(get(), sql, nullptr, nullptr, nullptr));
  }
  auto Prepare(const char* sql) {
    sqlite3_stmt* q = nullptr;
    Check(sqlite3_prepare_v2(get(), sql, -1, &q, nullptr));
    return std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>(
        q, sqlite3_finalize);
  }
  void Text(sqlite3_stmt* q, int n, const std::string& s) {
    Check(sqlite3_bind_text(q, n, s.c_str(), s.size(), SQLITE_TRANSIENT));
  }
  void Done(sqlite3_stmt* q) {
    if (sqlite3_step(q) != SQLITE_DONE)
      throw std::runtime_error(sqlite3_errmsg(get()));
  }

 private:
  std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db_{nullptr,
                                                         sqlite3_close};
};
}  // namespace skim
