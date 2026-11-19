#pragma once

#include <vector>

#include "core/types.h"

namespace skim {
int SlotSamples(const std::string& mode);
std::vector<Spot> DecodeAudio(const std::string& mode,
                              const std::vector<float>& audio, int64_t dial);
std::vector<Spot> DecodeSlot(const std::string& mode, const char* path,
                             int64_t dial);
}  // namespace skim
