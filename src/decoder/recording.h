#pragma once

#include <string>
#include <vector>

#include "core/settings.h"
#include "core/types.h"

namespace skim {
struct SmRecordingDecode {
  std::vector<SmSpot> spots;
  // The final text of each CW or RTTY channel; empty for slot modes.
  std::vector<SmTextChannel> channels;
};

// The sample rate and channel count DecodeRecording expects for a mode.
int RecordingRate(const std::string& mode);
int RecordingChannels(const std::string& mode);

// Decodes a recording on demand without storing or reporting anything.
// FT8, FT4 and WSPR take one complete slot of 12 kHz mono audio and a USB
// dial. CW takes 48 kHz and RTTY 12 kHz stereo IQ (I left, Q right) and the
// RF center. Frequencies in the result are RF.
SmRecordingDecode DecodeRecording(const std::string& mode, const char* path,
                                  int64_t reference_hz,
                                  const SmSettingValues& options);
}  // namespace skim
