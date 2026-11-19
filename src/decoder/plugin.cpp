#include <cmath>
#include <cstring>

#include "decoder/api.h"
#include "decoder/engines.h"

namespace skim {
namespace {
int Process(const char* mode, const float* audio, uint32_t count, int64_t dial,
            skim_emit_result emit, void* user) {
  if (!mode || !audio || !emit) return -1;
  try {
#ifdef SKIM_WSPR
    if (std::strcmp(mode, "WSPR") || count != 1440000) return -1;
    auto spots = DecodeWspr({audio, audio + count}, dial);
#else
    if ((std::strcmp(mode, "FT8") || count != 180000) &&
        (std::strcmp(mode, "FT4") || count != 90000))
      return -1;
    auto spots = DecodeFtx(mode, {audio, audio + count}, dial);
#endif
    for (const auto& s : spots) {
      skim_decode_result r{s.frequency,
                           s.message.c_str(),
                           s.callsign.c_str(),
                           s.grid.c_str(),
                           s.wspr ? s.wspr->power.c_str() : nullptr,
                           s.snr.value_or(NAN),
                           s.wspr ? s.wspr->dt : 0};
      if (emit(user, &r)) return -1;
    }
    return 0;
  } catch (...) {
    return -1;
  }
}
}  // namespace
}  // namespace skim
#ifdef _WIN32
#define SKIM_EXPORT __declspec(dllexport)
#else
#define SKIM_EXPORT __attribute__((visibility("default")))
#endif
extern "C" SKIM_EXPORT const skim_slot_api* skim_decoder_v1() {
  static const skim_slot_api api{1, sizeof(skim_slot_api), skim::Process};
  return &api;
}
