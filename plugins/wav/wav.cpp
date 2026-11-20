#include <sndfile.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <memory>
#include <stdexcept>

#include "receiver/plugin_api.h"

namespace skim {
namespace {
using WavFile = std::unique_ptr<SNDFILE, decltype(&sf_close)>;
WavFile OpenIq(const char* path, SF_INFO& info) {
  WavFile input(sf_open(path, SFM_READ, &info), sf_close);
  if (!input)
    throw std::runtime_error(std::string("cannot open IQ WAV: ") +
                             sf_strerror(nullptr));
  const int format = info.format & SF_FORMAT_TYPEMASK;
  if ((format != SF_FORMAT_WAV && format != SF_FORMAT_WAVEX &&
       format != SF_FORMAT_RF64) ||
      info.channels != 2)
    throw std::runtime_error(
        "expected a stereo WAV with I on left and Q on right");
  if (info.frames <= 0) throw std::runtime_error("IQ WAV contains no samples");
  return input;
}

void ProbeDevices(const std::string& selector,
                  const SmReceiverCandidate& found) {
  namespace fs = std::filesystem;
  auto emit = [&](const fs::path& path, const SF_INFO& info) {
    return found(path.string(), path.filename().string(), info.samplerate);
  };
  if (fs::is_regular_file(selector)) {
    SF_INFO info{};
    OpenIq(selector.c_str(), info);
    emit(selector, info);
    return;
  }
  for (const auto& entry : fs::directory_iterator(selector)) {
    std::error_code error;
    if (!entry.is_regular_file(error)) continue;
    auto extension = entry.path().extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    if (extension != ".wav" && extension != ".wave" && extension != ".rf64")
      continue;
    SF_INFO info{};
    try {
      OpenIq(entry.path().string().c_str(), info);
    } catch (const std::runtime_error&) {
      continue;
    }
    if (!emit(entry.path(), info)) break;
  }
}
void ReadWavIq(const char* path, int rate, const Samples& consume) {
  SF_INFO info{};
  auto input = OpenIq(path, info);
  if (info.samplerate != rate)
    throw std::runtime_error("configured sample rate " + std::to_string(rate) +
                             " does not match IQ WAV rate " +
                             std::to_string(info.samplerate));

  std::array<std::complex<float>, 4096> iq;
  std::array<float, 2 * iq.size()> frames;
  sf_count_t received = 0;
  for (;;) {
    auto count = sf_readf_float(input.get(), frames.data(), iq.size());
    if (count <= 0) break;
    for (sf_count_t i = 0; i < count; ++i)
      iq[i] = {frames[2 * i], frames[2 * i + 1]};
    consume(iq.data(), count);
    received += count;
  }
  if (sf_error(input.get()) != SF_ERR_NO_ERROR || received != info.frames)
    throw std::runtime_error("failed to read the complete IQ WAV");
}

class SmWavReceiverPlugin final : public SmReceiverPlugin {
 public:
  const SmReceiverPluginMetadata& metadata() const noexcept override {
    static constexpr SmReceiverPluginMetadata metadata{"wav"};
    return metadata;
  }

  void Probe(const std::string& selector,
             const SmReceiverCandidate& found) const override {
    ProbeDevices(selector, found);
  }

  void Capture(const SmReceiverCaptureRequest& cfg, const Samples& samples,
               const std::function<void()>& gap) const override {
    (void)gap;
    if (cfg.gain_db || cfg.correction_ppm)
      throw std::runtime_error(
          "IQ WAV does not support gain or PPM correction");
    ReadWavIq(cfg.device.c_str(), cfg.rate, samples);
  }
};
}  // namespace
}  // namespace skim

extern "C" SKIM_PLUGIN_EXPORT const skim::SmReceiverPlugin*
skim_receiver_v2() noexcept {
  static const skim::SmWavReceiverPlugin plugin;
  return &plugin;
}

extern "C" SKIM_PLUGIN_EXPORT const skim::SmReceiverPluginDescriptor*
skim_receiver_descriptor_v2() noexcept {
  static constexpr skim::SmReceiverPluginDescriptor descriptor{
      "wav:", "IQ WAV", "File or directory on server", true, false, true};
  return &descriptor;
}
