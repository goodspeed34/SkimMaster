#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace skim {
struct WsprDetails {
  int64_t dial;
  double dt;
  std::string power;
};
struct Spot {
  int64_t frequency;
  std::string message;
  std::string callsign;
  std::string grid;
  std::optional<double> snr{};
  std::optional<double> speed{};
  std::optional<WsprDetails> wspr{};
};
}  // namespace skim
