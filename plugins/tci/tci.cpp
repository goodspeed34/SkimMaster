// TCI (Transceiver Control Interface) receiver: ExpertSDR, SunSDR and other
// TCI servers. Control is text over a WebSocket; IQ arrives as binary stream
// frames with a 64-byte header.

#include <algorithm>
#include <array>
#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <cctype>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstring>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "receiver/plugin_api.h"

namespace skim {
namespace {
namespace beast = boost::beast;
namespace websocket = beast::websocket;
using Tcp = boost::asio::ip::tcp;
using Clock = std::chrono::steady_clock;

constexpr char kDefaultPort[] = "40001";
constexpr size_t kHeaderBytes = 64;
constexpr uint32_t kIqStream = 0;
constexpr int kIqRates[] = {48000, 96000, 192000, 384000};

struct SmTciAddress {
  std::string host;
  std::string port = kDefaultPort;
  int receiver = 0;
};

// host[:port][/receiver]
SmTciAddress ParseAddress(std::string device) {
  SmTciAddress address;
  if (const auto slash = device.rfind('/'); slash != std::string::npos) {
    const auto index = device.substr(slash + 1);
    if (index.empty() || index.size() > 2 ||
        index.find_first_not_of("0123456789") != std::string::npos)
      throw std::runtime_error("expected TCI host:port/receiver");
    address.receiver = std::stoi(index);
    device.erase(slash);
  }
  if (device.size() > 2 && device.front() == '[') {
    const auto close = device.find(']');
    if (close == std::string::npos)
      throw std::runtime_error("unterminated IPv6 TCI address");
    address.host = device.substr(1, close - 1);
    if (close + 1 < device.size()) {
      if (device[close + 1] != ':')
        throw std::runtime_error("expected TCI [host]:port");
      address.port = device.substr(close + 2);
    }
  } else if (const auto colon = device.rfind(':'); colon != std::string::npos) {
    address.host = device.substr(0, colon);
    address.port = device.substr(colon + 1);
  } else {
    address.host = device;
  }
  if (address.host.empty() || address.port.empty())
    throw std::runtime_error("expected TCI host:port");
  return address;
}

struct SmTciCommand {
  std::string name;
  std::vector<std::string> arguments;
};

// Splits "name:arg,arg;name;" into commands. Names are case-insensitive.
std::vector<SmTciCommand> ParseCommands(const std::string& text) {
  std::vector<SmTciCommand> commands;
  size_t start = 0;
  while (start < text.size()) {
    auto end = text.find(';', start);
    if (end == std::string::npos) end = text.size();
    auto item = text.substr(start, end - start);
    start = end + 1;
    const auto first = item.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) continue;
    item.erase(0, first);
    SmTciCommand command;
    const auto colon = item.find(':');
    command.name = item.substr(0, colon);
    for (auto& c : command.name)
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (colon != std::string::npos) {
      size_t from = colon + 1;
      for (;;) {
        const auto comma = item.find(',', from);
        command.arguments.push_back(item.substr(from, comma - from));
        if (comma == std::string::npos) break;
        from = comma + 1;
      }
    }
    commands.push_back(std::move(command));
  }
  return commands;
}

std::optional<int64_t> Integer(const std::string& value) {
  try {
    size_t used = 0;
    const auto number = std::stoll(value, &used);
    if (used == value.size()) return number;
  } catch (const std::exception&) {
  }
  return std::nullopt;
}

uint32_t Word(const uint8_t* bytes, size_t index) {
  const auto* p = bytes + 4 * index;
  return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 |
         uint32_t(p[3]) << 24;
}

// The server's announced state, updated by notifications.
struct SmTciState {
  std::string device = "TCI";
  int receivers = 1;
  std::optional<int> rate;
  std::map<int, int64_t> dds;
  bool ready = false;

  // Returns true when the command changed the rate or a DDS.
  bool Apply(const SmTciCommand& command) {
    const auto& args = command.arguments;
    if (command.name == "device" && !args.empty()) {
      device = args[0];
    } else if (command.name == "trx_count" && !args.empty()) {
      if (auto count = Integer(args[0]); count && *count > 0 && *count < 16)
        receivers = static_cast<int>(*count);
    } else if (command.name == "iq_samplerate" && !args.empty()) {
      if (auto value = Integer(args[0]); value && *value > 0) {
        rate = static_cast<int>(*value);
        return true;
      }
    } else if (command.name == "dds" && args.size() >= 2) {
      auto rx = Integer(args[0]);
      auto hz = Integer(args[1]);
      if (rx && hz) {
        dds[static_cast<int>(*rx)] = *hz;
        return true;
      }
    } else if (command.name == "ready") {
      ready = true;
    }
    return false;
  }
};

class SmTciConnection {
 public:
  explicit SmTciConnection(const SmTciAddress& address) : ws_(io_) {
    Tcp::resolver resolver(io_);
    auto endpoints = resolver.resolve(address.host, address.port);
    beast::get_lowest_layer(ws_).expires_after(std::chrono::seconds(5));
    beast::get_lowest_layer(ws_).connect(endpoints);
    beast::get_lowest_layer(ws_).expires_never();
    ws_.set_option(
        websocket::stream_base::timeout::suggested(beast::role_type::client));
    ws_.read_message_max(16 * 1024 * 1024);
    ws_.handshake(address.host + ":" + address.port, "/");
  }

  ~SmTciConnection() {
    beast::error_code ignored;
    ws_.close(websocket::close_code::normal, ignored);
  }

  void Send(const std::string& commands) {
    ws_.text(true);
    ws_.write(boost::asio::buffer(commands));
  }

  // Reads one message into buffer_. Returns false when the server closed.
  bool Read(bool& binary) {
    buffer_.clear();
    beast::error_code error;
    ws_.read(buffer_, error);
    if (error == websocket::error::closed || error == boost::asio::error::eof)
      return false;
    if (error) throw beast::system_error(error);
    binary = ws_.got_binary();
    return true;
  }

  // Reads one message, waiting at most until deadline. Returns false on
  // timeout or close.
  bool ReadUntil(Clock::time_point deadline, bool& binary) {
    buffer_.clear();
    std::optional<beast::error_code> result;
    ws_.async_read(buffer_,
                   [&](beast::error_code error, size_t) { result = error; });
    io_.restart();
    io_.run_until(deadline);
    if (!result) {
      beast::get_lowest_layer(ws_).cancel();
      io_.restart();
      io_.run();
      return false;
    }
    if (*result == websocket::error::closed ||
        *result == boost::asio::error::eof)
      return false;
    if (*result) throw beast::system_error(*result);
    binary = ws_.got_binary();
    return true;
  }

  std::string Text() const { return beast::buffers_to_string(buffer_.data()); }
  const uint8_t* Data() const {
    return static_cast<const uint8_t*>(buffer_.data().data());
  }
  size_t Size() const { return buffer_.size(); }

  // Reads the state burst a server sends on connect, up to "ready;".
  SmTciState ReadInitialState() {
    SmTciState state;
    const auto deadline = Clock::now() + std::chrono::seconds(5);
    bool binary = false;
    while (!state.ready && ReadUntil(deadline, binary))
      if (!binary)
        for (const auto& command : ParseCommands(Text())) state.Apply(command);
    if (!state.ready) throw std::runtime_error("TCI server did not get ready");
    return state;
  }

 private:
  boost::asio::io_context io_;
  websocket::stream<beast::tcp_stream> ws_;
  beast::flat_buffer buffer_;
};

// Converts one IQ stream frame for receiver rx. Returns false for frames of
// other receivers or stream types.
bool DecodeIq(const uint8_t* data, size_t size, int rx,
              std::vector<std::complex<float>>& iq, uint32_t& rate) {
  if (size < kHeaderBytes) throw std::runtime_error("short TCI stream frame");
  if (Word(data, 6) != kIqStream || Word(data, 0) != uint32_t(rx)) return false;
  if (Word(data, 3) != 0)
    throw std::runtime_error("compressed TCI IQ streams are not supported");
  rate = Word(data, 1);
  const auto format = Word(data, 2);
  static constexpr size_t widths[] = {2, 3, 4, 4};
  if (format > 3) throw std::runtime_error("unknown TCI sample format");
  const size_t width = widths[format];
  const size_t payload = size - kHeaderBytes;
  if (payload % (2 * width)) throw std::runtime_error("truncated TCI IQ frame");
  const auto* p = data + kHeaderBytes;
  auto scalar = [&](size_t i) -> float {
    const auto* s = p + i * width;
    switch (format) {
      case 0:
        return static_cast<int16_t>(uint16_t(s[0] | s[1] << 8)) / 32768.0f;
      case 1: {
        int32_t v = int32_t(s[0]) | int32_t(s[1]) << 8 | int32_t(s[2]) << 16;
        if (v & 0x800000) v -= 0x1000000;
        return v / 8388608.0f;
      }
      case 2:
        return static_cast<int32_t>(Word(s, 0)) / 2147483648.0f;
      default: {
        const uint32_t bits = Word(s, 0);
        float value;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
      }
    }
  };
  iq.resize(payload / (2 * width));
  for (size_t i = 0; i < iq.size(); ++i)
    iq[i] = {scalar(2 * i), scalar(2 * i + 1)};
  return true;
}

class SmTciReceiverPlugin final : public SmReceiverPlugin {
 public:
  const SmReceiverPluginMetadata& metadata() const noexcept override {
    static constexpr SmReceiverPluginMetadata metadata{"tci"};
    return metadata;
  }

  void Probe(const std::string& selector,
             const SmReceiverCandidate& found) const override {
    auto address = ParseAddress(selector.empty() ? "127.0.0.1" : selector);
    std::optional<SmTciState> state;
    try {
      SmTciConnection connection(address);
      state = connection.ReadInitialState();
    } catch (const std::exception&) {
      // An empty selector looks for a local server; absence is not an error.
      if (selector.empty()) return;
      throw;
    }
    const auto base =
        (address.host.find(':') != std::string::npos ? "[" + address.host + "]"
                                                     : address.host) +
        ":" + address.port;
    for (int rx = 0; rx < state->receivers; ++rx)
      if (!found(base + "/" + std::to_string(rx),
                 state->device + " receiver " + std::to_string(rx) + " (TCI)",
                 state->rate ? static_cast<uint32_t>(*state->rate) : 0))
        break;
  }

  void Capture(const SmReceiverCaptureRequest& request,
               const SmCaptureSink& sink) const override {
    if (request.gain_db || request.correction_ppm)
      throw std::runtime_error(
          "set TCI gain and calibration in the radio software");
    if (std::find(std::begin(kIqRates), std::end(kIqRates), request.rate) ==
        std::end(kIqRates))
      throw std::runtime_error(
          "TCI IQ rates are 48000, 96000, 192000 or 384000 Hz");
    const auto address = ParseAddress(request.device);
    SmTciConnection connection(address);
    auto state = connection.ReadInitialState();
    if (address.receiver >= state.receivers)
      throw std::runtime_error("TCI server has no receiver " +
                               std::to_string(address.receiver));
    const auto rx = std::to_string(address.receiver);
    connection.Send("iq_samplerate:" + std::to_string(request.rate) + ";");
    connection.Send("dds:" + rx + "," + std::to_string(request.center_hz) +
                    ";");
    connection.Send("iq_start:" + rx + ";");
    // Servers announce the change to every client, but may not echo it to
    // the sender; assume the request applies until told otherwise.
    state.rate = request.rate;
    state.dds[address.receiver] = request.center_hz;
    auto report = [&] {
      sink.state({state.dds[address.receiver], state.rate, std::nullopt});
    };
    report();
    std::vector<std::complex<float>> iq;
    bool binary = false;
    while (connection.Read(binary)) {
      if (!binary) {
        bool changed = false;
        for (const auto& command : ParseCommands(connection.Text())) {
          const bool ours = command.name != "dds" ||
                            (!command.arguments.empty() &&
                             Integer(command.arguments[0]) == address.receiver);
          if (state.Apply(command) && ours) changed = true;
        }
        if (changed) report();
        continue;
      }
      uint32_t rate = 0;
      if (!DecodeIq(connection.Data(), connection.Size(), address.receiver, iq,
                    rate))
        continue;
      if (rate && state.rate != static_cast<int>(rate)) {
        state.rate = static_cast<int>(rate);
        report();
      }
      if (!iq.empty()) sink.samples(iq.data(), iq.size());
    }
  }
};
}  // namespace
}  // namespace skim

extern "C" SKIM_PLUGIN_EXPORT const skim::SmReceiverPlugin*
skim_receiver_v3() noexcept {
  static const skim::SmTciReceiverPlugin plugin;
  return &plugin;
}

extern "C" SKIM_PLUGIN_EXPORT const skim::SmReceiverPluginDescriptor*
skim_receiver_descriptor_v3() noexcept {
  static constexpr skim::SmReceiverPluginDescriptor descriptor{
      "tci:",
      "TCI (ExpertSDR / SunSDR)",
      "host:port, default port 40001",
      false,
      false,
      true,
      true};
  return &descriptor;
}
