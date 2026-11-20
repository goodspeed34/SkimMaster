#pragma once

#include <cstdint>
#include <string_view>

namespace skim {
struct SmBand {
  std::string_view name;
  int64_t low_hz;
  int64_t high_hz;
};

// Amateur allocations, taken as the union across the three ITU regions so a
// frequency is named wherever the station is.
inline constexpr SmBand kAmateurBands[] = {
    {"2200m", 135700, 137800},        {"630m", 472000, 479000},
    {"160m", 1800000, 2000000},       {"80m", 3500000, 4000000},
    {"60m", 5250000, 5450000},        {"40m", 7000000, 7300000},
    {"30m", 10100000, 10150000},      {"20m", 14000000, 14350000},
    {"17m", 18068000, 18168000},      {"15m", 21000000, 21450000},
    {"12m", 24890000, 24990000},      {"10m", 28000000, 29700000},
    {"6m", 50000000, 54000000},       {"4m", 70000000, 70500000},
    {"2m", 144000000, 148000000},     {"1.25m", 222000000, 225000000},
    {"70cm", 420000000, 450000000},   {"33cm", 902000000, 928000000},
    {"23cm", 1240000000, 1300000000},
};

// Returns the amateur band holding hz, or an empty name outside them.
inline std::string_view BandName(int64_t hz) {
  for (const auto& band : kAmateurBands)
    if (hz >= band.low_hz && hz <= band.high_hz) return band.name;
  return {};
}
}  // namespace skim
