#pragma once
#include <nlohmann/json.hpp>
namespace skim {
void RunReporter(const char* feed, const char* database,
                 const nlohmann::json& cfg);
void RunReporter(const char* feed, const char* database, const char* config);
}  // namespace skim
