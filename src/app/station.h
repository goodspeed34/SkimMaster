#pragma once
#include <nlohmann/json.hpp>
#include <string>
namespace skim {
void ValidateStation(const nlohmann::json& config);
void RunStationWorker(const char* database, const std::string& id);
void RestartStation(const char* database);
void RestartServiceWorker(const char* database, const std::string& service);
void SetScheduleOverride(const char* database, const std::string& receiver,
                         const nlohmann::json& schedule, int minutes);
nlohmann::json ScheduleOverrides(const char* database);
void TestReceiverCapture(const nlohmann::json& settings);
}  // namespace skim
