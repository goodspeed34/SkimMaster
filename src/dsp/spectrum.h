#pragma once
#include <array>
#include <complex>
#include <cstdint>
#include <string>
namespace skim {
class SmSpectrum {
 public:
  SmSpectrum(std::string database, std::string receiver, int rate,
             int64_t center);
  void Observe(const std::complex<float>* samples, size_t count);

 private:
  std::string database_, receiver_;
  int rate_;
  int64_t center_;
  int64_t next_ = 0;
  std::array<double, 256> window_{};
};
void InitSpectrum(const char* db);
}  // namespace skim
