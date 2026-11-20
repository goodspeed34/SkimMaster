#pragma once

#include <vector>

#include "core/types.h"

namespace skim {
std::vector<SmSpot> DecodeFtx(const std::string& mode,
                              const std::vector<float>& audio);
std::vector<SmSpot> DecodeWspr(const std::vector<float>& audio);
}  // namespace skim
