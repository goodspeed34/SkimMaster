#pragma once

#include <filesystem>
#include <memory>
#include <string>

namespace skim {
void BackupStation(const char* db, const char* gallery, const char* archive);
std::shared_ptr<const std::filesystem::path> TemporarySnapshot(
    const char* db, const char* gallery);
// Checks a snapshot without changing the station. Throws when it is invalid.
void ValidateSnapshot(const char* archive);
// Replaces the station database and adds the snapshot's gallery images. The
// station must be stopped. Returns the pre-restore snapshot path, if any.
std::string RestoreStation(const char* archive, const char* db,
                           const char* gallery);
// Saves a snapshot, then removes the database and gallery images. With
// configuration set, also replaces the station TOML with the first-run
// sample after keeping a copy. Returns the pre-reset snapshot path, if any.
std::string FactoryReset(const char* db, const char* gallery,
                         const std::filesystem::path& config,
                         bool configuration);

// The panel stages a restore upload or factory reset request at these paths
// and asks for a full restart. ApplyPendingMaintenance carries it out
// between station runs, while no supervisor holds the station lock.
std::filesystem::path PendingRestorePath(const std::filesystem::path& db);
std::filesystem::path PendingResetPath(const std::filesystem::path& db);
void ApplyPendingMaintenance(const std::filesystem::path& config);
}  // namespace skim
