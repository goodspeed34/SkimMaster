#include "decoder/slot.h"

#include <sndfile.h>

#include <memory>
#include <stdexcept>

namespace skim {
int SlotSamples(const std::string& mode) {
  if (mode != "FT4" && mode != "FT8" && mode != "WSPR")
    throw std::runtime_error("supported slot modes are FT4, FT8 and WSPR");
  return mode == "WSPR" ? 1440000 : (mode == "FT8" ? 180000 : 90000);
}

std::vector<SmSpot> DecodeSlot(const std::string& mode, const char* path,
                               int64_t dial, int64_t utc_ms) {
  SF_INFO info{};
  std::unique_ptr<SNDFILE, decltype(&sf_close)> file(
      sf_open(path, SFM_READ, &info), sf_close);
  if (!file) throw std::runtime_error(sf_strerror(nullptr));
  const int samples = SlotSamples(mode);
  if (info.channels != 1 || info.samplerate != 12000 || info.frames != samples)
    throw std::runtime_error("expected one complete slot of 12 kHz mono audio");
  std::vector<float> audio(samples);
  if (sf_readf_float(file.get(), audio.data(), samples) != samples)
    throw std::runtime_error("short audio read");

  return DecodeAudio(mode, std::move(audio), dial, utc_ms);
}

}  // namespace skim
