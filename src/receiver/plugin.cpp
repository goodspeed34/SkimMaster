#include <cstdio>
#include <stdexcept>

#include "receiver/adapters.h"
#include "receiver/api.h"
namespace {
int Capture(const skim_radio_config* cfg, skim_iq_callback emit, void* user,
            char* error, uint32_t capacity) {
  try {
    auto samples = [&](const std::complex<float>* data, size_t n) {
      if (emit(user, reinterpret_cast<const float*>(data), n))
        throw std::runtime_error("receiver consumer failed");
    };
#if !defined(SKIM_RTLTCP) && !defined(SKIM_WAV)
    auto gap = [&] {
      if (emit(user, nullptr, 0))
        throw std::runtime_error("receiver consumer failed");
    };
#endif
#if defined(SKIM_WAV)
    if (std::isfinite(cfg->gain_db) || std::isfinite(cfg->correction_ppm))
      throw std::runtime_error(
          "IQ WAV does not support gain or PPM correction");
    skim::ReadWavIq(cfg->device, cfg->rate, samples);
#elif defined(SKIM_RTLTCP)
    skim::ReadRtlTcp(cfg->device, cfg->center_hz, cfg->rate, cfg->gain_db,
                     cfg->correction_ppm, samples);
#elif defined(SKIM_ALSA)
    if (std::isfinite(cfg->gain_db) || std::isfinite(cfg->correction_ppm))
      throw std::runtime_error(
          "configure zero-IF hardware gain and frequency externally");
    skim::ReadAlsa(cfg->device, cfg->rate, samples, gap);
#else
    skim::ReadSoapy(cfg->device, cfg->center_hz, cfg->rate, cfg->gain_db,
                    cfg->correction_ppm, samples, gap);
#endif
    return 0;
  } catch (const std::exception& e) {
    std::snprintf(error, capacity, "%s", e.what());
    return -1;
  } catch (...) {
    std::snprintf(error, capacity, "receiver failed");
    return -1;
  }
}
}  // namespace
#ifdef _WIN32
#define SKIM_EXPORT __declspec(dllexport)
#else
#define SKIM_EXPORT __attribute__((visibility("default")))
#endif
extern "C" SKIM_EXPORT const skim_radio_api* skim_receiver_v1() {
  static const skim_radio_api api{1, sizeof(skim_radio_api), Capture};
  return &api;
}
extern "C" SKIM_EXPORT int skim_receiver_probe_v1(const char* selector,
                                                  skim_receiver_found found,
                                                  void* user, char* error,
                                                  uint32_t capacity) {
  try {
    skim::ProbeDevices(
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
