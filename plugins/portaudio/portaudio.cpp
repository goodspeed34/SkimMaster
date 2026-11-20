#include <portaudio.h>

#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <system_error>

#include "receiver/plugin_api.h"

namespace skim {
namespace {
void CheckPortAudio(PaError error, const char* operation) {
  if (error != paNoError)
    throw std::runtime_error(std::string(operation) + ": " +
                             Pa_GetErrorText(error));
}

std::mutex portaudio_lifecycle;

class SmPortAudio {
 public:
  SmPortAudio() {
    std::lock_guard<std::mutex> lock(portaudio_lifecycle);
    CheckPortAudio(Pa_Initialize(), "cannot initialize PortAudio");
  }

  ~SmPortAudio() {
    std::lock_guard<std::mutex> lock(portaudio_lifecycle);
    Pa_Terminate();
  }

  SmPortAudio(const SmPortAudio&) = delete;
  SmPortAudio& operator=(const SmPortAudio&) = delete;
};

PaDeviceIndex InputIndex(const std::string& selector) {
  if (selector == "default") {
    const auto index = Pa_GetDefaultInputDevice();
    if (index == paNoDevice)
      throw std::runtime_error("PortAudio has no default input device");
    return index;
  }
  PaDeviceIndex index{};
  const auto* end = selector.data() + selector.size();
  const auto parsed = std::from_chars(selector.data(), end, index);
  if (parsed.ec != std::errc{} || parsed.ptr != end || index < 0)
    throw std::runtime_error("expected a PortAudio device index or default");
  return index;
}

class SmPortAudioReceiverPlugin final : public SmReceiverPlugin {
 public:
  const SmReceiverPluginMetadata& metadata() const noexcept override {
    static constexpr SmReceiverPluginMetadata metadata{"portaudio"};
    return metadata;
  }

  void Probe(const std::string& selector,
             const SmReceiverCandidate& found) const override {
    SmPortAudio audio;
    const auto count = Pa_GetDeviceCount();
    if (count < 0) CheckPortAudio(count, "cannot enumerate PortAudio devices");
    for (PaDeviceIndex index = 0; index < count; ++index) {
      const auto* device = Pa_GetDeviceInfo(index);
      if (!device || device->maxInputChannels < 2) continue;
      auto label = std::string(device->name);
      if (const auto* api = Pa_GetHostApiInfo(device->hostApi))
        label += " (" + std::string(api->name) + ")";
      const auto id = std::to_string(index);
      if (selector != id && label.find(selector) == std::string::npos) continue;
      const auto nominal = std::round(device->defaultSampleRate);
      const auto rate =
          nominal > 0 && nominal <= std::numeric_limits<uint32_t>::max()
              ? static_cast<uint32_t>(nominal)
              : 0;
      if (!found(id, label, rate)) break;
    }
  }

  void Capture(const SmReceiverCaptureRequest& request, const Samples& samples,
               const std::function<void()>& gap) const override {
    if (request.gain_db || request.correction_ppm)
      throw std::runtime_error(
          "configure zero-IF hardware gain and frequency externally");
    SmPortAudio audio;
    const auto index = InputIndex(request.device);
    const auto* device = Pa_GetDeviceInfo(index);
    if (!device) throw std::runtime_error("PortAudio input device unavailable");
    if (device->maxInputChannels < 2)
      throw std::runtime_error("PortAudio IQ requires a stereo input device");

    constexpr unsigned long frames = 1024;
    const PaStreamParameters input{index, 2, paFloat32,
                                   device->defaultLowInputLatency, nullptr};
    PaStream* handle = nullptr;
    CheckPortAudio(Pa_OpenStream(&handle, &input, nullptr, request.rate, frames,
                                 paNoFlag, nullptr, nullptr),
                   "cannot open PortAudio stereo input");
    std::unique_ptr<PaStream, decltype(&Pa_CloseStream)> stream(handle,
                                                                Pa_CloseStream);
    CheckPortAudio(Pa_StartStream(handle), "cannot start PortAudio input");

    std::array<float, 2 * frames> stereo;
    std::array<std::complex<float>, frames> iq;
    for (;;) {
      const auto status = Pa_ReadStream(handle, stereo.data(), frames);
      if (status == paInputOverflowed)
        gap();
      else
        CheckPortAudio(status, "cannot read PortAudio input");
      for (size_t i = 0; i < iq.size(); ++i)
        iq[i] = {stereo[2 * i], stereo[2 * i + 1]};
      samples(iq.data(), iq.size());
    }
  }
};
}  // namespace
}  // namespace skim

extern "C" SKIM_PLUGIN_EXPORT const skim::SmReceiverPlugin*
skim_receiver_v2() noexcept {
  static const skim::SmPortAudioReceiverPlugin plugin;
  return &plugin;
}

extern "C" SKIM_PLUGIN_EXPORT const skim::SmReceiverPluginDescriptor*
skim_receiver_descriptor_v2() noexcept {
  static constexpr skim::SmReceiverPluginDescriptor descriptor{
      "portaudio:", "PortAudio", "Optional input name or device index",
      false,        false,       false};
  return &descriptor;
}
