#pragma once
#include <nlohmann/json.hpp>
#include <string>
namespace skim {
void ValidateStation(const nlohmann::json& config);
void RunStationWorker(const char* database, const std::string& id);
// Appends a message to the station's system log.
void LogStation(const char* database, const std::string& component,
                const std::string& message);
// Recycles every receiver and service worker.
void RestartStation(const char* database);
// Stops the station and starts it again from its TOML file, applying listener
// and data path changes.
void RequestFullRestart(const char* database);
void RestartServiceWorker(const char* database, const std::string& service);
void SetScheduleOverride(const char* database, const std::string& receiver,
                         const nlohmann::json& schedule, int minutes);
nlohmann::json ScheduleOverrides(const char* database);
void TestReceiverCapture(const nlohmann::json& settings);
}  // namespace skim
