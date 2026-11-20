#pragma once

#include <filesystem>
#include <memory>

namespace skim {
// Builds a ZIP an operator can attach to a bug report: redacted station
// TOML, worker state, recent system log, versions, plugins, clock state,
// recent spectrum frames and reception counts. It holds no receptions,
// images or personal settings. The file lives as long as the pointer.
std::shared_ptr<const std::filesystem::path> TemporaryDiagnostics(
    const char* config, const char* database, const char* gallery);
}  // namespace skim
