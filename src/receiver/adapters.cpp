#include "receiver/adapters.h"

#include <boost/asio.hpp>
#ifdef SKIM_ALSA
#include <alsa/asoundlib.h>
#endif
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <stdexcept>

namespace skim {
#ifndef SKIM_ALSA
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
}  // namespace
void ProbeDevices(const std::string& selector, const ReceiverCandidate& found) {
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
#endif
#ifdef SKIM_ALSA
void ProbeDevices(const std::string& selector, const ReceiverCandidate& found) {
  void** hints = nullptr;
  const int rc = snd_device_name_hint(-1, "pcm", &hints);
  if (rc < 0) throw std::runtime_error(snd_strerror(rc));
  std::unique_ptr<void*, decltype(&snd_device_name_free_hint)> cleanup(
      hints, snd_device_name_free_hint);
  for (auto hint = hints; hint && *hint; ++hint) {
    auto get = [&](const char* key) {
      std::unique_ptr<char, decltype(&std::free)> value(
          snd_device_name_get_hint(*hint, key), std::free);
      return value ? std::string(value.get()) : std::string();
    };
    if (get("IOID") == "Output") continue;
    auto name = get("NAME");
    auto label = get("DESC");
    if (name.empty() || name == "null" ||
        (name.find(selector) == std::string::npos &&
         label.find(selector) == std::string::npos))
      continue;
    for (char& c : label)
      if (c == '\n') c = ' ';
    if (!found(name, label.empty() ? name : label, 0)) break;
  }
}

void ReadAlsa(const std::string& name, int rate, const Samples& consume,
              const std::function<void()>& gap) {
  snd_pcm_t* handle = nullptr;
  auto check = [](int rc) {
    if (rc < 0) throw std::runtime_error(snd_strerror(rc));
  };
  check(snd_pcm_open(&handle, name.c_str(), SND_PCM_STREAM_CAPTURE, 0));
  std::unique_ptr<snd_pcm_t, decltype(&snd_pcm_close)> pcm(handle,
                                                           snd_pcm_close);
  check(snd_pcm_set_params(handle, SND_PCM_FORMAT_S16,
                           SND_PCM_ACCESS_RW_INTERLEAVED, 2, rate, 1, 50000));
  std::array<int16_t, 8192> raw;
  std::array<std::complex<float>, 4096> iq;
  for (;;) {
    auto n = snd_pcm_readi(handle, raw.data(), iq.size());
    if (n < 0) {
      check(snd_pcm_recover(handle, n, 1));
      gap();
      continue;
    }
    for (int i = 0; i < n; ++i)
      iq[i] = {raw[2 * i] / 32768.f, raw[2 * i + 1] / 32768.f};
    consume(iq.data(), n);
  }
}
#endif
}  // namespace skim
