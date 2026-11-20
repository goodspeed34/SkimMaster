#include "receiver/receiver.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>

#include "core/plugin_modules.h"

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif
namespace skim {
namespace {
using SmReceiverFactory = const SmReceiverPlugin*() noexcept;
using SmReceiverDescriptorFactory =
    const SmReceiverPluginDescriptor*() noexcept;

std::vector<SmReceiverDriver> DiscoverReceivers() {
  std::vector<SmReceiverDriver> drivers;
  for (const auto& path : PluginModules("librcvr")) {
    boost::dll::fs::error_code error;
    boost::dll::shared_library module(path, error);
    if (error || !module.has("skim_receiver_v3") ||
        !module.has("skim_receiver_descriptor_v3"))
      continue;
    const auto* plugin = module.get<SmReceiverFactory>("skim_receiver_v3")();
    const auto* descriptor = module.get<SmReceiverDescriptorFactory>(
        "skim_receiver_descriptor_v3")();
    if (!plugin || !descriptor) continue;
    const auto id = plugin->metadata().driver_id;
    if (id.empty() || descriptor->label.empty() ||
        (!descriptor->prefix.empty() && descriptor->prefix.back() != ':') ||
        (descriptor->recording && descriptor->prefix.empty()))
      continue;
    const auto duplicate =
        std::find_if(drivers.begin(), drivers.end(), [&](const auto& driver) {
          return driver.id == id || driver.prefix == descriptor->prefix;
        });
    if (duplicate != drivers.end()) continue;
    drivers.push_back({std::string(id), std::string(descriptor->prefix),
                       std::string(descriptor->label),
                       std::string(descriptor->probe_hint),
                       descriptor->recording, descriptor->adjustable_gain,
                       descriptor->probe_from_device, descriptor->reports_state,
                       std::filesystem::path(path.native())});
  }
  std::stable_partition(drivers.begin(), drivers.end(), [](const auto& driver) {
    return driver.prefix.empty();
  });
  return drivers;
}

const SmReceiverDriver* FindExplicitDriver(std::string_view device) {
  const SmReceiverDriver* match = nullptr;
  for (const auto& driver : ReceiverDrivers())
    if (!driver.prefix.empty() && device.rfind(driver.prefix, 0) == 0 &&
        (!match || driver.prefix.size() > match->prefix.size()))
      match = &driver;
  return match;
}

std::filesystem::path ResolveFilePath(std::string_view path,
                                      const std::filesystem::path& base = {});

// Copy backend errors before the module is unloaded. A callback can throw
// a host exception to stop capture, which must reach the original caller.
template <typename Operation>
void InvokePlugin(const Operation& operation, const char* fallback,
                  const std::exception_ptr& callback_error = {}) {
  try {
    operation();
  } catch (const std::exception& e) {
    if (!callback_error) throw std::runtime_error(e.what());
  } catch (...) {
    if (!callback_error) throw std::runtime_error(fallback);
  }
  if (callback_error) std::rethrow_exception(callback_error);
}

std::string DescribeState(const SmReceiverState& state) {
  std::ostringstream text;
  text << "Device reports";
  if (state.center_hz) text << " center " << *state.center_hz << " Hz";
  if (state.rate) text << " rate " << *state.rate << " Hz";
  if (state.gain_db) text << " gain " << *state.gain_db << " dB";
  if (!state.center_hz && !state.rate && !state.gain_db) text << " no tuning";
  return text.str();
}
}  // namespace

std::string StateMismatch(const SmReceiverCaptureRequest& request,
                          const SmReceiverState& state) {
  if (state.center_hz && *state.center_hz != request.center_hz)
    return "the center returns to " + std::to_string(request.center_hz) + " Hz";
  if (state.rate && *state.rate != request.rate)
    return "the rate returns to " + std::to_string(request.rate) + " Hz";
  return {};
}

namespace {
class SmReceiverSession {
 public:
  explicit SmReceiverSession(const SmReceiverDriver& driver)
      : module_(boost::dll::fs::path(driver.module_path.native())) {
    if (!module_.has("skim_receiver_v3"))
      throw std::runtime_error(
          "receiver module does not export skim_receiver_v3");
    plugin_ = module_.get<SmReceiverFactory>("skim_receiver_v3")();
    if (!plugin_ || plugin_->metadata().driver_id != driver.id)
      throw std::runtime_error("receiver plugin driver ID mismatch");
  }

  void Probe(const std::string& selector, const SmReceiverCandidate& found) {
    InvokePlugin([&] { plugin_->Probe(selector, found); },
                 "receiver probe failed");
  }

  void Capture(const SmReceiverCaptureRequest& request, const Samples& consume,
               const std::function<void()>& gap) {
    std::exception_ptr callback_error;
    auto call = [&](const auto& callback) {
      if (callback_error) std::rethrow_exception(callback_error);
      try {
        callback();
      } catch (...) {
        callback_error = std::current_exception();
        throw;
      }
    };
    // IQ is dropped while the device reports tuning other than requested.
    bool off_plan = false;
    std::optional<SmReceiverState> last;
    SmCaptureSink sink{
        [&](const std::complex<float>* data, size_t count) {
          call([&] {
            if (!off_plan) consume(data, count);
          });
        },
        [&] { call(gap); },
        [&](const SmReceiverState& state) {
          call([&] {
            const auto mismatch = StateMismatch(request, state);
            if (!last || last->center_hz != state.center_hz ||
                last->rate != state.rate || last->gain_db != state.gain_db ||
                mismatch.empty() == off_plan)
              std::cerr << "[receiver] " << DescribeState(state)
                        << (mismatch.empty() ? ""
                                             : "; holding IQ until " + mismatch)
                        << std::endl;
            last = state;
            if (!mismatch.empty() && !off_plan) gap();
            off_plan = !mismatch.empty();
          });
        }};
    InvokePlugin([&] { plugin_->Capture(request, sink); }, "receiver failed",
                 callback_error);
  }

 private:
  boost::dll::shared_library module_;
  const SmReceiverPlugin* plugin_;
};
}  // namespace

const std::vector<SmReceiverDriver>& ReceiverDrivers() {
  static const auto drivers = DiscoverReceivers();
  return drivers;
}

const SmReceiverDriver& DriverFor(std::string_view device) {
  if (const auto* driver = FindExplicitDriver(device)) return *driver;
  const auto& drivers = ReceiverDrivers();
  const auto prefix = device.substr(0, device.find(':'));
  if (prefix.size() < device.size() &&
      prefix.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_-") ==
          std::string_view::npos)
    throw std::runtime_error("receiver device prefix has no installed driver");
  if (drivers.empty() || !drivers.front().prefix.empty())
    throw std::runtime_error("no default receiver driver is installed");
  return drivers.front();
}

bool IsFileReceiver(std::string_view device) {
  const auto* driver = FindExplicitDriver(device);
  return driver && driver->recording;
}

std::string ResolveReceiverDevice(std::string_view device,
                                  const std::filesystem::path& base) {
  const auto* driver = FindExplicitDriver(device);
  if (!driver || !driver->recording) return std::string(device);
  return driver->prefix +
         ResolveFilePath(device.substr(driver->prefix.size()), base).string();
}

std::vector<SmFoundReceiver> ProbeReceiver(std::string_view id,
                                           std::string selector,
                                           const std::filesystem::path& base) {
  const auto& drivers = ReceiverDrivers();
  auto driver = std::find_if(drivers.begin(), drivers.end(),
                             [&](const auto& d) { return id == d.id; });
  if (driver == drivers.end())
    throw std::runtime_error("unknown receiver driver");
  if (selector.rfind(driver->prefix, 0) == 0)
    selector.erase(0, driver->prefix.size());
  if (driver->recording)
    selector = selector.empty() ? base.string()
                                : ResolveFilePath(selector, base).string();
  SmReceiverSession session(*driver);
  std::vector<SmFoundReceiver> devices;
  session.Probe(selector, [&](const std::string& device,
                              const std::string& label, uint32_t rate) {
    constexpr size_t kMaxDevices = 64;
    if (devices.size() < kMaxDevices)
      devices.push_back({driver->prefix + device, label, rate});
    return devices.size() < kMaxDevices;
  });
  return devices;
}

namespace {
std::filesystem::path ResolveFilePath(std::string_view path,
                                      const std::filesystem::path& base) {
  namespace fs = std::filesystem;
  if (path.empty())
    throw std::runtime_error("recording receiver requires a file path");
  if (path.rfind("~/", 0) == 0
#ifdef _WIN32
      || path.rfind("~\\", 0) == 0
#endif
  ) {
#ifdef _WIN32
    const char* home = std::getenv("USERPROFILE");
#else
    const char* home = std::getenv("HOME");
#endif
    if (!home || !fs::path(home).is_absolute())
      throw std::runtime_error(
          "cannot resolve the recording path's home directory");
    return (fs::path(home) / fs::path(path.substr(2)).relative_path())
        .lexically_normal();
  }
  return (base / fs::path(path)).lexically_normal();
}
}  // namespace
void ReadIq(const char* device, int64_t dial_hz, int rate,
            const Samples& consume, const std::function<void()>& gap) {
  ReadReceiver(device, dial_hz, rate, NAN, NAN, consume, gap);
}
void ReadReceiver(const char* device, int64_t dial_hz, int rate, double gain,
                  double ppm, const Samples& consume,
                  const std::function<void()>& gap) {
  ReadReceiver({device, dial_hz, rate, gain, ppm}, consume, gap);
}

void ReadReceiver(SmReceiverCaptureRequest request, const Samples& consume,
                  const std::function<void()>& gap) {
  if (request.device == "-") {
    std::array<std::complex<float>, 4096> samples;
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
#endif
    while (std::cin.read(reinterpret_cast<char*>(samples.data()),
                         sizeof(samples)) ||
           std::cin.gcount()) {
      auto bytes = std::cin.gcount();
      if (bytes % sizeof(samples[0]))
        throw std::runtime_error("truncated IQ pair");
      consume(samples.data(), bytes / sizeof(samples[0]));
    }
    if (!std::cin.eof()) throw std::runtime_error("IQ input failure");
    return;
  }
  if (request.gain_db && !std::isfinite(*request.gain_db))
    request.gain_db.reset();
  if (request.correction_ppm && !std::isfinite(*request.correction_ppm))
    request.correction_ppm.reset();
  const auto& driver = DriverFor(request.device);
  request.device.erase(0, driver.prefix.size());
  if (driver.recording)
    request.device = ResolveFilePath(request.device).string();
  SmReceiverSession session(driver);
  session.Capture(request, consume, gap);
}
}  // namespace skim
