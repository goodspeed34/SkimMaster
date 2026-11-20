#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "receiver/plugin_api.h"

namespace skim {
struct SmReceiverDriver {
  std::string id;
  std::string prefix;
  std::string label;
  std::string probe_hint;
  bool recording;
  bool adjustable_gain;
  bool probe_from_device;
  std::filesystem::path module_path;
};
struct SmFoundReceiver {
  std::string device;
  std::string label;
  uint32_t rate;
};
const std::vector<SmReceiverDriver>& ReceiverDrivers();
const SmReceiverDriver& DriverFor(std::string_view device);
std::vector<SmFoundReceiver> ProbeReceiver(std::string_view driver,
                                           std::string selector,
                                           const std::filesystem::path& base);
bool IsFileReceiver(std::string_view device);
inline bool IsReplayReceiver(std::string_view device) {
  return device == "-" || IsFileReceiver(device);
}
// Resolve file-backed selectors relative to the caller's configuration
// directory.
std::string ResolveReceiverDevice(std::string_view device,
                                  const std::filesystem::path& base);
using IqReader = std::function<void(const char*, int64_t, int, const Samples&,
                                    const std::function<void()>&)>;
void ReadReceiver(SmReceiverCaptureRequest request, const Samples& samples,
                  const std::function<void()>& gap);
void ReadReceiver(const char* device, int64_t center, int rate, double gain,
                  double ppm, const Samples& samples,
                  const std::function<void()>& gap);
void ReadIq(const char* device, int64_t center, int rate,
            const Samples& samples, const std::function<void()>& gap);
}  // namespace skim
