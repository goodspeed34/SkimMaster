#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
  const char* device;
  int64_t center_hz;
  uint32_t rate;
  double gain_db;
  double correction_ppm;
} skim_radio_config;
// Null samples indicate a discontinuity. Returning nonzero aborts capture.
typedef int (*skim_iq_callback)(void*, const float* iq, uint32_t count);
typedef struct {
  uint32_t version;
  uint32_t size;
  int (*capture)(const skim_radio_config*, skim_iq_callback, void*, char* error,
                 uint32_t capacity);
} skim_radio_api;
// The selector is backend-specific. Strings are valid during the callback.
// A zero rate means discovery could not determine a sample rate.
typedef int (*skim_receiver_found)(void*, const char* device, const char* label,
                                   uint32_t rate);
typedef int (*skim_receiver_probe)(const char* selector, skim_receiver_found,
                                   void*, char* error, uint32_t capacity);
#ifdef __cplusplus
}
#endif
