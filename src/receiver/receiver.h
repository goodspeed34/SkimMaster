#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "core/iq.h"

namespace skim {
struct ReceiverDriver {
  const char* id;
  const char* prefix;
  const char* label;
};
struct FoundReceiver {
  std::string device;
  std::string label;
  uint32_t rate;
};
const std::vector<ReceiverDriver>& ReceiverDrivers();
const ReceiverDriver& DriverFor(std::string_view device);
std::vector<FoundReceiver> ProbeReceiver(std::string_view driver,
                                         std::string selector,
                                         const std::filesystem::path& base);
inline bool IsWavReceiver(std::string_view device) {
  return device.rfind("wav:", 0) == 0;
}
inline bool IsReplayReceiver(std::string_view device) {
  return device == "-" || IsWavReceiver(device);
}
// Resolve a recording name relative to the caller's configuration directory.
std::filesystem::path ResolveWavPath(std::string_view path,
                                     const std::filesystem::path& base = {});
using IqReader = std::function<void(const char*, int64_t, int, const Samples&,
                                    const std::function<void()>&)>;
void ReadReceiver(const char* device, int64_t center, int rate, double gain,
                  double ppm, const Samples& samples,
                  const std::function<void()>& gap);
void ReadIq(const char* device, int64_t center, int rate,
            const Samples& samples, const std::function<void()>& gap);
}  // namespace skim
