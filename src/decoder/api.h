#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
// Strings and sample data are borrowed for the duration of the call.
typedef struct {
  int64_t frequency;
  const char* message;
  const char* call;
  const char* grid;
  const char* power;
  double snr;
  double dt;
} skim_decode_result;
typedef int (*skim_emit_result)(void*, const skim_decode_result*);
typedef struct {
  uint32_t version;
  uint32_t size;
  int (*process_slot)(const char* mode, const float* audio, uint32_t samples,
                      int64_t dial, skim_emit_result emit, void* user);
} skim_slot_api;
#ifdef __cplusplus
}
#endif
