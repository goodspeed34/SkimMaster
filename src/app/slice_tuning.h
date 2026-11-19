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
inline bool SliceFits(const nlohmann::json& slice, int64_t center, int rate) {
  const std::string mode = slice.at("mode");
  const auto rf =
      SliceReference(slice, mode, center) + (IsTimedDecoder(mode) ? 1500 : 0);
  const int width = mode == "CW" ? 48000 : 12000;
  return 2 * std::abs(rf - center) + width <= rate;
}
}  // namespace skim
