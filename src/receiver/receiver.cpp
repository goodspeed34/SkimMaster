#include "receiver/receiver.h"

#include <algorithm>
#include <array>
#include <boost/dll/runtime_symbol_info.hpp>
#include <boost/dll/shared_library.hpp>
#include <cmath>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif
namespace skim {
namespace {
boost::dll::shared_library LoadReceiverModule(const SmReceiverDriver& driver) {
  auto path = boost::dll::program_location().parent_path() /
              (std::string("librcvr") + driver.id + SKIM_MODULE_SUFFIX);
  return boost::dll::shared_library(path);
}
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

class SmReceiverSession {
 public:
  explicit SmReceiverSession(const SmReceiverDriver& driver)
      : module_(LoadReceiverModule(driver)) {
    if (!module_.has("skim_receiver_v2"))
      throw std::runtime_error(
          "receiver module does not export skim_receiver_v2");
    plugin_ =
        module_.get<const SmReceiverPlugin*() noexcept>("skim_receiver_v2")();
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
    InvokePlugin(
        [&] {
          plugin_->Capture(
              request,
              [&](const std::complex<float>* data, size_t count) {
                call([&] { consume(data, count); });
              },
              [&] { call(gap); });
        },
        "receiver failed", callback_error);
  }

 private:
  boost::dll::shared_library module_;
  const SmReceiverPlugin* plugin_;
};
}  // namespace

const std::vector<SmReceiverDriver>& ReceiverDrivers() {
  static const std::vector<SmReceiverDriver> drivers{
      {"soapy", "", "SoapySDR"},
      {"rtltcp", "rtltcp:", "RTL-TCP"},
#ifdef __linux__
      {"alsa", "alsa:", "ALSA"},
#endif
      {"wav", "wav:", "IQ WAV"},
  };
  return drivers;
}
const SmReceiverDriver& DriverFor(std::string_view device) {
  const auto& drivers = ReceiverDrivers();
  for (const auto& driver : drivers)
    if (*driver.prefix && device.rfind(driver.prefix, 0) == 0) return driver;
  if (device.rfind("alsa:", 0) == 0)
    throw std::runtime_error("ALSA receivers are available only on Linux");
  return drivers.front();
}

std::vector<SmFoundReceiver> ProbeReceiver(std::string_view id,
                                           std::string selector,
                                           const std::filesystem::path& base) {
  const auto& drivers = ReceiverDrivers();
  auto driver = std::find_if(drivers.begin(), drivers.end(),
                             [&](const auto& d) { return id == d.id; });
  if (driver == drivers.end())
    throw std::runtime_error("unknown receiver driver");
  if (*driver->prefix && selector.rfind(driver->prefix, 0) == 0)
    selector.erase(0, std::char_traits<char>::length(driver->prefix));
  if (id == "wav")
    selector = selector.empty() ? base.string()
                                : ResolveWavPath(selector, base).string();
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

std::filesystem::path ResolveWavPath(std::string_view path,
                                     const std::filesystem::path& base) {
  namespace fs = std::filesystem;
  if (path.empty())
    throw std::runtime_error("WAV receiver requires a file path");
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
      throw std::runtime_error("cannot resolve the WAV path's home directory");
    return (fs::path(home) / fs::path(path.substr(2)).relative_path())
        .lexically_normal();
  }
  return (base / fs::path(path)).lexically_normal();
}
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
  request.device.erase(0, std::char_traits<char>::length(driver.prefix));
  if (std::string_view(driver.id) == "wav")
    request.device = ResolveWavPath(request.device).string();
  SmReceiverSession session(driver);
  session.Capture(request, consume, gap);
}
}  // namespace skim
