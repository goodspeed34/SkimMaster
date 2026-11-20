#pragma once

#include <vector>

#include "core/types.h"

namespace skim {
int SlotSamples(const std::string& mode);
std::vector<Spot> DecodeAudio(const std::string& mode, std::vector<float> audio,
                              int64_t dial, int64_t utc_ms);
std::vector<Spot> DecodeSlot(const std::string& mode, const char* path,
                             int64_t dial, int64_t utc_ms);
}  // namespace skim
