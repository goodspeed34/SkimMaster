#include "app/backup.h"

#include <sqlite3.h>
#include <zip.h>

#include <array>
#include <boost/interprocess/sync/file_lock.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <nlohmann/json.hpp>
#include <random>
#include <regex>
#include <set>

#include "app/station_lock.h"
#include "core/database.h"
#include "store/store.h"

namespace skim {
namespace {
namespace fs = std::filesystem;
using Zip = std::unique_ptr<zip_t, decltype(&zip_discard)>;
class SmScratch {
 public:
  explicit SmScratch(const fs::path& base) {
    for (int i = 0; i < 10; ++i) {
      path = base / (".skimmaster-" + std::to_string(std::random_device{}()));
      if (fs::create_directory(path)) {
        fs::permissions(path, fs::perms::owner_all, fs::perm_options::replace);
        return;
      }
    }
    throw std::runtime_error("cannot reserve temporary directory");
  }
  ~SmScratch() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
  fs::path path;
};
void CopyDatabase(sqlite3* source, sqlite3* target) {
  auto handle = sqlite3_backup_init(target, "main", source, "main");
  if (!handle) throw std::runtime_error(sqlite3_errmsg(target));
  int rc = sqlite3_backup_step(handle, -1);
  int finish = sqlite3_backup_finish(handle);
  if (rc != SQLITE_DONE || finish != SQLITE_OK)
    throw std::runtime_error(sqlite3_errmsg(target));
}
Zip OpenZip(const fs::path& path, int flags) {
  int err = 0;
  Zip zip(zip_open(path.string().c_str(), flags, &err), zip_discard);
  if (!zip) throw std::runtime_error("cannot open archive: " + path.string());
  return zip;
}
void Add(zip_t* zip, const fs::path& path, const std::string& name) {
  auto source = zip_source_file(zip, path.string().c_str(), 0, -1);
  if (!source) throw std::runtime_error(zip_strerror(zip));
  if (zip_file_add(zip, name.c_str(), source, ZIP_FL_ENC_UTF_8) < 0) {
    zip_source_free(source);
    throw std::runtime_error(zip_strerror(zip));
  }
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
  Add(zip.get(), snapshot, "station.sqlite");
  std::ofstream(temp.path / "manifest.json")
      << "{\"format\":\"SkimMaster\",\"version\":1}";
  CountEntry("manifest.json", fs::file_size(temp.path / "manifest.json"),
             total);
  Add(zip.get(), temp.path / "manifest.json", "manifest.json");
  if (fs::exists(gallery))
    for (const auto& file : fs::directory_iterator(gallery)) {
      auto name = file.path().filename().string();
      if (fs::is_regular_file(file.symlink_status()) &&
          std::regex_match(name, kImage)) {
        if (++entries > kMaxEntries)
          throw std::runtime_error("snapshot exceeds 100000 entries");
        CountEntry("gallery/" + name, fs::file_size(file.path()), total);
        Add(zip.get(), file.path(), "gallery/" + name);
      }
    }
  if (zip_close(zip.get()) != 0)
    throw std::runtime_error(zip_strerror(zip.get()));
  zip.release();
}
std::shared_ptr<const fs::path> TemporarySnapshot(const char* path,
                                                  const char* gallery) {
  struct SmSnapshot {
    SmScratch scratch{fs::temp_directory_path()};
    fs::path archive = scratch.path / "station-snapshot.zip";
  };
  auto snapshot = std::make_shared<SmSnapshot>();
  BackupStation(path, gallery, snapshot->archive.string().c_str());
  return {snapshot, &snapshot->archive};
}
void RestoreStation(const char* archive, const char* path,
                    const char* gallery) {
  const auto database = OperationalDatabasePath(path);
  const auto database_name = database.string();
  std::string lockfile = StationLockPath(database).string();
  std::ofstream(lockfile, std::ios::app).close();
  boost::interprocess::file_lock lock(lockfile.c_str());
  if (!lock.try_lock())
    throw std::runtime_error("stop the station before restoring");
  SmScratch temp(fs::temp_directory_path());
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
    auto destination = temp.path / name;
    fs::create_directories(destination.parent_path());
    Extract(zip.get(), i, destination, stat.size);
  }
  if (!names.count("manifest.json") || !names.count("station.sqlite"))
    throw std::runtime_error("incomplete snapshot");
  std::ifstream manifest(temp.path / "manifest.json");
  auto metadata = nlohmann::json::parse(manifest);
  if (metadata.at("format") != "SkimMaster" || metadata.at("version") != 1)
    throw std::runtime_error("unsupported snapshot");
  auto source = (temp.path / "station.sqlite").string();
  SmDatabase src(source.c_str());
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
  auto check = src.Prepare(
      "SELECT receiver,utc_ms,mode,frequency_hz,message,callsign,grid FROM "
      "spots LIMIT 0");
  check.reset();
  // Exercise the storage migration and prepare the production insert statements
  // on the extracted copy before touching the operator's files.
  SaveSlot(source.c_str(), "", "", 0, {});
  if (fs::exists(database)) {
    auto before =
        database_name + ".pre-restore-" +
        std::to_string(
            std::chrono::system_clock::now().time_since_epoch().count()) +
        ".zip";
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
}
}  // namespace skim
