#include "store/workers.h"

#include <sqlite_orm/sqlite_orm.h>

#include <stdexcept>
#include <utility>

#include "core/schema.h"

namespace skim {
namespace {
// The table row as stored. Enumerations are kept as their names so SQL in the
// panel can filter on them.
struct SmWorkerRow {
  std::string id;
  std::string kind;
  std::string plan;
  std::string state;
  std::string status;
  std::optional<int64_t> pid;
  std::optional<int64_t> started_at;
  int restarts = 0;
  std::optional<double> cpu_percent;
  std::optional<int64_t> rss_bytes;
};

auto MapWorkers(const char* database) {
  using namespace sqlite_orm;
  return make_storage(
      database,
      make_table("workers", make_column("id", &SmWorkerRow::id, primary_key()),
                 make_column("kind", &SmWorkerRow::kind),
                 make_column("plan", &SmWorkerRow::plan),
                 make_column("state", &SmWorkerRow::state),
                 make_column("status", &SmWorkerRow::status),
                 make_column("pid", &SmWorkerRow::pid),
                 make_column("started_at", &SmWorkerRow::started_at),
                 make_column("restarts", &SmWorkerRow::restarts),
                 make_column("cpu_percent", &SmWorkerRow::cpu_percent),
                 make_column("rss_bytes", &SmWorkerRow::rss_bytes)));
}

template <typename Enum, size_t N>
Enum Parse(const std::string& name, const char* const (&names)[N]) {
  for (size_t i = 0; i < N; ++i)
    if (name == names[i]) return static_cast<Enum>(i);
  throw std::runtime_error("unknown worker field value: " + name);
}

constexpr const char* kKinds[] = {"receiver", "service", "panel", "controller"};
constexpr const char* kStates[] = {"starting", "running", "exited", "finished",
                                   "failed"};
}  // namespace

const char* WorkerKindName(SmWorkerKind kind) {
  return kKinds[static_cast<int>(kind)];
}
const char* WorkerStateName(SmWorkerState state) {
  return kStates[static_cast<int>(state)];
}

struct SmWorkerStore::SmState {
  explicit SmState(const char* database) : storage(MapWorkers(database)) {
    storage.on_open = [](sqlite3* db) {
      if (sqlite3_busy_timeout(db, 5000) != SQLITE_OK)
        throw std::runtime_error(sqlite3_errmsg(db));
      MigrateStationSchema(db);
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

std::vector<SmWorkerRecord> SmWorkerStore::All() {
  std::vector<SmWorkerRecord> records;
  for (auto& row : state_->storage.get_all<SmWorkerRow>())
    records.push_back({std::move(row.id), Parse<SmWorkerKind>(row.kind, kKinds),
                       std::move(row.plan),
                       Parse<SmWorkerState>(row.state, kStates),
                       std::move(row.status), row.pid, row.started_at,
                       row.restarts, row.cpu_percent, row.rss_bytes});
  return records;
}

void SmWorkerStore::Save(const SmWorkerRecord& record) {
  state_->storage.replace(
      SmWorkerRow{record.id, WorkerKindName(record.kind), record.plan,
                  WorkerStateName(record.state), record.status, record.pid,
                  record.started_at, record.restarts, record.cpu_percent,
                  record.rss_bytes});
}

void SmWorkerStore::SaveUsage(const std::string& id,
                              std::optional<double> cpu_percent,
                              std::optional<int64_t> rss_bytes) {
  using namespace sqlite_orm;
  state_->storage.update_all(set(c(&SmWorkerRow::cpu_percent) = cpu_percent,
                                 c(&SmWorkerRow::rss_bytes) = rss_bytes),
                             where(c(&SmWorkerRow::id) == id));
}

void SmWorkerStore::Remove(const std::string& id) {
  state_->storage.remove<SmWorkerRow>(id);
}

void SmWorkerStore::Clear() { state_->storage.remove_all<SmWorkerRow>(); }
}  // namespace skim
