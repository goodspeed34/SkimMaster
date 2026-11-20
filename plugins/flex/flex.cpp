// FlexRadio 6000/8000 receiver: SmartSDR TCP control on port 4992 and DAX IQ
// as VITA-49 packets over UDP. Each DAX IQ channel (1-4) is its own device
// string, flex:host[:port]/channel.

#include <algorithm>
#include <array>
#include <boost/asio.hpp>
#include <chrono>
#include <cmath>
#include <complex>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <exception>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "receiver/plugin_api.h"

namespace skim {
namespace {
using Tcp = boost::asio::ip::tcp;
using Udp = boost::asio::ip::udp;
using Clock = std::chrono::steady_clock;

constexpr char kDefaultPort[] = "4992";
constexpr unsigned short kDiscoveryPort = 4992;
constexpr int kDaxChannels = 4;
constexpr int kDaxRates[] = {24000, 48000, 96000, 192000};

struct SmFlexAddress {
  std::string host;
  std::string port = kDefaultPort;
  int channel = 1;
};

// host[:port][/channel]
SmFlexAddress ParseAddress(std::string device) {
  SmFlexAddress address;
  if (const auto slash = device.rfind('/'); slash != std::string::npos) {
    const auto channel = device.substr(slash + 1);
    if (channel.size() != 1 || channel[0] < '1' ||
        channel[0] > '0' + kDaxChannels)
      throw std::runtime_error("expected FlexRadio DAX IQ channel 1-4");
    address.channel = channel[0] - '0';
    device.erase(slash);
  }
  if (const auto colon = device.rfind(':'); colon != std::string::npos) {
    address.host = device.substr(0, colon);
    address.port = device.substr(colon + 1);
  } else {
    address.host = device;
  }
  if (address.host.empty() || address.port.empty())
    throw std::runtime_error("expected FlexRadio host[:port]/channel");
  return address;
}

// key=value pairs of a SmartSDR status or reply, separated by spaces or
// commas outside double quotes.
std::map<std::string, std::string> Fields(const std::string& text) {
  std::map<std::string, std::string> fields;
  std::string word;
  bool quoted = false;
  auto flush = [&] {
    if (const auto equals = word.find('='); equals != std::string::npos)
      fields[word.substr(0, equals)] = word.substr(equals + 1);
    word.clear();
  };
  for (char c : text) {
    if (c == '"') quoted = !quoted;
    if (!quoted && (c == ' ' || c == ',' || c == '\t'))
      flush();
    else
      word += c;
  }
  flush();
  return fields;
}

std::string Unquote(std::string value) {
  if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
    value = value.substr(1, value.size() - 2);
  return value;
}

uint32_t BigWord(const uint8_t* p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 |
         uint32_t(p[3]);
}

std::optional<uint32_t> StreamId(const std::string& text) {
  try {
    size_t used = 0;
    const auto value = std::stoul(text, &used, 16);
    if (used == text.size() && value <= 0xffffffffUL)
      return static_cast<uint32_t>(value);
  } catch (const std::exception&) {
  }
  return std::nullopt;
}

struct SmVitaPacket {
  uint32_t stream = 0;
  int count = 0;
  const uint8_t* payload = nullptr;
  size_t payload_bytes = 0;
};

// Parses a VITA-49 IF or extension data packet with a stream ID. Returns
// false for other packet types.
bool ParseVita(const uint8_t* data, size_t size, SmVitaPacket& packet) {
  if (size < 8) return false;
  const auto header = BigWord(data);
  const auto type = header >> 28;
  if (type != 1 && type != 3) return false;
  const size_t words = header & 0xffff;
  if (words * 4 > size || words < 2) return false;
  size_t offset = 1;
  packet.stream = BigWord(data + 4 * offset++);
  if (header & (1u << 27)) offset += 2;  // class ID
  if ((header >> 22) & 3) offset += 1;   // integer timestamp
  if ((header >> 20) & 3) offset += 2;   // fractional timestamp
  const size_t trailer = header & (1u << 26) ? 1 : 0;
  if (offset + trailer > words) return false;
  packet.count = (header >> 16) & 0xf;
  packet.payload = data + 4 * offset;
  packet.payload_bytes = 4 * (words - offset - trailer);
  return true;
}

class SmFlexConnection {
 public:
  explicit SmFlexConnection(const SmFlexAddress& address) : socket_(io_) {
    Tcp::resolver resolver(io_);
    const auto endpoints = resolver.resolve(address.host, address.port);
    boost::system::error_code error = boost::asio::error::timed_out;
    boost::asio::async_connect(socket_, endpoints,
                               [&](auto result, auto) { error = result; });
    Run(std::chrono::seconds(5));
    if (error) throw boost::system::system_error(error, "FlexRadio connect");
    // The radio greets with V<version> and H<client handle>.
    while (handle_.empty()) {
      const auto line = ReadLine(Clock::now() + std::chrono::seconds(5));
      if (!line) throw std::runtime_error("FlexRadio sent no client handle");
      if (!line->empty() && (*line)[0] == 'H') handle_ = line->substr(1);
    }
  }

  ~SmFlexConnection() {
    boost::system::error_code ignored;
    socket_.close(ignored);
  }

  boost::asio::io_context& io() { return io_; }
  Tcp::socket& socket() { return socket_; }

  // Sends a command and waits for its reply, keeping status lines for the
  // caller. Throws when the radio reports an error.
  std::string Command(const std::string& command) {
    const auto sequence = std::to_string(++sequence_);
    const auto text = "C" + sequence + "|" + command + "\n";
    boost::asio::write(socket_, boost::asio::buffer(text));
    const auto deadline = Clock::now() + std::chrono::seconds(5);
    for (;;) {
      const auto line = ReadLine(deadline);
      if (!line)
        throw std::runtime_error("FlexRadio did not answer: " + command);
      if (line->rfind("R" + sequence + "|", 0) != 0) {
        if (!line->empty() && (*line)[0] == 'S') status_.push_back(*line);
        continue;
      }
      const auto rest = line->substr(sequence.size() + 2);
      const auto bar = rest.find('|');
      const auto code = rest.substr(0, bar);
      const auto message = bar == std::string::npos ? "" : rest.substr(bar + 1);
      if (code.find_first_not_of('0') != std::string::npos)
        throw std::runtime_error("FlexRadio rejected \"" + command +
                                 "\": " + code + " " + message);
      return message;
    }
  }

  // Best effort, for cleanup: sends without waiting for the reply.
  void Send(const std::string& command) {
    boost::system::error_code ignored;
    const auto text = "C" + std::to_string(++sequence_) + "|" + command + "\n";
    boost::asio::write(socket_, boost::asio::buffer(text), ignored);
  }

  std::vector<std::string> TakeStatus() {
    std::vector<std::string> status;
    status.swap(status_);
    return status;
  }
  boost::asio::streambuf& buffer() { return buffer_; }

 private:
  void Run(Clock::duration timeout) {
    io_.restart();
    io_.run_for(timeout);
    if (!io_.stopped()) {
      boost::system::error_code ignored;
      socket_.cancel(ignored);
      io_.restart();
      io_.run();
    }
  }

  std::optional<std::string> ReadLine(Clock::time_point deadline) {
    std::optional<boost::system::error_code> result;
    boost::asio::async_read_until(socket_, buffer_, '\n',
                                  [&](auto error, size_t) { result = error; });
    Run(std::max(Clock::duration::zero(), deadline - Clock::now()));
    if (!result || *result) return std::nullopt;
    std::istream input(&buffer_);
    std::string line;
    std::getline(input, line);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    return line;
  }

  boost::asio::io_context io_;
  Tcp::socket socket_;
  boost::asio::streambuf buffer_;
  std::string handle_;
  unsigned sequence_ = 0;
  std::vector<std::string> status_;
};

// Listens for SmartSDR discovery broadcasts.
std::map<std::string, std::string> Discover() {
  boost::asio::io_context io;
  Udp::socket socket(io);
  socket.open(Udp::v4());
  socket.set_option(boost::asio::socket_base::reuse_address(true));
  boost::system::error_code error;
  socket.bind({boost::asio::ip::address_v4::any(), kDiscoveryPort}, error);
  if (error) return {};
  std::map<std::string, std::string> radios;
  std::array<uint8_t, 2048> packet;
  Udp::endpoint sender;
  std::function<void()> receive = [&] {
    socket.async_receive_from(
        boost::asio::buffer(packet), sender, [&](auto failed, size_t size) {
          if (failed) return;
          SmVitaPacket vita;
          if (ParseVita(packet.data(), size, vita)) {
            std::string text(reinterpret_cast<const char*>(vita.payload),
                             vita.payload_bytes);
            text.resize(std::strlen(text.c_str()));
            auto fields = Fields(text);
            const auto ip = fields.count("ip") ? fields["ip"]
                                               : sender.address().to_string();
            const auto port = fields.count("port") ? fields["port"]
                                                   : std::string(kDefaultPort);
            auto model = Unquote(fields["model"]);
            if (model.empty()) model = "FlexRadio";
            if (const auto nick = Unquote(fields["nickname"]); !nick.empty())
              model += " " + nick;
            radios[ip + ":" + port] = model;
          }
          receive();
        });
  };
  receive();
  io.run_for(std::chrono::milliseconds(1500));
  return radios;
}

// IQ blocks handed from the UDP reader thread to the capture thread. When
// the consumer falls more than `capacity` samples behind, new blocks are
// dropped and the next delivered block carries a gap.
class SmIqBuffer {
 public:
  struct SmBlock {
    std::vector<std::complex<float>> iq;
    bool gap = false;
  };

  explicit SmIqBuffer(size_t capacity) : capacity_(capacity) {}

  void Push(std::vector<std::complex<float>> iq, bool gap) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (samples_ + iq.size() > capacity_) {
        overflowed_ = true;
        return;
      }
      samples_ += iq.size();
      blocks_.push_back({std::move(iq), gap || overflowed_});
      overflowed_ = false;
    }
    ready_.notify_one();
  }

  // Ends the stream because the reader failed; Pop rethrows the error.
  void Fail(std::exception_ptr error) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      error_ = std::move(error);
    }
    ready_.notify_all();
  }

  bool Pop(SmBlock& block, std::chrono::milliseconds wait) {
    std::unique_lock<std::mutex> lock(mutex_);
    ready_.wait_for(lock, wait, [&] { return !blocks_.empty() || error_; });
    if (error_) std::rethrow_exception(error_);
    if (blocks_.empty()) return false;
    block = std::move(blocks_.front());
    blocks_.pop_front();
    samples_ -= block.iq.size();
    return true;
  }

 private:
  const size_t capacity_;
  std::mutex mutex_;
  std::condition_variable ready_;
  std::deque<SmBlock> blocks_;
  size_t samples_ = 0;
  bool overflowed_ = false;
  std::exception_ptr error_;
};

class SmFlexReceiverPlugin final : public SmReceiverPlugin {
 public:
  const SmReceiverPluginMetadata& metadata() const noexcept override {
    static constexpr SmReceiverPluginMetadata metadata{"flex"};
    return metadata;
  }

  void Probe(const std::string& selector,
             const SmReceiverCandidate& found) const override {
    std::map<std::string, std::string> radios;
    if (selector.empty()) {
      radios = Discover();
    } else {
      const auto address = ParseAddress(selector);
      SmFlexConnection connection(address);
      auto model = Unquote(Fields(connection.Command("info"))["model"]);
      radios[address.host + ":" + address.port] =
          model.empty() ? "FlexRadio" : model;
    }
    for (const auto& [target, model] : radios)
      for (int channel = 1; channel <= kDaxChannels; ++channel)
        if (!found(target + "/" + std::to_string(channel),
                   model + " DAX IQ " + std::to_string(channel), 0))
          return;
  }

  void Capture(const SmReceiverCaptureRequest& request,
               const SmCaptureSink& sink) const override {
    if (request.correction_ppm)
      throw std::runtime_error("set FlexRadio calibration in SmartSDR");
    if (std::find(std::begin(kDaxRates), std::end(kDaxRates), request.rate) ==
        std::end(kDaxRates))
      throw std::runtime_error(
          "FlexRadio DAX IQ rates are 24000, 48000, 96000 or 192000 Hz");
    const auto address = ParseAddress(request.device);
    SmFlexConnection connection(address);
    auto& io = connection.io();

    // DAX IQ is read on its own thread so a slow decoder cannot overflow
    // the kernel's UDP buffer, which holds well under a second of IQ.
    boost::asio::io_context udp_io;
    Udp::socket udp(udp_io, Udp::endpoint(Udp::v4(), 0));
    udp.set_option(boost::asio::socket_base::receive_buffer_size(8 << 20));
    connection.Command("client udpport " +
                       std::to_string(udp.local_endpoint().port()));

    auto reply = connection.Command("display panafall create x=100 y=100");
    auto fields = Fields(reply);
    // Firmware answers with the panadapter ID alone, "pan,waterfall" or
    // pan=ID.
    const auto pan =
        fields.count("pan") ? fields["pan"] : reply.substr(0, reply.find(','));
    struct SmCleanup {
      SmFlexConnection& connection;
      std::vector<std::string> commands;
      ~SmCleanup() {
        for (auto it = commands.rbegin(); it != commands.rend(); ++it)
          connection.Send(*it);
      }
    } cleanup{connection, {"display pan remove " + pan}};

    char center[32];
    std::snprintf(center, sizeof(center), "%.6f", request.center_hz / 1e6);
    std::string tune = "display pan set " + pan + " center=" + center +
                       " daxiq_channel=" + std::to_string(address.channel);
    if (request.gain_db)
      tune += " rfgain=" + std::to_string(std::lround(*request.gain_db));
    connection.Command(tune);
    const auto stream_text =
        connection.Command("stream create type=dax_iq daxiq_channel=" +
                           std::to_string(address.channel));
    const auto stream = StreamId(stream_text);
    if (!stream)
      throw std::runtime_error("FlexRadio returned no DAX IQ stream: " +
                               stream_text);
    cleanup.commands.push_back("stream remove " + stream_text);
    connection.Command("stream set " + stream_text +
                       " daxiq_rate=" + std::to_string(request.rate));

    SmReceiverState state{request.center_hz, request.rate, request.gain_db};
    sink.state(state);
    auto apply_status = [&](const std::string& line) {
      const auto bar = line.find('|');
      if (bar == std::string::npos) return;
      const auto body = line.substr(bar + 1);
      std::istringstream words(body);
      std::string object, id;
      words >> object >> id;
      if (object == "display" && id == "pan")
        words >> id;
      else if (object == "display")
        return;
      auto fields = Fields(body);
      bool changed = false;
      if (object == "display" && id == pan) {
        if (body.find(" removed") != std::string::npos)
          throw std::runtime_error("FlexRadio closed the panadapter");
        if (fields.count("center")) {
          state.center_hz = std::llround(std::stod(fields["center"]) * 1e6);
          changed = true;
        }
        if (fields.count("rfgain")) {
          state.gain_db = std::stod(fields["rfgain"]);
          changed = true;
        }
      } else if (object == "stream" && StreamId(id) == stream) {
        if (body.find(" removed") != std::string::npos)
          throw std::runtime_error("FlexRadio removed the DAX IQ stream");
        if (fields.count("daxiq_rate")) {
          state.rate = std::stoi(fields["daxiq_rate"]);
          changed = true;
        }
      }
      if (changed) sink.state(state);
    };
    // Status queued during setup describes states our commands superseded.
    connection.TakeStatus();

    bool finished = false;
    std::function<void()> read_status = [&] {
      boost::asio::async_read_until(
          connection.socket(), connection.buffer(), '\n',
          [&](boost::system::error_code error, size_t) {
            if (error) {
              finished = true;
              io.stop();
              return;
            }
            std::istream input(&connection.buffer());
            std::string line;
            std::getline(input, line);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty() && line[0] == 'S') apply_status(line);
            read_status();
          });
    };
    SmIqBuffer buffer(2 * static_cast<size_t>(request.rate));
    std::array<uint8_t, 65536> packet;
    int expected_count = -1;
    // Payloads are big-endian float32 I/Q pairs. The 4-bit packet count
    // reveals datagrams lost on the network.
    auto receive = [&](size_t size) {
      SmVitaPacket vita;
      if (!ParseVita(packet.data(), size, vita) || vita.stream != *stream)
        return;
      const bool lost = expected_count >= 0 && vita.count != expected_count;
      expected_count = (vita.count + 1) & 0xf;
      std::vector<std::complex<float>> iq(vita.payload_bytes / 8);
      for (size_t i = 0; i < iq.size(); ++i) {
        float values[2];
        for (int j = 0; j < 2; ++j) {
          const auto bits = BigWord(vita.payload + 8 * i + 4 * j);
          std::memcpy(&values[j], &bits, sizeof(float));
        }
        iq[i] = {values[0], values[1]};
      }
      buffer.Push(std::move(iq), lost);
    };
    std::function<void()> read_iq = [&] {
      udp.async_receive(boost::asio::buffer(packet),
                        [&](boost::system::error_code error, size_t size) {
                          if (error == boost::asio::error::operation_aborted)
                            return;
                          if (!error) receive(size);
                          read_iq();
                        });
    };
    read_iq();
    std::thread reader([&] {
      try {
        udp_io.run();
      } catch (...) {
        buffer.Fail(std::current_exception());
      }
    });
    struct SmJoin {
      boost::asio::io_context& io;
      std::thread& thread;
      ~SmJoin() {
        io.stop();
        if (thread.joinable()) thread.join();
      }
    } join{udp_io, reader};

    auto deliver = [&](SmIqBuffer::SmBlock& block) {
      if (block.gap) sink.gap();
      if (!block.iq.empty()) sink.samples(block.iq.data(), block.iq.size());
    };
    read_status();
    SmIqBuffer::SmBlock block;
    while (!finished) {
      io.restart();
      io.poll();
      if (finished) break;
      if (buffer.Pop(block, std::chrono::milliseconds(20))) deliver(block);
    }
    // Deliver IQ that arrived before the control connection closed.
    udp_io.stop();
    reader.join();
    udp.cancel();
    while (buffer.Pop(block, std::chrono::milliseconds(0))) deliver(block);
    boost::system::error_code drained;
    while (udp.available(drained) && !drained) {
      const auto size = udp.receive(boost::asio::buffer(packet), 0, drained);
      if (!drained) receive(size);
    }
    while (buffer.Pop(block, std::chrono::milliseconds(0))) deliver(block);
  }
};
}  // namespace
}  // namespace skim

extern "C" SKIM_PLUGIN_EXPORT const skim::SmReceiverPlugin*
skim_receiver_v3() noexcept {
  static const skim::SmFlexReceiverPlugin plugin;
  return &plugin;
}

extern "C" SKIM_PLUGIN_EXPORT const skim::SmReceiverPluginDescriptor*
skim_receiver_descriptor_v3() noexcept {
  static constexpr skim::SmReceiverPluginDescriptor descriptor{
      "flex:",
      "FlexRadio (DAX IQ)",
      "host[:port], blank discovers on the LAN",
      false,
      true,
      true,
      true};
  return &descriptor;
}
