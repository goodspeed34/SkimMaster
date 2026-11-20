#pragma once

#include <cstdint>
#include <functional>

#include "core/iq.h"
#include "core/types.h"

namespace skim {
using TextSpot = std::function<void(SmSpot, uint64_t sample_index)>;
void DecodeText(bool cw, const IqStream& stream, const TextSpot& emit,
                const std::function<void()>& gap);
}  // namespace skim
