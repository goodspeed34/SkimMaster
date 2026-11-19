#pragma once
#include <mutex>
namespace skim {
// FFTW planning and plan destruction share process-global state.
inline std::mutex& FftPlannerMutex() {
  static std::mutex mutex;
  return mutex;
}
}  // namespace skim
