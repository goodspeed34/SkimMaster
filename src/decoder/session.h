#pragma once

#include <functional>
#include <memory>

#include "decoder/api.h"

namespace skim {
struct DecoderRoute {
  std::string decoder_id;
  std::string result_endpoint;
  const std::atomic_bool& stop;
};

class SlotDecoderSession {
 public:
  SlotDecoderSession(const std::string& mode, const DecoderRoute& route,
                     int sample_rate, SampleType sample_type);
  ~SlotDecoderSession();

  bool Submit(DecoderSlot slot, bool wait);
  void Finish();

 private:
  struct State;
  std::unique_ptr<State> state_;
};

void CollectDecoderResults(
    const std::function<void(const std::string&, const std::atomic_bool&)>&
        produce,
    const std::function<void(DecoderResult)>& collect);

void ResolveSlotFrequencies(std::vector<Spot>& spots, int64_t reference_hz);
}  // namespace skim
