#include "decoder/slot_plugin.h"

namespace {
std::vector<skim::SmSpot> DecodeFixture(skim::SmSlotMode,
                                        const std::vector<float>&,
                                        std::mutex&) {
  return {{1234, "CQ External", "K1SDK", "FN42"}};
}
}  // namespace

extern "C" SKIM_PLUGIN_EXPORT const skim::SmSlotDecoderPlugin*
skim_slot_decoder_v2(skim::SmSlotMode mode) {
  using namespace skim;
  static const SmBuiltinSlotDecoder decoder(
      {"External fixture", "1", "SkimMaster tests", "", SmSlotMode::Ft8, 12000,
       12000, SmSampleType::MonoAudio},
      DecodeFixture);
  return mode == SmSlotMode::Ft8 ? &decoder : nullptr;
}
