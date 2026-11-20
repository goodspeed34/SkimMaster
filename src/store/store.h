#pragma once

#include <vector>

#include "core/types.h"

namespace skim {
void SaveSlot(const char* database, const char* receiver, const char* mode,
              int64_t utc_ms, const std::vector<SmSpot>& spots,
              bool live = false);
}  // namespace skim
