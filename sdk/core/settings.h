#pragma once

#include <map>
#include <string>
#include <vector>

namespace skim {
enum class SmSettingKind { Integer, Decimal, Boolean, Choice };

// One operator-adjustable setting a plugin declares. The host validates the
// station TOML against it, renders it in the panel and passes the resolved
// value back to the plugin. Booleans use 0 and 1.
struct SmSetting {
  std::string key;
  std::string label;
  std::string help;
  SmSettingKind kind = SmSettingKind::Integer;
  double default_value = 0;
  double min = 0;
  double max = 0;
  // Allowed values for Choice settings.
  std::vector<double> choices;
};

// Resolved settings: every declared key is present, using its default when
// the station TOML leaves it out.
using SmSettingValues = std::map<std::string, double>;
}  // namespace skim
