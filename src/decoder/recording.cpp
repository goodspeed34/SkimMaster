#include "decoder/recording.h"

#include <sndfile.h>

#include <complex>
#include <memory>
#include <stdexcept>
#include <utility>

#include "decoder/slot.h"
#include "decoder/text_modes.h"

namespace skim {
namespace {
bool IsTextMode(const std::string& mode) {
  return mode == "CW" || mode == "RTTY";
}
}  // namespace

int RecordingRate(const std::string& mode) {
  return mode == "CW" ? 48000 : 12000;
}

int RecordingChannels(const std::string& mode) {
  return IsTextMode(mode) ? 2 : 1;
}

SmRecordingDecode DecodeRecording(const std::string& mode, const char* path,
                                  int64_t reference_hz,
                                  const SmSettingValues& options) {
  if (mode != "CW" && mode != "RTTY" && mode != "FT8" && mode != "FT4" &&
      mode != "WSPR")
    throw std::runtime_error("select CW, RTTY, FT8, FT4 or WSPR");
  if (reference_hz <= 0 || reference_hz > 1000000000000LL)
    throw std::runtime_error("enter the recording's RF frequency");
  SmRecordingDecode result;
  if (!IsTextMode(mode)) {
    result.spots = DecodeSlot(mode, path, reference_hz, 0, &options);
    return result;
  }
  SF_INFO info{};
  std::unique_ptr<SNDFILE, decltype(&sf_close)> file(
      sf_open(path, SFM_READ, &info), sf_close);
  if (!file) throw std::runtime_error(sf_strerror(nullptr));
  const int rate = RecordingRate(mode);
  if (info.channels != 2 || info.samplerate != rate)
    throw std::runtime_error(mode + " needs stereo IQ at " +
                             std::to_string(rate) + " Hz");
  DecodeText(
      mode == "CW", options,
      [&](const Samples& samples, const std::function<void()>&) {
        std::vector<std::complex<float>> block(4096);
        sf_count_t frames;
        while ((frames = sf_readf_float(file.get(),
                                        reinterpret_cast<float*>(block.data()),
                                        block.size())) > 0)
          samples(block.data(), static_cast<size_t>(frames));
      },
      [&](SmSpot spot, uint64_t) {
        spot.frequency += reference_hz;
        result.spots.push_back(std::move(spot));
      },
      [] {},
      [&](std::vector<SmTextChannel> channels, uint64_t) {
        for (auto& channel : channels) channel.frequency += reference_hz;
        result.channels = std::move(channels);
      });
  return result;
}
}  // namespace skim
