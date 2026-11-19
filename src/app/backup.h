#pragma once

#include <filesystem>
#include <memory>

namespace skim {
void BackupStation(const char* db, const char* gallery, const char* archive);
std::shared_ptr<const std::filesystem::path> TemporarySnapshot(
    const char* db, const char* gallery);
void RestoreStation(const char* archive, const char* db, const char* gallery);
}  // namespace skim
