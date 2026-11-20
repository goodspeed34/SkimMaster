#pragma once

// ZIP and scratch-directory helpers shared by snapshots and diagnostics.

#include <zip.h>

#include <filesystem>
#include <memory>
#include <string>

namespace skim {
using SmZip = std::unique_ptr<zip_t, decltype(&zip_discard)>;

// A private directory removed with its contents on destruction.
class SmScratch {
 public:
  explicit SmScratch(const std::filesystem::path& base);
  ~SmScratch();
  SmScratch(const SmScratch&) = delete;
  SmScratch& operator=(const SmScratch&) = delete;

  std::filesystem::path path;
};

SmZip OpenZip(const std::filesystem::path& path, int flags);
// Adds a file from disk. libzip reads it when the archive is closed.
void AddZipFile(zip_t* zip, const std::filesystem::path& path,
                const std::string& name);
// Adds an in-memory entry. The archive keeps its own copy.
void AddZipText(zip_t* zip, const std::string& name, const std::string& text);
// Writes the archive. On failure the partial file is discarded.
void CloseZip(SmZip& zip);

// A file in a scratch directory that lives as long as the returned pointer.
std::shared_ptr<const std::filesystem::path> TemporaryFile(
    const std::string& name);
}  // namespace skim
