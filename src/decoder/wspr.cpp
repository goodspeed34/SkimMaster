#include <array>
#include <cmath>
#include <stdexcept>
#include <vector>

#include "decoder/slot_plugin.h"
extern "C" {
#include <libcsdr.h>
#include <wsprd/wsprd.h>
}

namespace skim {
namespace {
std::vector<SmSpot> DecodeWspr(SmSlotMode, const std::vector<float>& audio,
                               std::mutex& fft_planner) {
  std::lock_guard<std::mutex> lock(fft_planner);
  if (audio.size() != 1440000) throw std::runtime_error("incomplete slot");
  // Translate the WSPR USB passband (dial + 1500 Hz) and decimate to 375 Hz.
  constexpr int taps_count = 513, decimation = 32;
  std::array<float, taps_count> taps{};
  firdes_lowpass_f(taps.data(), taps.size(), 145.0f / 12000, WINDOW_HAMMING);
  std::vector<complexf> mixed(audio.size() + taps_count - 1);
  for (size_t i = 0; i < audio.size(); ++i) {
    auto phase = -2 * 3.14159265358979323846 * (i % 8) / 8;
    mixed[i + taps_count / 2] = {
        static_cast<float>(audio[i] * std::cos(phase)),
        static_cast<float>(audio[i] * std::sin(phase))};
  }
  std::vector<complexf> baseband(audio.size() / decimation);
  int n = fir_decimate_cc(mixed.data(), baseband.data(), mixed.size(),
                          decimation, taps.data(), taps.size());
  std::vector<float> re(n), im(n);
  double energy = 0;
  for (int i = 0; i < n; ++i) {
    re[i] = baseband[i].i;
    im[i] = baseband[i].q;
    energy += re[i] * re[i] + im[i] * im[i];
  }
  if (!std::isfinite(energy) || energy <= 0) return {};
  decoder_options options{};
  options.npasses = 2;
  options.subtraction = 1;
  std::array<decoder_results, MAX_UNIQUES> decoded{};
  int count = 0;
  if (wspr_decode(re.data(), im.data(), n, options, decoded.data(), &count))
    throw std::runtime_error("WSPR decoder initialization failed");
  std::vector<SmSpot> spots;
  for (int i = 0; i < count; ++i) {
    const auto& r = decoded[i];
    spots.push_back({std::llround(r.freq * 1e6),
                     r.message,
                     r.call,
                     r.loc,
                     r.snr,
                     {},
                     SmWsprDetails{0, r.dt, r.pwr}});
  }
  return spots;
}
}  // namespace
}  // namespace skim

extern "C" SKIM_SLOT_DECODER_EXPORT const skim::SmSlotDecoderPlugin*
skim_slot_decoder_v2(skim::SmSlotMode mode) {
  using namespace skim;
  static const SmBuiltinSlotDecoder wspr(
      {"WSPR", SKIM_VERSION, "Guenael Jouchet (rtlsdr-wsprd)",
       "https://github.com/Guenael/rtlsdr-wsprd", SmSlotMode::Wspr, 12000,
       12000, SmSampleType::MonoAudio},
      DecodeWspr);
  return mode == SmSlotMode::Wspr ? &wspr : nullptr;
}
