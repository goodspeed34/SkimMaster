#pragma once
#include <nlohmann/json.hpp>
namespace skim {
void Wideband(const char* config);
void Wideband(const nlohmann::json& root);
}  // namespace skim
