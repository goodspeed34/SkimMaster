#pragma once

#include <vector>

#include "core/settings.h"
#include "core/types.h"

namespace skim {
int SlotSamples(const std::string& mode);
void ResolveSlotFrequencies(std::vector<SmSpot>& spots, int64_t reference_hz);
// Decodes one complete slot of 12 kHz mono audio with the given settings, or
// the decoder's defaults when options is null.
std::vector<SmSpot> DecodeSlot(const std::string& mode, const char* path,
                               int64_t dial, int64_t utc_ms,
                               const SmSettingValues* options = nullptr);
}  // namespace skim
