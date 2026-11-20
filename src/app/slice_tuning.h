#pragma once

#include <cstdint>
#include <cstdlib>
#include <nlohmann/json.hpp>
#include <string_view>

namespace skim {
inline bool IsSkimmer(std::string_view mode) {
  return mode == "CW" || mode == "RTTY";
}

inline bool IsTimedDecoder(std::string_view mode) {
  return mode == "FT8" || mode == "FT4" || mode == "WSPR";
}

inline int64_t SliceReference(const nlohmann::json& slice,
                              std::string_view mode, int64_t receiver_center) {
  if (IsSkimmer(mode)) {
    if (slice.contains("center_hz")) return slice.at("center_hz");
    // Interpret the old field as the slice center for existing TOML files.
    return slice.value("dial_hz", receiver_center);
  }
  return slice.at("dial_hz");
}
}  // namespace skim
