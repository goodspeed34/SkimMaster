#pragma once

#include <cstdint>
#include <optional>

namespace skim {
struct SmProcessUsage {
  // User plus system CPU time consumed since the process started.
  double cpu_seconds;
  // Resident (Linux) or working-set (Windows) memory.
  int64_t memory_bytes;
};

// Samples a running process. Returns nothing when the process has exited or
// cannot be inspected.
std::optional<SmProcessUsage> SampleProcess(int64_t pid);

// The current process ID.
int64_t CurrentProcessId();

// Turns two samples into a CPU percentage of one core.
class SmCpuMeter {
 public:
  // Returns nothing until two samples are available.
  std::optional<double> Update(double cpu_seconds, double wall_seconds) {
    std::optional<double> percent;
    if (last_wall_ && wall_seconds > *last_wall_ && cpu_seconds >= last_cpu_)
      percent = 100 * (cpu_seconds - last_cpu_) / (wall_seconds - *last_wall_);
    last_cpu_ = cpu_seconds;
    last_wall_ = wall_seconds;
    return percent;
  }

 private:
  double last_cpu_ = 0;
  std::optional<double> last_wall_;
};
}  // namespace skim
