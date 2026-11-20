#include <alsa/asoundlib.h>

#include <array>
#include <cstdlib>
#include <memory>
#include <stdexcept>

#include "receiver/plugin_api.h"

namespace skim {
namespace {
void ProbeDevices(const std::string& selector,
                  const SmReceiverCandidate& found) {
  void** hints = nullptr;
  const int rc = snd_device_name_hint(-1, "pcm", &hints);
  if (rc < 0) throw std::runtime_error(snd_strerror(rc));
  std::unique_ptr<void*, decltype(&snd_device_name_free_hint)> cleanup(
      hints, snd_device_name_free_hint);
  for (auto hint = hints; hint && *hint; ++hint) {
    auto get = [&](const char* key) {
      std::unique_ptr<char, decltype(&std::free)> value(
          snd_device_name_get_hint(*hint, key), std::free);
      return value ? std::string(value.get()) : std::string();
    };
    if (get("IOID") == "Output") continue;
    auto name = get("NAME");
    auto label = get("DESC");
    if (name.empty() || name == "null" ||
        (name.find(selector) == std::string::npos &&
         label.find(selector) == std::string::npos))
      continue;
    for (char& c : label)
      if (c == '\n') c = ' ';
    if (!found(name, label.empty() ? name : label, 0)) break;
  }
}

void ReadAlsa(const std::string& name, int rate, const Samples& consume,
              const std::function<void()>& gap) {
  snd_pcm_t* handle = nullptr;
  auto check = [](int rc) {
    if (rc < 0) throw std::runtime_error(snd_strerror(rc));
  };
  check(snd_pcm_open(&handle, name.c_str(), SND_PCM_STREAM_CAPTURE, 0));
  std::unique_ptr<snd_pcm_t, decltype(&snd_pcm_close)> pcm(handle,
                                                           snd_pcm_close);
  check(snd_pcm_set_params(handle, SND_PCM_FORMAT_S16,
                           SND_PCM_ACCESS_RW_INTERLEAVED, 2, rate, 1, 50000));
  std::array<int16_t, 8192> raw;
  std::array<std::complex<float>, 4096> iq;
  for (;;) {
    auto n = snd_pcm_readi(handle, raw.data(), iq.size());
    if (n < 0) {
      check(snd_pcm_recover(handle, n, 1));
      gap();
      continue;
    }
    for (int i = 0; i < n; ++i)
      iq[i] = {raw[2 * i] / 32768.f, raw[2 * i + 1] / 32768.f};
    consume(iq.data(), n);
  }
}

class SmAlsaReceiverPlugin final : public SmReceiverPlugin {
 public:
  const SmReceiverPluginMetadata& metadata() const noexcept override {
    static constexpr SmReceiverPluginMetadata metadata{"alsa"};
    return metadata;
  }

  void Probe(const std::string& selector,
             const SmReceiverCandidate& found) const override {
    ProbeDevices(selector, found);
  }

  void Capture(const SmReceiverCaptureRequest& cfg, const Samples& samples,
               const std::function<void()>& gap) const override {
    if (cfg.gain_db || cfg.correction_ppm)
      throw std::runtime_error(
          "configure zero-IF hardware gain and frequency externally");
    ReadAlsa(cfg.device, cfg.rate, samples, gap);
  }
};
}  // namespace
}  // namespace skim

extern "C" SKIM_PLUGIN_EXPORT const skim::SmReceiverPlugin*
skim_receiver_v2() noexcept {
  static const skim::SmAlsaReceiverPlugin plugin;
  return &plugin;
}

extern "C" SKIM_PLUGIN_EXPORT const skim::SmReceiverPluginDescriptor*
skim_receiver_descriptor_v2() noexcept {
  static constexpr skim::SmReceiverPluginDescriptor descriptor{
      "alsa:", "ALSA", "Optional PCM name filter", false, false, false};
  return &descriptor;
}
