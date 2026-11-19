#pragma once

#include <filesystem>

namespace skim {
inline std::filesystem::path OperationalDatabasePath(
    const std::filesystem::path& path) {
  return std::filesystem::weakly_canonical(path);
}

inline std::filesystem::path StationLockPath(
    const std::filesystem::path& database) {
  auto lock = database;
  lock += ".station.lock";
  return lock;
}
}  // namespace skim
