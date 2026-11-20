#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include "core/iq.h"
#include "core/settings.h"
#include "core/types.h"

namespace skim {
using TextSpot = std::function<void(SmSpot, uint64_t sample_index)>;
// Settings of the built-in CW (cw set) or RTTY decoder.
const std::vector<SmSetting>& TextDecoderSettings(bool cw);
// Receives the recent text of every active channel about once a second.
using TextChannels =
    std::function<void(std::vector<SmTextChannel>, uint64_t sample_index)>;
// The most characters of recent text kept per channel.
constexpr size_t kTextTail = 160;
// options holds a resolved value for every TextDecoderSettings(cw) key.
void DecodeText(bool cw, const SmSettingValues& options, const IqStream& stream,
                const TextSpot& emit, const std::function<void()>& gap,
                const TextChannels& channels = nullptr);
}  // namespace skim
