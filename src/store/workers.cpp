#include "store/workers.h"

#include <sqlite_orm/sqlite_orm.h>

#include <stdexcept>
#include <utility>

namespace skim {
namespace {
struct SmWorkerRow {
  std::string id;
  std::string plan;
  std::string status;
};

auto MapWorkers(const char* database) {
  using namespace sqlite_orm;
  return make_storage(
      database,
      make_table("workers", make_column("id", &SmWorkerRow::id, primary_key()),
                 make_column("plan", &SmWorkerRow::plan),
                 make_column("status", &SmWorkerRow::status)));
}
}  // namespace

struct SmWorkerStore::SmState {
  explicit SmState(const char* database) : storage(MapWorkers(database)) {
    storage.on_open = [](sqlite3* db) {
      if (sqlite3_busy_timeout(db, 5000) != SQLITE_OK)
        throw std::runtime_error(sqlite3_errmsg(db));
    };
    storage.open_forever();
  }

  decltype(MapWorkers(nullptr)) storage;
};

SmWorkerStore::SmWorkerStore(const char* database)
    : state_(std::make_unique<SmState>(database)) {}
SmWorkerStore::~SmWorkerStore() = default;

std::string SmWorkerStore::Plan(const std::string& id) {
  auto row = state_->storage.get_pointer<SmWorkerRow>(id);
  if (!row) throw std::runtime_error("worker plan unavailable");
  return std::move(row->plan);
}

void SmWorkerStore::Save(const std::string& id, std::string plan,
                         const std::string& status) {
  state_->storage.replace(SmWorkerRow{id, std::move(plan), status});
}

void SmWorkerStore::Remove(const std::string& id) {
  state_->storage.remove<SmWorkerRow>(id);
}

void SmWorkerStore::Clear() { state_->storage.remove_all<SmWorkerRow>(); }
}  // namespace skim
