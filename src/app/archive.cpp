#include "app/archive.h"

#include <cstdlib>
#include <cstring>
#include <random>
#include <stdexcept>

namespace skim {
namespace fs = std::filesystem;

SmScratch::SmScratch(const fs::path& base) {
  for (int i = 0; i < 10; ++i) {
    path = base / (".skimmaster-" + std::to_string(std::random_device{}()));
    if (fs::create_directory(path)) {
      fs::permissions(path, fs::perms::owner_all, fs::perm_options::replace);
      return;
    }
  }
  throw std::runtime_error("cannot reserve temporary directory");
}

SmScratch::~SmScratch() {
  std::error_code ec;
  fs::remove_all(path, ec);
}

SmZip OpenZip(const fs::path& path, int flags) {
  int err = 0;
  SmZip zip(zip_open(path.string().c_str(), flags, &err), zip_discard);
  if (!zip) throw std::runtime_error("cannot open archive: " + path.string());
  return zip;
}

void AddZipFile(zip_t* zip, const fs::path& path, const std::string& name) {
  auto source = zip_source_file(zip, path.string().c_str(), 0, -1);
  if (!source) throw std::runtime_error(zip_strerror(zip));
  if (zip_file_add(zip, name.c_str(), source, ZIP_FL_ENC_UTF_8) < 0) {
    zip_source_free(source);
    throw std::runtime_error(zip_strerror(zip));
  }
}

void AddZipText(zip_t* zip, const std::string& name, const std::string& text) {
  void* copy = std::malloc(text.size() ? text.size() : 1);
  if (!copy) throw std::bad_alloc();
  std::memcpy(copy, text.data(), text.size());
  // freep=1 hands the buffer to libzip.
  auto source = zip_source_buffer(zip, copy, text.size(), 1);
  if (!source) {
    std::free(copy);
    throw std::runtime_error(zip_strerror(zip));
  }
  if (zip_file_add(zip, name.c_str(), source, ZIP_FL_ENC_UTF_8) < 0) {
    zip_source_free(source);
    throw std::runtime_error(zip_strerror(zip));
  }
}

void CloseZip(SmZip& zip) {
  if (zip_close(zip.get()) != 0)
    throw std::runtime_error(zip_strerror(zip.get()));
  zip.release();
}

std::shared_ptr<const fs::path> TemporaryFile(const std::string& name) {
  struct SmTemporary {
    SmScratch scratch{fs::temp_directory_path()};
    fs::path file;
  };
  auto temporary = std::make_shared<SmTemporary>();
  temporary->file = temporary->scratch.path / name;
  return {temporary, &temporary->file};
}
}  // namespace skim
