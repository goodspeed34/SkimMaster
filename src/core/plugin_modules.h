#pragma once

#include <boost/dll/shared_library.hpp>
#include <string_view>
#include <vector>

namespace skim {
// Bundled builds place modules under plugins/ next to the tools executable.
// Installed modules may also sit directly alongside that executable.
std::vector<boost::dll::fs::path> PluginModules(std::string_view prefix);
}  // namespace skim
