#pragma once

#include <memory>
#include <string>

namespace skim {
class SmWorkerStore {
 public:
  explicit SmWorkerStore(const char* database);
  ~SmWorkerStore();

  std::string Plan(const std::string& id);
  void Save(const std::string& id, std::string plan, const std::string& status);
  void Remove(const std::string& id);
  void Clear();

 private:
  struct SmState;
  std::unique_ptr<SmState> state_;
};
}  // namespace skim
