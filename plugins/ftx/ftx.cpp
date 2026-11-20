#include <common/monitor.h>
#include <ft8/message.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <stdexcept>
#include <utility>

#include "decoder/slot_plugin.h"

namespace skim {
namespace {
std::string Field(const std::string& text, const ftx_message_offsets_t& fields,
                  int index, ftx_field_t type) {
  if (fields.types[index] != type || fields.offsets[index] < 0) return {};
  size_t pos = fields.offsets[index];
  if (pos >= text.size()) return {};
  return text.substr(pos, text.find(' ', pos) - pos);
}

std::vector<SmSetting> FtxSettings() {
  using Kind = SmSettingKind;
  return {
      {"min_hz", "Lowest audio frequency (Hz)",
       "Lower edge of the search above the USB dial.", Kind::Integer, 200, 0,
       2900},
      {"max_hz", "Highest audio frequency (Hz)",
       "Upper edge of the search above the USB dial. Live slices pass "
       "audio up to 3000 Hz.",
       Kind::Integer, 3000, 300, 3000},
      {"max_candidates", "Candidates per slot",
       "Sync peaks examined per slot. More find weaker signals in crowded "
       "bands at a higher CPU cost.",
       Kind::Integer, 140, 10, 1000},
      {"min_score", "Minimum sync score",
       "Candidates below this Costas sync score are skipped. Lower values try "
       "weaker signals.",
       Kind::Integer, 10, 0, 100},
      {"ldpc_iterations", "LDPC iterations",
       "Belief-propagation iterations per candidate.", Kind::Integer, 25, 5,
       100},
  };
}

std::vector<SmSpot> DecodeFtx(SmSlotMode mode, const std::vector<float>& audio,
                              std::mutex&, const SmSettingValues& options) {
  int samples = mode == SmSlotMode::Ft8 ? 180000 : 90000;
  if (audio.size() != static_cast<size_t>(samples))
    throw std::runtime_error("incomplete slot");

  monitor_t monitor{};
  const auto min_hz = static_cast<float>(options.at("min_hz"));
  const auto max_hz = static_cast<float>(options.at("max_hz"));
  if (min_hz >= max_hz)
    throw std::runtime_error("FT8/FT4 min_hz must be below max_hz");
  monitor_config_t config{
      min_hz, max_hz,
      12000,  2,
      2,      mode == SmSlotMode::Ft8 ? FTX_PROTOCOL_FT8 : FTX_PROTOCOL_FT4};
  monitor_init(&monitor, &config);
  std::unique_ptr<monitor_t, decltype(&monitor_free)> cleanup(&monitor,
                                                              monitor_free);
  // ft8_lib searches -10 to +19 symbols around the waterfall origin.
  // Offset the FT4 origin by nine symbols to include late starts within a slot.
  int origin = mode == SmSlotMode::Ft4 ? 9 * monitor.block_size : 0;
  for (int i = origin; i + monitor.block_size <= samples;
       i += monitor.block_size)
    monitor_process(&monitor, audio.data() + i);
  std::vector<ftx_candidate_t> candidates(
      static_cast<size_t>(options.at("max_candidates")));
  int count =
      ftx_find_candidates(&monitor.wf, candidates.size(), candidates.data(),
                          static_cast<int>(options.at("min_score")));
  const int iterations = static_cast<int>(options.at("ldpc_iterations"));
  std::vector<SmSpot> spots;

  for (int i = 0; i < count; ++i) {
    const auto& c = candidates[i];
    ftx_message_t message{};
    ftx_decode_status_t status{};
    if (!ftx_decode_candidate(&monitor.wf, &c, iterations, &message, &status))
      continue;
    char text[FTX_MAX_MESSAGE_LENGTH]{};
    ftx_message_offsets_t fields{};
    if (ftx_message_decode(&message, nullptr, text, &fields) !=
        FTX_MESSAGE_RC_OK)
      continue;
    auto hz =
        std::llround((monitor.min_bin + c.freq_offset +
                      static_cast<double>(c.freq_sub) / monitor.wf.freq_osr) /
                     monitor.symbol_period);
    // Candidates are sorted strongest first. Neighboring FFT bins often
    // recover the same transmission.
    if (std::none_of(spots.begin(), spots.end(), [&](const SmSpot& spot) {
          return spot.message == text &&
                 std::abs(spot.frequency - hz) <= 1.0 / monitor.symbol_period;
        }))
      spots.push_back({hz, text, Field(text, fields, 1, FTX_FIELD_CALL),
                       Field(text, fields, 2, FTX_FIELD_GRID)});
  }
  return spots;
}
}  // namespace
}  // namespace skim

extern "C" SKIM_PLUGIN_EXPORT const skim::SmSlotDecoderPlugin*
skim_slot_decoder_v3(skim::SmSlotMode mode) {
  using namespace skim;
  static const SmBuiltinSlotDecoder ft4(
      {"FT4", SKIM_VERSION, "Karlis Goba (ft8_lib)",
       "https://github.com/kgoba/ft8_lib", SmSlotMode::Ft4, 12000, 12000,
       SmSampleType::MonoAudio, FtxSettings()},
      DecodeFtx);
  static const SmBuiltinSlotDecoder ft8(
      {"FT8", SKIM_VERSION, "Karlis Goba (ft8_lib)",
       "https://github.com/kgoba/ft8_lib", SmSlotMode::Ft8, 12000, 12000,
       SmSampleType::MonoAudio, FtxSettings()},
      DecodeFtx);
  if (mode == SmSlotMode::Ft4) return &ft4;
  if (mode == SmSlotMode::Ft8) return &ft8;
  return nullptr;
}
