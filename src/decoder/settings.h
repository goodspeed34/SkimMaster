#pragma once

#include <nlohmann/json.hpp>
#include <string_view>
#include <vector>

#include "core/settings.h"

namespace skim {
// The settings the decoder for a mode declares. CW and RTTY are built in;
// FT8, FT4 and WSPR come from the installed slot decoder plugin. SSTV has
// none. Unknown modes and missing plugins throw.
const std::vector<SmSetting>& DecoderSettings(std::string_view mode);

// Checks configured values (a [decoders.<MODE>] table) against the schema
// and fills in defaults. Throws on unknown keys or out-of-range values.
SmSettingValues ResolveDecoderSettings(std::string_view mode,
                                       const nlohmann::json& configured);

// Checks one value against a setting's kind and range.
bool SettingAccepts(const SmSetting& setting, double value);
}  // namespace skim
