#pragma once

#include <vector>

#include "core/types.h"

namespace skim {
int SlotSamples(const std::string& mode);
void ResolveSlotFrequencies(std::vector<SmSpot>& spots, int64_t reference_hz);
std::vector<SmSpot> DecodeSlot(const std::string& mode, const char* path,
                               int64_t dial, int64_t utc_ms);
}  // namespace skim
