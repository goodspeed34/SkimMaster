#include "core/process_usage.h"

#ifdef _WIN32
#include <windows.h>
// psapi.h needs windows.h first.
#include <psapi.h>
#else
#include <unistd.h>

#include <fstream>
#include <sstream>
#include <string>
#endif

namespace skim {
#ifdef _WIN32
std::optional<SmProcessUsage> SampleProcess(int64_t pid) {
  HANDLE process =
      OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE,
                  static_cast<DWORD>(pid));
  if (!process) return std::nullopt;
  std::optional<SmProcessUsage> usage;
  FILETIME created, exited, kernel, user;
  PROCESS_MEMORY_COUNTERS memory{};
  DWORD code = 0;
  if (GetExitCodeProcess(process, &code) && code == STILL_ACTIVE &&
      GetProcessTimes(process, &created, &exited, &kernel, &user) &&
      GetProcessMemoryInfo(process, &memory, sizeof(memory))) {
    auto ticks = [](const FILETIME& time) {
      return (static_cast<uint64_t>(time.dwHighDateTime) << 32) |
             time.dwLowDateTime;
    };
    // FILETIME counts 100 ns intervals.
    usage = SmProcessUsage{(ticks(kernel) + ticks(user)) / 1e7,
                           static_cast<int64_t>(memory.WorkingSetSize)};
  }
  CloseHandle(process);
  return usage;
}

int64_t CurrentProcessId() { return GetCurrentProcessId(); }
#else
std::optional<SmProcessUsage> SampleProcess(int64_t pid) {
  const auto base = "/proc/" + std::to_string(pid);
  std::ifstream stat_file(base + "/stat");
  std::string stat;
  if (!std::getline(stat_file, stat)) return std::nullopt;
  // The command name is parenthesized and may contain spaces, so parse the
  // fields after its closing parenthesis. They start with field 3 (state).
  const auto close = stat.rfind(')');
  if (close == std::string::npos) return std::nullopt;
  std::istringstream fields(stat.substr(close + 2));
  std::string state;
  fields >> state;
  if (state == "Z" || state == "X") return std::nullopt;
  std::string skipped;
  for (int field = 4; field < 14; ++field) fields >> skipped;
  unsigned long long user = 0, system = 0;
  if (!(fields >> user >> system)) return std::nullopt;

  std::ifstream statm(base + "/statm");
  long long size = 0, resident = 0;
  if (!(statm >> size >> resident)) return std::nullopt;
  const double ticks = static_cast<double>(sysconf(_SC_CLK_TCK));
  const long long page = sysconf(_SC_PAGESIZE);
  return SmProcessUsage{(user + system) / ticks, resident * page};
}

int64_t CurrentProcessId() { return getpid(); }
#endif
}  // namespace skim
