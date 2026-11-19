#include "receiver/receiver.h"

#include <algorithm>
#include <array>
#include <boost/dll/runtime_symbol_info.hpp>
#include <boost/dll/shared_library.hpp>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <type_traits>

#include "receiver/api.h"

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif
namespace skim {
namespace {
boost::dll::shared_library LoadReceiverModule(const ReceiverDriver& driver) {
  auto path = boost::dll::program_location().parent_path() /
              (std::string("librcvr") + driver.id + SKIM_MODULE_SUFFIX);
  return boost::dll::shared_library(path);
}
}  // namespace

const std::vector<ReceiverDriver>& ReceiverDrivers() {
  static const std::vector<ReceiverDriver> drivers{
      {"soapy", "", "SoapySDR"},
      {"rtltcp", "rtltcp:", "RTL-TCP"},
#ifdef __linux__
      {"alsa", "alsa:", "ALSA"},
#endif
      {"wav", "wav:", "IQ WAV"},
  };
  return drivers;
}
const ReceiverDriver& DriverFor(std::string_view device) {
  const auto& drivers = ReceiverDrivers();
  for (const auto& driver : drivers)
    if (*driver.prefix && device.rfind(driver.prefix, 0) == 0) return driver;
  if (device.rfind("alsa:", 0) == 0)
    throw std::runtime_error("ALSA receivers are available only on Linux");
  return drivers.front();
}

std::vector<FoundReceiver> ProbeReceiver(std::string_view id,
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
  auto module = LoadReceiverModule(*driver);
  if (!module.has("skim_receiver_probe_v1"))
    throw std::runtime_error(
        "installed receiver module does not support probing");
  auto& probe = module.get<std::remove_pointer_t<skim_receiver_probe>>(
      "skim_receiver_probe_v1");
  struct Results {
    const char* prefix;
    std::vector<FoundReceiver> devices;
  } results{driver->prefix, {}};
  auto add = [](void* opaque, const char* device, const char* label,
                uint32_t rate) {
    auto& result = *static_cast<Results*>(opaque);
    result.devices.push_back(
        {std::string(result.prefix) + device, label, rate});
    return result.devices.size() >= 64 ? 1 : 0;
  };
  std::array<char, 512> error{};
  if (probe(selector.c_str(), add, &results, error.data(), error.size()))
    throw std::runtime_error(error.data());
  return std::move(results.devices);
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
  std::array<std::complex<float>, 4096> samples;
  if (std::string(device) == "-") {
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
  const auto& driver = DriverFor(device);
  std::string address = device + std::char_traits<char>::length(driver.prefix);
  if (IsWavReceiver(device)) address = ResolveWavPath(address).string();
  auto module = LoadReceiverModule(driver);
  auto api = module.get<const skim_radio_api*()>("skim_receiver_v1")();
  if (!api || api->version != 1 || api->size < sizeof(*api) || !api->capture)
    throw std::runtime_error("unsupported receiver ABI");
  struct Callbacks {
    const Samples& consume;
    const std::function<void()>& gap;
    std::exception_ptr error;
  };
  Callbacks callbacks{consume, gap, {}};
  auto callback = [](void* opaque, const float* data, uint32_t count) {
    auto& c = *static_cast<Callbacks*>(opaque);
    try {
      if (data)
        c.consume(reinterpret_cast<const std::complex<float>*>(data), count);
      else
        c.gap();
      return 0;
    } catch (...) {
      c.error = std::current_exception();
      return -1;
    }
  };
  skim_radio_config cfg{address.c_str(), dial_hz, static_cast<uint32_t>(rate),
                        gain, ppm};
  std::array<char, 512> error{};
  if (api->capture(&cfg, callback, &callbacks, error.data(), error.size())) {
    if (callbacks.error) std::rethrow_exception(callbacks.error);
    throw std::runtime_error(error.data());
  }
}
}  // namespace skim
