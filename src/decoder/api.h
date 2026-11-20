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

#include "core/types.h"

namespace skim {
enum class SlotMode { Ft4, Ft8, Wspr };
enum class SampleType { IQ, MonoAudio };

struct SlotDecoderMetadata {
  std::string decoder_name;
  std::string decoder_version;
  std::string decoder_author;
  std::string decoder_website;
  SlotMode supported_mode;
  long long min_samplerate;
  long long max_samplerate;
  SampleType required_type;
};

struct DecoderSlot {
  int64_t utc_ms;
  std::variant<std::vector<float>, std::vector<std::complex<float>>> samples;
};

using DecoderInput =
    moodycamel::BlockingReaderWriterCircularBuffer<std::optional<DecoderSlot>>;

class SlotDecoder {
 public:
  virtual ~SlotDecoder() = default;
  virtual const SlotDecoderMetadata& metadata() const noexcept = 0;
  virtual std::future<void> start(std::string decoder_id, DecoderInput& input,
                                  std::string result_endpoint,
                                  const std::atomic_bool& stop,
                                  std::mutex& fft_planner) const = 0;
};

struct DecoderResult {
  std::string decoder_id;
  int64_t utc_ms;
  std::vector<Spot>
      spots;  // Frequencies are signed offsets from the slice reference.
};
}  // namespace skim
