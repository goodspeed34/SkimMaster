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

// Returned by skim_receiver_descriptor_v3. An empty prefix selects the
// default driver for device strings with no registered prefix.
struct SmReceiverPluginDescriptor {
  std::string_view prefix;
  std::string_view label;
  std::string_view probe_hint;
  bool recording;
  bool adjustable_gain;
  bool probe_from_device;
  // The plugin reports the tuning the device applies through
  // SmCaptureSink::state.
  bool reports_state;
};

struct SmReceiverCaptureRequest {
  std::string device;
  int64_t center_hz;
  int rate;
  std::optional<double> gain_db;
  std::optional<double> correction_ppm;
};

// What the device actually applied. Leave a field empty when the device does
// not report it. A device with several IQ streams (receivers, panadapters)
// exposes each as its own device string from Probe.
struct SmReceiverState {
  std::optional<int64_t> center_hz;
  std::optional<int> rate;
  std::optional<double> gain_db;
};

// Callbacks for one capture. They run synchronously on the capture thread
// and can throw to abort capture.
struct SmCaptureSink {
  Samples samples;
  // A discontinuity in the IQ stream.
  std::function<void()> gap;
  // Report the applied tuning when capture starts and whenever it changes,
  // for example when a network radio is retuned from its own controls. The
  // host drops IQ while the state differs from the request.
  std::function<void(const SmReceiverState&)> state;
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
  // Capture owns the device handle for this call and returns at the end of
  // the stream. The plugin receives a resolved selector.
  virtual void Capture(const SmReceiverCaptureRequest& request,
                       const SmCaptureSink& sink) const = 0;
};
}  // namespace skim
