#pragma once

#include <functional>
#include <memory>

#include "decoder/api.h"

namespace skim {
struct SmDecoderRoute {
  std::string decoder_id;
  std::string result_endpoint;
  const std::atomic_bool& stop;
};

class SmSlotDecoderSession {
 public:
  SmSlotDecoderSession(const std::string& mode, const SmDecoderRoute& route,
                       int sample_rate, SmSampleType sample_type);
  ~SmSlotDecoderSession();

  bool Submit(SmDecoderSlot slot, bool wait);
  void Finish();

 private:
  struct SmState;
  std::unique_ptr<SmState> state_;
};

void CollectDecoderResults(
    const std::function<void(const std::string&, const std::atomic_bool&)>&
        produce,
    const std::function<void(SmDecoderResult)>& collect);

void ResolveSlotFrequencies(std::vector<SmSpot>& spots, int64_t reference_hz);
}  // namespace skim
