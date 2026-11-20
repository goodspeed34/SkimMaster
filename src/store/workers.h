#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace skim {
// What a row in the workers table supervises.
enum class SmWorkerKind { Receiver, Service, Panel, Controller };

// Lifecycle of a supervised process. Running means the process was launched
// and has not exited; status carries the operator-facing detail.
enum class SmWorkerState { Starting, Running, Exited, Finished, Failed };

const char* WorkerKindName(SmWorkerKind kind);
const char* WorkerStateName(SmWorkerState state);

struct SmWorkerRecord {
  std::string id;
  SmWorkerKind kind = SmWorkerKind::Receiver;
  std::string plan = "{}";
  SmWorkerState state = SmWorkerState::Starting;
  std::string status;
  std::optional<int64_t> pid;
  // Unix seconds when the current process was launched.
  std::optional<int64_t> started_at;
  int restarts = 0;
  std::optional<double> cpu_percent;
  std::optional<int64_t> rss_bytes;
};

class SmWorkerStore {
 public:
  explicit SmWorkerStore(const char* database);
  ~SmWorkerStore();

  std::string Plan(const std::string& id);
  std::vector<SmWorkerRecord> All();
  void Save(const SmWorkerRecord& record);
  void SaveUsage(const std::string& id, std::optional<double> cpu_percent,
                 std::optional<int64_t> rss_bytes);
  void Remove(const std::string& id);
  void Clear();

 private:
  struct SmState;
  std::unique_ptr<SmState> state_;
};
}  // namespace skim
