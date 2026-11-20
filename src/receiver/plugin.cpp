#include <cmath>
#include <cstdio>
#include <stdexcept>

#include "receiver/adapters.h"
#include "receiver/api.h"
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

const SmReceiverPlugin& BuiltinPlugin() noexcept {
  static const SmBuiltinReceiverPlugin plugin;
  return plugin;
}

int CaptureV1(const skim_radio_config* cfg, skim_iq_callback emit, void* user,
              char* error, uint32_t capacity) {
  try {
    SmReceiverCaptureRequest request{
        cfg->device, cfg->center_hz, static_cast<int>(cfg->rate), {}, {}};
    if (std::isfinite(cfg->gain_db)) request.gain_db = cfg->gain_db;
    if (std::isfinite(cfg->correction_ppm))
      request.correction_ppm = cfg->correction_ppm;
    BuiltinPlugin().Capture(
        request,
        [&](const std::complex<float>* data, size_t n) {
          if (emit(user, reinterpret_cast<const float*>(data), n))
            throw std::runtime_error("receiver consumer failed");
        },
        [&] {
          if (emit(user, nullptr, 0))
            throw std::runtime_error("receiver consumer failed");
        });
    return 0;
  } catch (const std::exception& e) {
    std::snprintf(error, capacity, "%s", e.what());
  } catch (...) {
    std::snprintf(error, capacity, "receiver failed");
  }
  return -1;
}
}  // namespace
}  // namespace skim

#ifdef _WIN32
#define SKIM_EXPORT __declspec(dllexport)
#else
#define SKIM_EXPORT __attribute__((visibility("default")))
#endif

extern "C" SKIM_EXPORT const skim::SmReceiverPlugin*
skim_receiver_v2() noexcept {
  return &skim::BuiltinPlugin();
}

extern "C" SKIM_EXPORT const skim_radio_api* skim_receiver_v1() {
  static const skim_radio_api api{1, sizeof(skim_radio_api), skim::CaptureV1};
  return &api;
}

extern "C" SKIM_EXPORT int skim_receiver_probe_v1(const char* selector,
                                                  skim_receiver_found found,
                                                  void* user, char* error,
                                                  uint32_t capacity) {
  try {
    skim::BuiltinPlugin().Probe(
        selector, [&](const auto& device, const auto& label, uint32_t rate) {
          return found(user, device.c_str(), label.c_str(), rate) == 0;
        });
    return 0;
  } catch (const std::exception& e) {
    std::snprintf(error, capacity, "%s", e.what());
  } catch (...) {
    std::snprintf(error, capacity, "receiver probe failed");
  }
  return -1;
}
