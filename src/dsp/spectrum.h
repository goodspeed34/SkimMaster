#pragma once

#include <complex>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace skim {
// Points per FFT frame and display bins per spectrum row.
constexpr int kSpectrumFft = 2048;
constexpr int kSpectrumBins = 512;

// The noise power of one FFT bin: the median of the central 80% of bins
// after dropping bins more than 6 dB above their median, which excludes
// carriers and the anti-aliasing roll-off. Returns 0 for an empty input.
double NoiseFloor(const std::vector<double>& power);

// Publishes a once-a-second averaged spectrum of a receiver's IQ to the
// spectra table and one-minute noise-floor averages to noise_floor. Levels
// are relative to a full-scale complex sinusoid (dBFS); the noise floor is a
// density in dBFS/Hz, so receivers with different rates compare directly.
class SmSpectrum {
 public:
  SmSpectrum(std::string database, std::string receiver, int rate,
             int64_t center);
  SmSpectrum(SmSpectrum&&) noexcept;
  SmSpectrum& operator=(SmSpectrum&&) noexcept;
  ~SmSpectrum();

  // utc_seconds defaults to the system clock; tests pass their own.
  void Observe(const std::complex<float>* samples, size_t count,
               int64_t utc_seconds = -1);

 private:
  struct SmState;
  std::unique_ptr<SmState> state_;
};
}  // namespace skim
