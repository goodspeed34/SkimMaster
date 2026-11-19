#pragma once

#include <vector>

#include "core/types.h"

namespace skim {
std::vector<Spot> DecodeFtx(const std::string& mode,
                            const std::vector<float>& audio, int64_t dial);
std::vector<Spot> DecodeWspr(const std::vector<float>& audio, int64_t dial);
}  // namespace skim
