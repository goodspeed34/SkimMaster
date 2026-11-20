#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace skim {
struct SmWsprDetails {
  int64_t dial;
  double dt;
  std::string power;
};
struct SmSpot {
  int64_t frequency;
  std::string message;
  std::string callsign;
  std::string grid;
  std::optional<double> snr{};
  std::optional<double> speed{};
  std::optional<SmWsprDetails> wspr{};
};
// The recent text of one CW or RTTY channel. frequency is an offset from the
// slice center until the receiver resolves it to RF.
struct SmTextChannel {
  int64_t frequency;
  std::string text;
  std::optional<double> snr{};
  // WPM for CW, baud for RTTY.
  std::optional<double> speed{};
};
}  // namespace skim
