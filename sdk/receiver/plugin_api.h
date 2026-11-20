#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "core/iq.h"
#include "core/plugin_export.h"

namespace skim {
struct SmReceiverPluginMetadata {
  std::string_view driver_id;
};

// Returned by skim_receiver_descriptor_v2. An empty prefix selects the
// default driver for device strings with no registered prefix.
struct SmReceiverPluginDescriptor {
  std::string_view prefix;
  std::string_view label;
  std::string_view probe_hint;
  bool recording;
  bool adjustable_gain;
  bool probe_from_device;
};

struct SmReceiverCaptureRequest {
  std::string device;
  int64_t center_hz;
  int rate;
  std::optional<double> gain_db;
  std::optional<double> correction_ppm;
};

// Return false to stop probing. A zero rate means the rate is unknown.
using SmReceiverCandidate =
    std::function<bool(const std::string&, const std::string&, uint32_t)>;

class SmReceiverPlugin {
 public:
  virtual ~SmReceiverPlugin() = default;
  virtual const SmReceiverPluginMetadata& metadata() const noexcept = 0;

  virtual void Probe(const std::string& selector,
                     const SmReceiverCandidate& found) const = 0;
  // Capture owns the device handle for this call. Callbacks run synchronously
  // and can throw to abort capture. The plugin receives a resolved selector.
  virtual void Capture(const SmReceiverCaptureRequest& request,
                       const Samples& samples,
                       const std::function<void()>& gap) const = 0;
};
}  // namespace skim
