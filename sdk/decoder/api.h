#pragma once

#include <readerwritercircularbuffer.h>

#include <atomic>
#include <complex>
#include <future>
#include <mutex>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "core/settings.h"
#include "core/types.h"

namespace skim {
enum class SmSlotMode { Ft4, Ft8, Wspr };
enum class SmSampleType { IQ, MonoAudio };

struct SmSlotDecoderMetadata {
  std::string decoder_name;
  std::string decoder_version;
  std::string decoder_author;
  std::string decoder_website;
  SmSlotMode supported_mode;
  long long min_samplerate;
  long long max_samplerate;
  SmSampleType required_type;
  // Settings the operator can change in [decoders.<MODE>].
  std::vector<SmSetting> settings;
};

struct SmDecoderSlot {
  int64_t utc_ms;
  std::variant<std::vector<float>, std::vector<std::complex<float>>> samples;
};

using DecoderInput = moodycamel::BlockingReaderWriterCircularBuffer<
    std::optional<SmDecoderSlot>>;

class SmSlotDecoderPlugin {
 public:
  virtual ~SmSlotDecoderPlugin() = default;
  virtual const SmSlotDecoderMetadata& metadata() const noexcept = 0;
  // options holds a value for every declared setting.
  virtual std::future<void> start(std::string decoder_id, DecoderInput& input,
                                  std::string result_endpoint,
                                  const std::atomic_bool& stop,
                                  std::mutex& fft_planner,
                                  SmSettingValues options) const = 0;
};

struct SmDecoderResult {
  std::string decoder_id;
  int64_t utc_ms;
  std::vector<SmSpot>
      spots;  // Frequencies are signed offsets from the slice reference.
};
}  // namespace skim
