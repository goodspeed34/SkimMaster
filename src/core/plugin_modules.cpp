#include "core/plugin_modules.h"

#include <algorithm>
#include <boost/dll/runtime_symbol_info.hpp>
#include <filesystem>

namespace skim {
std::vector<boost::dll::fs::path> PluginModules(std::string_view prefix) {
  namespace fs = std::filesystem;
  const fs::path root(boost::dll::program_location().parent_path().native());
  std::vector<boost::dll::fs::path> modules;
  auto add = [&](const fs::directory_entry& entry) {
    const auto& path = entry.path();
    std::error_code error;
    if (path.filename().string().rfind(prefix, 0) == 0 &&
        path.extension() == SKIM_MODULE_SUFFIX && entry.is_regular_file(error))
      modules.emplace_back(path.native());
  };
  std::error_code error;
  for (fs::recursive_directory_iterator
           it(root / "plugins", fs::directory_options::skip_permission_denied,
              error),
       end;
       it != end && !error; it.increment(error))
    add(*it);
  std::sort(modules.begin(), modules.end());
  const auto bundled = modules.size();
  for (fs::directory_iterator it(root, error), end; it != end && !error;
       it.increment(error))
    add(*it);
  std::sort(modules.begin() + bundled, modules.end());
  return modules;
}
}  // namespace skim
