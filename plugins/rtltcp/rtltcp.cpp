#include <array>
#include <boost/asio.hpp>
#include <chrono>
#include <cmath>
#include <cstring>
#include <stdexcept>

#include "receiver/plugin_api.h"

namespace skim {
namespace {
using Tcp = boost::asio::ip::tcp;
using RtlHeader = std::array<uint8_t, 12>;

std::pair<std::string, std::string> RtlEndpoint(const std::string& address) {
  auto colon = address.rfind(':');
  if (colon == std::string::npos || colon == 0 || colon + 1 == address.size())
    throw std::runtime_error("expected rtl_tcp host:port");
  auto host = address.substr(0, colon);
  if (host.size() > 2 && host.front() == '[' && host.back() == ']')
    host = host.substr(1, host.size() - 2);
  return {host, address.substr(colon + 1)};
}

void CheckRtlHeader(const RtlHeader& header) {
  if (std::memcmp(header.data(), "RTL0", 4))
    throw std::runtime_error("invalid rtl_tcp handshake");
}

void ProbeDevices(const std::string& selector,
                  const SmReceiverCandidate& found) {
  const auto address = selector.empty() ? "127.0.0.1:1234" : selector;
  const auto [host, port] = RtlEndpoint(address);
  boost::asio::io_context io;
  Tcp::resolver resolver(io);
  Tcp::socket socket(io);
  boost::asio::steady_timer deadline(io, std::chrono::seconds(2));
  RtlHeader header{};
  boost::system::error_code result;
  bool timed_out = false;
  auto finish = [&](boost::system::error_code error) {
    result = error;
    deadline.cancel();
  };
  deadline.async_wait([&](boost::system::error_code error) {
    if (error) return;
    timed_out = true;
    resolver.cancel();
    socket.close(error);
  });
  resolver.async_resolve(host, port, [&](auto error, auto endpoints) {
    if (error || timed_out) return finish(error);
    boost::asio::async_connect(
        socket, endpoints, [&](auto connect_error, auto) {
          if (connect_error || timed_out) return finish(connect_error);
          boost::asio::async_read(
              socket, boost::asio::buffer(header),
              [&](auto read_error, auto) { finish(read_error); });
        });
  });
  io.run();
  if (timed_out) throw std::runtime_error("RTL-TCP probe timed out");
  if (result == boost::asio::error::connection_refused && selector.empty())
    return;
  if (result)
    throw std::runtime_error("RTL-TCP probe failed: " + result.message());
  CheckRtlHeader(header);
  found(address, "RTL-TCP at " + address, 0);
}

void ReadRtlTcp(const std::string& address, int64_t center, int rate,
                double gain, double ppm, const Samples& consume) {
  const auto [host, port] = RtlEndpoint(address);
  if (center > UINT32_MAX)
    throw std::runtime_error("RTL-TCP requires a 32-bit tuning frequency");
  boost::asio::io_context io;
  Tcp::resolver resolver(io);
  Tcp::socket socket(io);
  boost::asio::connect(socket, resolver.resolve(host, port));
  RtlHeader header{};
  boost::asio::read(socket, boost::asio::buffer(header));
  CheckRtlHeader(header);
  auto command = [&](uint8_t op, uint32_t value) {
    std::array<uint8_t, 5> bytes{op, static_cast<uint8_t>(value >> 24),
                                 static_cast<uint8_t>(value >> 16),
                                 static_cast<uint8_t>(value >> 8),
                                 static_cast<uint8_t>(value)};
    boost::asio::write(socket, boost::asio::buffer(bytes));
  };
  command(2, rate);
  command(1, center);
  if (std::isfinite(gain)) {
    command(3, 1);
    command(4, static_cast<int32_t>(std::lround(10 * gain)));
  }
  if (std::isfinite(ppm)) command(5, static_cast<int32_t>(std::lround(ppm)));
  std::array<uint8_t, 8192> raw;
  std::array<std::complex<float>, 4096> iq;
  for (;;) {
    boost::system::error_code error;
    auto n = boost::asio::read(socket, boost::asio::buffer(raw), error);
    if (n % 2) throw std::runtime_error("truncated rtl_tcp IQ pair");
    for (size_t i = 0; i < n / 2; ++i)
      iq[i] = {(raw[2 * i] - 127.5f) / 128, (raw[2 * i + 1] - 127.5f) / 128};
    if (n) consume(iq.data(), n / 2);
    if (error == boost::asio::error::eof) break;
    if (error) throw boost::system::system_error(error);
  }
}

class SmRtlTcpReceiverPlugin final : public SmReceiverPlugin {
 public:
  const SmReceiverPluginMetadata& metadata() const noexcept override {
    static constexpr SmReceiverPluginMetadata metadata{"rtltcp"};
    return metadata;
  }

  void Probe(const std::string& selector,
             const SmReceiverCandidate& found) const override {
    ProbeDevices(selector, found);
  }

  void Capture(const SmReceiverCaptureRequest& cfg, const Samples& samples,
               const std::function<void()>& gap) const override {
    (void)gap;
    ReadRtlTcp(cfg.device, cfg.center_hz, cfg.rate, cfg.gain_db.value_or(NAN),
               cfg.correction_ppm.value_or(NAN), samples);
  }
};
}  // namespace
}  // namespace skim

extern "C" SKIM_PLUGIN_EXPORT const skim::SmReceiverPlugin*
skim_receiver_v2() noexcept {
  static const skim::SmRtlTcpReceiverPlugin plugin;
  return &plugin;
}

extern "C" SKIM_PLUGIN_EXPORT const skim::SmReceiverPluginDescriptor*
skim_receiver_descriptor_v2() noexcept {
  static constexpr skim::SmReceiverPluginDescriptor descriptor{
      "rtltcp:", "RTL-TCP", "127.0.0.1:1234", false, true, true};
  return &descriptor;
}
