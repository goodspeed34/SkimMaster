#include <boost/dll/runtime_symbol_info.hpp>
#include <boost/dll/shared_library.hpp>
#include <cmath>
#include <stdexcept>

#include "decoder/api.h"
#include "decoder/slot.h"
#include "dsp/fft_lock.h"

namespace skim {
std::vector<Spot> DecodeAudio(const std::string& mode,
                              const std::vector<float>& audio, int64_t dial) {
  if (audio.size() != static_cast<size_t>(SlotSamples(mode)))
    throw std::runtime_error("incomplete slot");
  std::string name = mode == "WSPR" ? "decoderwspr" : "decoderftx";
  auto path = boost::dll::program_location().parent_path() /
              ("lib" + name + SKIM_MODULE_SUFFIX);
  boost::dll::shared_library plugin(path);
  auto api = plugin.get<const skim_slot_api*()>("skim_decoder_v1")();
  if (!api || api->version != 1 || api->size < sizeof(*api) ||
      !api->process_slot)
    throw std::runtime_error("unsupported slot decoder ABI");
  struct Result {
    std::vector<Spot> spots;
    int64_t dial;
  };
  Result result{{}, dial};
  auto emit = [](void* context, const skim_decode_result* r) {
    try {
      auto& out = *static_cast<Result*>(context);
      Spot s{r->frequency, r->message, r->call, r->grid};
      if (std::isfinite(r->snr)) s.snr = r->snr;
      if (r->power) s.wspr = WsprDetails{out.dial, r->dt, r->power};
      out.spots.push_back(std::move(s));
      return 0;
    } catch (...) {
      return -1;
    }
  };
  std::unique_lock<std::mutex> planner(FftPlannerMutex(), std::defer_lock);
  if (mode == "WSPR") planner.lock();
  if (api->process_slot(mode.c_str(), audio.data(), audio.size(), dial, emit,
                        &result))
    throw std::runtime_error("slot decoder failed");
  return result.spots;
}
}  // namespace skim
