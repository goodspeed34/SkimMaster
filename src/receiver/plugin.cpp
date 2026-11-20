#include <cmath>
#include <stdexcept>

#include "receiver/adapters.h"
#include "receiver/plugin_api.h"

namespace skim {
namespace {
class SmBuiltinReceiverPlugin final : public SmReceiverPlugin {
 public:
  const SmReceiverPluginMetadata& metadata() const noexcept override {
    static constexpr SmReceiverPluginMetadata metadata{SKIM_RECEIVER_ID};
    return metadata;
  }

  void Probe(const std::string& selector,
             const SmReceiverCandidate& found) const override {
    ProbeDevices(selector, found);
  }

  void Capture(const SmReceiverCaptureRequest& cfg, const Samples& samples,
               const std::function<void()>& gap) const override {
#if defined(SKIM_WAV)
    (void)gap;
    if (cfg.gain_db || cfg.correction_ppm)
      throw std::runtime_error(
          "IQ WAV does not support gain or PPM correction");
    ReadWavIq(cfg.device.c_str(), cfg.rate, samples);
#elif defined(SKIM_RTLTCP)
    (void)gap;
    ReadRtlTcp(cfg.device, cfg.center_hz, cfg.rate, cfg.gain_db.value_or(NAN),
               cfg.correction_ppm.value_or(NAN), samples);
#elif defined(SKIM_ALSA)
    if (cfg.gain_db || cfg.correction_ppm)
      throw std::runtime_error(
          "configure zero-IF hardware gain and frequency externally");
    ReadAlsa(cfg.device, cfg.rate, samples, gap);
#else
    ReadSoapy(cfg.device.c_str(), cfg.center_hz, cfg.rate,
              cfg.gain_db.value_or(NAN), cfg.correction_ppm.value_or(NAN),
              samples, gap);
#endif
  }
};

}  // namespace
}  // namespace skim

#ifdef _WIN32
#define SKIM_EXPORT __declspec(dllexport)
#else
#define SKIM_EXPORT __attribute__((visibility("default")))
#endif

extern "C" SKIM_EXPORT const skim::SmReceiverPlugin*
skim_receiver_v2() noexcept {
  static const skim::SmBuiltinReceiverPlugin plugin;
  return &plugin;
}
