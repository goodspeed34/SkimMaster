#include "decoder/settings.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>

#include "decoder/session.h"
#include "decoder/text_modes.h"

namespace skim {
const std::vector<SmSetting>& DecoderSettings(std::string_view mode) {
  static const std::vector<SmSetting> none;
  if (mode == "CW") return TextDecoderSettings(true);
  if (mode == "RTTY") return TextDecoderSettings(false);
  if (mode == "SSTV") return none;
  return SlotDecoderMetadata(std::string(mode)).settings;
}

bool SettingAccepts(const SmSetting& setting, double value) {
  if (!std::isfinite(value)) return false;
  switch (setting.kind) {
    case SmSettingKind::Boolean:
      return value == 0 || value == 1;
    case SmSettingKind::Choice:
      return std::any_of(
          setting.choices.begin(), setting.choices.end(),
          [&](double choice) { return std::abs(choice - value) < 1e-9; });
    case SmSettingKind::Integer:
      if (value != std::floor(value)) return false;
      [[fallthrough]];
    case SmSettingKind::Decimal:
      return value >= setting.min && value <= setting.max;
  }
  return false;
}

SmSettingValues ResolveDecoderSettings(std::string_view mode,
                                       const nlohmann::json& configured) {
  const auto prefix = "decoders." + std::string(mode) + ".";
  if (!configured.is_null() && !configured.is_object())
    throw std::runtime_error("decoders." + std::string(mode) +
                             " must be a table");
  const auto& schema = DecoderSettings(mode);
  SmSettingValues values;
  for (const auto& setting : schema)
    values[setting.key] = setting.default_value;
  if (configured.is_null()) return values;
  for (auto it = configured.begin(); it != configured.end(); ++it) {
    const auto found =
        std::find_if(schema.begin(), schema.end(),
                     [&](const SmSetting& s) { return s.key == it.key(); });
    if (found == schema.end())
      throw std::runtime_error("unknown decoder setting " + prefix + it.key());
    double value = 0;
    if (found->kind == SmSettingKind::Boolean && it->is_boolean())
      value = it->get<bool>() ? 1 : 0;
    else if (found->kind != SmSettingKind::Boolean && it->is_number())
      value = it->get<double>();
    else
      throw std::runtime_error("decoder setting " + prefix + it.key() +
                               " has the wrong type");
    if (!SettingAccepts(*found, value))
      throw std::runtime_error("decoder setting " + prefix + it.key() +
                               " is out of range");
    values[it.key()] = value;
  }
  return values;
}
}  // namespace skim
