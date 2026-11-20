#pragma once

#include <filesystem>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>

#include "core/settings.h"

namespace skim {
struct SmStationIdentity {
  std::string callsign;
  std::string email;
  std::string name;
  std::string grid;
};

struct SmInstanceSettings {
  std::string address;
  int port;
  bool first_run_wizard;
  std::string database;
  std::string gallery;
  std::optional<SmStationIdentity> identity;
  nlohmann::json topology;
  // Raw [services.*] tables before station identity fallback is applied.
  nlohmann::json services;
  std::string source;
  std::string revision;
};

struct SmReportingServiceSettings {
  std::string service;
  bool enabled = false;
  std::optional<std::string> callsign;
  std::optional<std::string> grid;
  std::optional<std::string> host;
  std::optional<std::string> endpoint;
  std::optional<std::string> sign_in;
  std::optional<std::string> band_limits;
  std::optional<int> port;
  std::optional<int> backup_port;
};

struct SmReceiverSettings {
  std::string name;
  std::string device;
  int rate = 192000;
  bool enabled = true;
  std::optional<int64_t> center;
  std::optional<double> gain;
  std::optional<double> ppm;
  bool repeat = false;
  std::optional<int64_t> utc;
  std::optional<int64_t> slice_center;
  std::string mode;
  std::optional<int64_t> dial;
};

struct SmSliceSettings {
  std::string mode;
  std::optional<int64_t> frequency;
};

struct SmScheduleSettings {
  std::string name;
  int start_minute;
  int end_minute;
  int priority;
  int64_t center_hz;
  int days;
  bool enabled;
  std::optional<SmSliceSettings> slice;
};

// Station TOML or JSON with operator email, name, RBN sign-in and legacy
// login values replaced, for diagnostics that may be shared.
std::string RedactedSettings(const std::string& source);
nlohmann::json RedactedJson(nlohmann::json value);

void CreateDefaultSettings(const std::filesystem::path& file);
SmInstanceSettings LoadSettings(const std::filesystem::path& file);
SmInstanceSettings PersistDecoderIds(const std::filesystem::path& file,
                                     SmInstanceSettings current);
void SaveSettings(const std::filesystem::path& file, const std::string& source,
                  const std::string& expected);
void UpdateReceiver(const std::filesystem::path& file,
                    const std::string& expected, const std::string& original,
                    const std::optional<SmReceiverSettings>& receiver);
void UpdateSlice(const std::filesystem::path& file, const std::string& expected,
                 const std::string& receiver, const std::string& group,
                 std::optional<size_t> schedule, size_t index,
                 const std::optional<SmSliceSettings>& slice);
// Appends a slice to a receiver: to its manual slices, or to the slices
// attached to its schedules when it is schedule-driven.
void AddSlice(const std::filesystem::path& file, const std::string& expected,
              const std::string& receiver, const SmSliceSettings& slice);
void UpdateSchedule(const std::filesystem::path& file,
                    const std::string& expected, const std::string& receiver,
                    std::optional<size_t> index, bool duplicate,
                    const std::optional<SmScheduleSettings>& schedule);
void UpdateReportingService(const std::filesystem::path& file,
                            const std::string& expected,
                            const SmReportingServiceSettings& service);
// Writes [decoders.<mode>], keeping only values that differ from the
// decoder's defaults. Empty tables are removed.
void UpdateDecoderSettings(const std::filesystem::path& file,
                           const std::string& expected, const std::string& mode,
                           const SmSettingValues& values);
void SaveWizard(const std::filesystem::path& file, const std::string& expected,
                const SmStationIdentity& identity, const std::string& receiver,
                const std::string& device, int rate,
                std::optional<int64_t> dial, const std::string& mode,
                std::optional<int64_t> center, std::optional<int64_t> utc,
                bool repeat);
}  // namespace skim
