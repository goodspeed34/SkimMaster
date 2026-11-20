#include "app/backup.h"

#include <sqlite3.h>
#include <zip.h>

#include <array>
#include <boost/interprocess/sync/file_lock.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>
#include <regex>
#include <set>

#include "app/archive.h"
#include "app/config.h"
#include "app/station.h"
#include "app/station_lock.h"
#include "core/database.h"
#include "core/schema.h"

namespace skim {
namespace {
namespace fs = std::filesystem;
void CopyDatabase(sqlite3* source, sqlite3* target) {
  auto handle = sqlite3_backup_init(target, "main", source, "main");
  if (!handle) throw std::runtime_error(sqlite3_errmsg(target));
  int rc = sqlite3_backup_step(handle, -1);
  int finish = sqlite3_backup_finish(handle);
  if (rc != SQLITE_DONE || finish != SQLITE_OK)
    throw std::runtime_error(sqlite3_errmsg(target));
}
void Extract(zip_t* zip, zip_uint64_t i, const fs::path& target,
             uint64_t size) {
  std::unique_ptr<zip_file_t, decltype(&zip_fclose)> entry(
      zip_fopen_index(zip, i, 0), zip_fclose);
  if (!entry) throw std::runtime_error(zip_strerror(zip));
  std::ofstream out(target, std::ios::binary);
  out.exceptions(std::ios::badbit | std::ios::failbit);
  std::array<char, 65536> buffer;
  while (size) {
    auto n = zip_fread(entry.get(), buffer.data(),
                       std::min<uint64_t>(size, buffer.size()));
    if (n <= 0) throw std::runtime_error("incomplete archive entry");
    out.write(buffer.data(), n);
    size -= n;
  }
  if (zip_fread(entry.get(), buffer.data(), 1) != 0)
    throw std::runtime_error("archive length or CRC mismatch");
  out.close();
}
const std::regex kImage(
    "[0-9]+-(?:Martin[12]|Scottie[12]|Robot36|Robot72)\\.png");
constexpr uint64_t kMaxEntries = 100000;
constexpr uint64_t kMaxBytes = 8ULL << 30;

void CountEntry(const std::string& name, uint64_t size, uint64_t& total) {
  const uint64_t limit = name == "station.sqlite"  ? 4ULL << 30
                         : name == "manifest.json" ? 4096
                                                   : 32ULL << 20;
  if (size > limit)
    throw std::runtime_error("snapshot entry exceeds its size limit: " + name);
  if (size > kMaxBytes - total)
    throw std::runtime_error("snapshot exceeds the 8 GiB restore limit");
  total += size;
}
}  // namespace
void BackupStation(const char* path, const char* gallery, const char* archive) {
  if (!fs::is_regular_file(path))
    throw std::runtime_error("station database not found");
  auto zip = OpenZip(archive, ZIP_CREATE | ZIP_EXCL);
  SmScratch temp(fs::temp_directory_path());
  auto snapshot = temp.path / "station.sqlite";
  {
    SmDatabase src(path), dst(snapshot.string().c_str());
    CopyDatabase(src.get(), dst.get());
  }
  uint64_t total = 0, entries = 2;
  CountEntry("station.sqlite", fs::file_size(snapshot), total);
  AddZipFile(zip.get(), snapshot, "station.sqlite");
  std::ofstream(temp.path / "manifest.json")
      << "{\"format\":\"SkimMaster\",\"version\":1}";
  CountEntry("manifest.json", fs::file_size(temp.path / "manifest.json"),
             total);
  AddZipFile(zip.get(), temp.path / "manifest.json", "manifest.json");
  if (fs::exists(gallery))
    for (const auto& file : fs::directory_iterator(gallery)) {
      auto name = file.path().filename().string();
      if (fs::is_regular_file(file.symlink_status()) &&
          std::regex_match(name, kImage)) {
        if (++entries > kMaxEntries)
          throw std::runtime_error("snapshot exceeds 100000 entries");
        CountEntry("gallery/" + name, fs::file_size(file.path()), total);
        AddZipFile(zip.get(), file.path(), "gallery/" + name);
      }
    }
  CloseZip(zip);
}
std::shared_ptr<const fs::path> TemporarySnapshot(const char* path,
                                                  const char* gallery) {
  auto archive = TemporaryFile("station-snapshot.zip");
  BackupStation(path, gallery, archive->string().c_str());
  return archive;
}
namespace {
// Extracts and validates a snapshot into directory: entry names and sizes,
// the manifest, SQLite integrity and the station schema. The extracted
// database is migrated to this build's schema. Returns the entry names.
std::set<std::string> ExtractSnapshot(const char* archive,
                                      const fs::path& directory) {
  auto zip = OpenZip(archive, ZIP_RDONLY | ZIP_CHECKCONS);
  auto count = zip_get_num_entries(zip.get(), 0);
  if (count < 2 || static_cast<uint64_t>(count) > kMaxEntries)
    throw std::runtime_error("invalid archive entry count");
  std::set<std::string> names;
  uint64_t total = 0;
  for (zip_int64_t i = 0; i < count; ++i) {
    zip_stat_t stat{};
    if (zip_stat_index(zip.get(), i, 0, &stat) < 0)
      throw std::runtime_error(zip_strerror(zip.get()));
    std::string name = stat.name;
    bool image = name.rfind("gallery/", 0) == 0 &&
                 std::regex_match(name.substr(8), kImage);
    if ((name != "station.sqlite" && name != "manifest.json" && !image) ||
        !names.insert(name).second)
      throw std::runtime_error("unexpected archive entry");
    CountEntry(name, stat.size, total);
    auto destination = directory / name;
    fs::create_directories(destination.parent_path());
    Extract(zip.get(), i, destination, stat.size);
  }
  if (!names.count("manifest.json") || !names.count("station.sqlite"))
    throw std::runtime_error("incomplete snapshot");
  std::ifstream manifest(directory / "manifest.json");
  auto metadata = nlohmann::json::parse(manifest);
  if (metadata.at("format") != "SkimMaster" || metadata.at("version") != 1)
    throw std::runtime_error("unsupported snapshot");
  SmDatabase src((directory / "station.sqlite").string().c_str());
  src.Exec("PRAGMA trusted_schema=OFF");
  auto verify = src.Prepare("PRAGMA integrity_check");
  if (sqlite3_step(verify.get()) != SQLITE_ROW ||
      std::string(reinterpret_cast<const char*>(
          sqlite3_column_text(verify.get(), 0))) != "ok")
    throw std::runtime_error("invalid database snapshot");
  verify.reset();
  auto schema = src.Prepare(
      "SELECT name,type FROM sqlite_schema WHERE type IN ('view','trigger') OR "
      "sql LIKE 'CREATE VIRTUAL%'");
  if (sqlite3_step(schema.get()) != SQLITE_DONE)
    throw std::runtime_error("unsupported snapshot schema");
  schema.reset();
  // Migrate the extracted copy before touching the operator's files. This
  // also rejects snapshots written by a newer schema.
  MigrateStationSchema(src.get());
  VerifyStationSchema(src.get());
  return names;
}

// Holds the station lock, so no supervisor can run during maintenance.
class SmMaintenanceLock {
 public:
  explicit SmMaintenanceLock(const fs::path& database)
      : path_(StationLockPath(database).string()) {
    std::ofstream(path_, std::ios::app).close();
    lock_ = boost::interprocess::file_lock(path_.c_str());
    if (!lock_.try_lock())
      throw std::runtime_error("stop the station before restoring");
  }

 private:
  std::string path_;
  boost::interprocess::file_lock lock_;
};

std::string Stamp() {
  return std::to_string(std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count());
}

void RemoveDatabaseFiles(const fs::path& database) {
  for (const char* suffix : {"", "-journal", "-wal", "-shm"})
    fs::remove(database.string() + suffix);
}
}  // namespace

void ValidateSnapshot(const char* archive) {
  SmScratch temp(fs::temp_directory_path());
  ExtractSnapshot(archive, temp.path);
}

std::string RestoreStation(const char* archive, const char* path,
                           const char* gallery) {
  const auto database = OperationalDatabasePath(path);
  const auto database_name = database.string();
  SmMaintenanceLock lock(database);
  SmScratch temp(fs::temp_directory_path());
  const auto names = ExtractSnapshot(archive, temp.path);
  const auto source = (temp.path / "station.sqlite").string();
  SmDatabase src(source.c_str());
  std::string before;
  if (fs::exists(database)) {
    before = database_name + ".pre-restore-" + Stamp() + ".zip";
    BackupStation(database_name.c_str(), gallery, before.c_str());
  }
  fs::create_directories(gallery);
  SmScratch staged(gallery);
  std::vector<fs::path> added;
  try {
    for (const auto& name : names)
      if (name.rfind("gallery/", 0) == 0) {
        auto leaf = fs::path(name).filename();
        auto dest = fs::path(gallery) / leaf;
        if (fs::exists(dest)) {
          if (!fs::is_regular_file(fs::symlink_status(dest)))
            throw std::runtime_error("invalid gallery destination");
          std::ifstream existing(dest, std::ios::binary),
              incoming(temp.path / name, std::ios::binary);
          if (!std::equal(std::istreambuf_iterator<char>(existing), {},
                          std::istreambuf_iterator<char>(incoming), {}))
            throw std::runtime_error("gallery image name collision");
        } else {
          fs::copy_file(temp.path / name, staged.path / leaf);
          fs::rename(staged.path / leaf, dest);
          added.push_back(dest);
        }
      }
    SmDatabase dst(database_name.c_str());
    CopyDatabase(src.get(), dst.get());
  } catch (...) {
    for (const auto& file : added) fs::remove(file);
    throw;
  }
  return before;
}

std::string FactoryReset(const char* path, const char* gallery,
                         const std::filesystem::path& config,
                         bool configuration) {
  const auto database = OperationalDatabasePath(path);
  const auto database_name = database.string();
  SmMaintenanceLock lock(database);
  const auto stamp = Stamp();
  std::string before;
  if (fs::exists(database)) {
    before = database_name + ".pre-reset-" + stamp + ".zip";
    BackupStation(database_name.c_str(), gallery, before.c_str());
  }
  RemoveDatabaseFiles(database);
  std::error_code ignored;
  for (const auto& file : fs::directory_iterator(gallery, ignored))
    if (fs::is_regular_file(file.symlink_status()) &&
        std::regex_match(file.path().filename().string(), kImage))
      fs::remove(file.path());
  if (configuration) {
    fs::copy_file(config, config.string() + ".pre-reset-" + stamp + ".toml");
    fs::remove(config);
    CreateDefaultSettings(config);
  }
  return before;
}

fs::path PendingRestorePath(const fs::path& database) {
  return database.string() + ".pending-restore.zip";
}

fs::path PendingResetPath(const fs::path& database) {
  return database.string() + ".pending-reset.json";
}

void ApplyPendingMaintenance(const fs::path& config) {
  const auto settings = LoadSettings(config);
  const auto database = OperationalDatabasePath(settings.database);
  const auto database_name = database.string();
  const auto restore = PendingRestorePath(database);
  const auto reset = PendingResetPath(database);
  auto record = [&](const std::string& message) {
    std::cerr << "[maintenance] " << message << '\n';
    LogStation(database_name.c_str(), "maintenance", message);
  };
  if (fs::exists(reset)) {
    bool configuration = false;
    try {
      std::ifstream in(reset);
      configuration = nlohmann::json::parse(in).value("configuration", false);
    } catch (const std::exception&) {
    }
    fs::remove(reset);
    fs::remove(restore);
    try {
      const auto before =
          FactoryReset(database_name.c_str(), settings.gallery.c_str(), config,
                       configuration);
      record(std::string("Factory reset") +
             (configuration ? " including station configuration" : "") +
             (before.empty() ? "" : "; previous data saved to " + before));
    } catch (const std::exception& error) {
      record(std::string("Factory reset failed: ") + error.what());
    }
    return;
  }
  if (!fs::exists(restore)) return;
  // Claim the upload so a failure is not retried on every start.
  const auto claimed = restore.string() + ".applying";
  fs::rename(restore, claimed);
  try {
    const auto before = RestoreStation(claimed.c_str(), database_name.c_str(),
                                       settings.gallery.c_str());
    fs::remove(claimed);
    record("Restored snapshot uploaded from the panel" +
           (before.empty() ? std::string()
                           : "; previous data saved to " + before));
  } catch (const std::exception& error) {
    const auto failed = restore.string() + ".failed";
    fs::rename(claimed, failed);
    record(std::string("Snapshot restore failed: ") + error.what() +
           "; the upload was kept as " + failed);
  }
}
}  // namespace skim
