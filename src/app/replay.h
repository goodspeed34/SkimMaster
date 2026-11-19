#pragma once

#include <cstdint>
#include <iosfwd>

namespace skim {
// Offline fixture profile: 48 kHz IQ at 14.070 MHz, sliced at +6 kHz to 12 kHz.
void RunReplay(std::istream& input, std::ostream& output,
               int64_t receiver_hz = 14070000, int64_t slice_hz = 14076000);
}  // namespace skim
