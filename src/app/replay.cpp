#include "app/replay.h"

#include <nng/nng.h>
#include <nng/protocol/pair1/pair.h>

#include <array>
#include <charconv>
#include <cmath>
#include <complex>
#include <cstdint>
#include <future>
#include <iomanip>
#include <istream>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

extern "C" {
#include <libcsdr.h>
}

namespace skim {
namespace {
constexpr int kInputRate = 48000;
constexpr int kDecimation = 4;

constexpr int kTapCount = 65;
constexpr int kBlockSize = 257;
constexpr double kPi = 3.14159265358979323846;

// Estimate one tone per contiguous segment from the mean phase increment.
class SmToneDecoder {
 public:
  void Process(complexf sample) {
    std::complex<double> current(sample.i, sample.q);
    if (count_) correlation_ += std::conj(previous_) * current;
    previous_ = current;
    ++count_;
  }

  void Finish(uint64_t start, int64_t slice_center, std::ostream& output) {
    if (count_ >= 2 && std::abs(correlation_) > 0) {
      double offset =
          std::arg(correlation_) * (kInputRate / kDecimation) / (2 * kPi);
      output << start << ' ' << count_ << ' ' << std::fixed
             << std::setprecision(2) << slice_center + offset << '\n';
    }
    *this = {};
  }

 private:
  uint64_t count_ = 0;
  std::complex<double> previous_{}, correlation_{};
};

class SmSlicer {
 public:
  explicit SmSlicer(int offset) : offset_(offset) {
    firdes_lowpass_f(taps_.data(), kTapCount, 0.10f, WINDOW_HAMMING);
  }

  void Process(std::vector<complexf>& block, SmToneDecoder& decoder) {
    if (block.empty()) return;
    auto tail = history_.size();
    history_.resize(tail + block.size());
    phase_ = shift_math_cc(block.data(), history_.data() + tail, block.size(),
                           -static_cast<float>(offset_) / kInputRate, phase_);
    std::array<complexf, kBlockSize> sliced;
    int count = fir_decimate_cc(history_.data(), sliced.data(), history_.size(),
                                kDecimation, taps_.data(), kTapCount);
    for (int i = 0; i < count; ++i) decoder.Process(sliced[i]);
    history_.erase(history_.begin(), history_.begin() + count * kDecimation);
    block.clear();
  }

  void Reset() {
    history_.clear();
    phase_ = 0;
  }

 private:
  int offset_;
  float phase_ = 0;
  std::array<float, kTapCount> taps_;
  std::vector<complexf> history_;
};

struct SmBlock {
  uint64_t start = 0;
  uint32_t count = 0;
  std::array<complexf, kBlockSize> samples{};
};

void CheckNng(int rc) {
  if (rc) throw std::runtime_error(nng_strerror(rc));
}

struct SmReplaySocket {
  nng_socket handle{};
  SmReplaySocket() { CheckNng(nng_pair1_open(&handle)); }
  ~SmReplaySocket() { nng_close(handle); }
  SmReplaySocket(const SmReplaySocket&) = delete;
  SmReplaySocket& operator=(const SmReplaySocket&) = delete;
};

void SliceStream(nng_socket input, int offset, int64_t center,
                 std::ostream& output) {
  SmSlicer slicer(offset);
  SmToneDecoder decoder;
  uint64_t start = 0, expected = 0;
  bool active = false;
  std::vector<complexf> samples;
  for (;;) {
    SmBlock block;
    size_t size = sizeof(block);
    CheckNng(nng_recv(input, &block, &size, 0));
    if (size != sizeof(block) || block.count > kBlockSize)
      throw std::runtime_error("invalid internal IQ block");
    if (!block.count) break;
    if (!active || block.start != expected) {
      decoder.Finish(start, center, output);
      slicer.Reset();
      start = block.start;
      active = true;
    }
    samples.assign(block.samples.begin(), block.samples.begin() + block.count);
    slicer.Process(samples, decoder);
    expected = block.start + block.count;
  }
  decoder.Finish(start, center, output);
}

void ReadReplay(std::istream& input, nng_socket socket) {
  SmBlock block;
  uint64_t previous = 0;
  auto flush = [&] {
    CheckNng(nng_send(socket, &block, sizeof(block), 0));
    block.count = 0;
  };
  size_t line_number = 0;
  std::string line;
  while (std::getline(input, line)) {
    ++line_number;
    std::istringstream row(line);
    std::string field, extra;
    complexf sample{};
    uint64_t index = 0;
    row >> field;
    auto parsed =
        std::from_chars(field.data(), field.data() + field.size(), index);
    if (parsed.ec != std::errc{} || parsed.ptr != field.data() + field.size() ||
        !(row >> sample.i >> sample.q) || row >> extra ||
        !std::isfinite(sample.i) || !std::isfinite(sample.q) ||
        (line_number > 1 && index <= previous)) {
      throw std::runtime_error("invalid replay sample at line " +
                               std::to_string(line_number));
    }
    if (block.count && index - previous != 1) flush();
    if (!block.count) block.start = index;
    block.samples[block.count++] = sample;
    if (block.count == kBlockSize) flush();
    previous = index;
  }
  if (!input.eof() || input.bad())
    throw std::runtime_error("error reading replay");
  if (block.count) flush();
  flush();  // Explicit EOF follows the last block on the same pipe.
}
}  // namespace

void RunReplay(std::istream& input, std::ostream& output, int64_t receiver_hz,
               int64_t slice_hz) {
  if (receiver_hz < 0 || slice_hz < 0 ||
      std::abs(slice_hz - receiver_hz) > 18000)
    throw std::runtime_error(
        "12 kHz slice falls outside the receiver passband");
  SmReplaySocket capture, slicing;
  auto endpoint =
      "inproc://skim-replay-" + std::to_string(nng_socket_id(capture.handle));
  CheckNng(nng_socket_set_int(capture.handle, NNG_OPT_SENDBUF, 4));
  CheckNng(nng_socket_set_int(slicing.handle, NNG_OPT_RECVBUF, 4));
  CheckNng(nng_socket_set_ms(capture.handle, NNG_OPT_SENDTIMEO, 5000));
  CheckNng(nng_listen(slicing.handle, endpoint.c_str(), nullptr, 0));
  CheckNng(nng_dial(capture.handle, endpoint.c_str(), nullptr, 0));
  auto worker = std::async(std::launch::async, [&, socket = slicing.handle] {
    SliceStream(socket, static_cast<int>(slice_hz - receiver_hz), slice_hz,
                output);
  });
  try {
    ReadReplay(input, capture.handle);
  } catch (...) {
    nng_close(slicing.handle);
    slicing.handle = NNG_SOCKET_INITIALIZER;
    worker.wait();
    throw;
  }
  worker.get();
}
}  // namespace skim
