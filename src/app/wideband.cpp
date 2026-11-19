#include "app/wideband.h"

#include <nng/nng.h>
#include <nng/protocol/pair1/pair.h>

#include <atomic>
#include <cmath>
#include <complex>
#include <cstring>
#include <fstream>
#include <future>
#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>
#include <set>
#include <thread>

#include "app/live.h"
#include "app/slice_tuning.h"
#include "dsp/spectrum.h"

extern "C" {
#include <libcsdr.h>
}

namespace skim {
namespace {
using Json = nlohmann::json;
void Check(int rc) {
  if (rc) throw std::runtime_error(nng_strerror(rc));
}
struct Socket {
  nng_socket socket{};
  Socket() { Check(nng_pair1_open(&socket)); }
  ~Socket() { nng_close(socket); }
  Socket(const Socket&) = delete;
};
struct Header {
  uint64_t sequence, generation;
};
// A windowed-sinc FIR tabulated at 64 fractional input phases. Integer
// phase accumulation keeps recorded slot boundaries stable across blocks.
class FractionalDecimator {
 public:
  FractionalDecimator(int input_rate, int output_rate, int length)
      : input_rate_(input_rate),
        output_rate_(output_rate),
        length_(length),
        phases_(kPhases, std::vector<float>(length)) {
    constexpr double pi = 3.14159265358979323846;
    double cutoff = 0.45 * output_rate / input_rate;
    for (int phase = 0; phase < kPhases; ++phase) {
      auto& taps = phases_[phase];
      double gain = 0;
      for (int j = 0; j < length; ++j) {
        double x = j - length / 2 - static_cast<double>(phase) / kPhases;
        double sinc =
            x == 0 ? 2 * cutoff : std::sin(2 * pi * cutoff * x) / (pi * x);
        taps[j] = sinc * (0.54 - 0.46 * std::cos(2 * pi * j / (length - 1)));
        gain += taps[j];
      }
      for (auto& tap : taps) tap /= gain;
    }
  }

  void Reset() { position_ = 0; }

  void Process(std::vector<complexf>& input, std::vector<complexf>& output) {
    output.clear();
    while (position_ / output_rate_ + length_ <= input.size()) {
      auto index = position_ / output_rate_;
      auto phase = (position_ % output_rate_) * kPhases / output_rate_;
      const float* taps = phases_[phase].data();
      const complexf* samples = input.data() + index;
      complexf sum{};
      for (int j = 0; j < length_; ++j) {
        sum.i += taps[j] * samples[j].i;
        sum.q += taps[j] * samples[j].q;
      }
      output.push_back(sum);
      position_ += input_rate_;
    }
    auto consumed = position_ / output_rate_;
    input.erase(input.begin(), input.begin() + consumed);
    position_ %= output_rate_;
  }

 private:
  static constexpr int kPhases = 64;
  const int input_rate_, output_rate_, length_;
  uint64_t position_ = 0;
  std::vector<std::vector<float>> phases_;
};

class Slice {
 public:
  Slice(const Json& cfg, int rate, int64_t center, const std::string& device,
        const std::string& receiver, const Json& root)
      : file_(IsReplayReceiver(device)) {
    std::string mode = cfg.at("mode");
    const auto frequency = SliceReference(cfg, mode, center);
    if (frequency <= 0 || frequency > 1000000000000LL)
      throw std::runtime_error("invalid slice frequency");
    out_rate_ = mode == "CW" ? 48000 : 12000;
    int64_t offset = frequency - center;
    if (IsTimedDecoder(mode))
      offset += 1500;
    else if (!IsSkimmer(mode) && mode != "SSTV")
      throw std::runtime_error("unknown slice mode");
    if (rate < out_rate_ || std::abs(offset) + out_rate_ / 2 > rate / 2)
      throw std::runtime_error(
          "slice outside passband or incompatible receiver sample rate");
    step_ = rate / out_rate_;
    shift_ = -static_cast<float>(offset) / rate;
    filter_length_ = 16 * ((rate + out_rate_ - 1) / out_rate_) + 1;
    if (rate % out_rate_)
      fractional_ = std::make_unique<FractionalDecimator>(rate, out_rate_,
                                                          filter_length_);
    else {
      taps_.resize(filter_length_);
      firdes_lowpass_f(taps_.data(), taps_.size(), 0.45f / step_,
                       WINDOW_HAMMING);
    }
    if (file_) history_.resize(filter_length_ / 2);
    auto addr = "inproc://skimmaster-slice-" +
                std::to_string(nng_socket_id(rx_.socket));
    Check(nng_socket_set_int(rx_.socket, NNG_OPT_RECVBUF, 8));
    Check(nng_socket_set_ms(tx_.socket, NNG_OPT_SENDTIMEO, file_ ? 100 : 5));
    Check(nng_listen(rx_.socket, addr.c_str(), nullptr, 0));
    Check(nng_dial(tx_.socket, addr.c_str(), nullptr, 0));
    auto db = root.at("database").get<std::string>();
    auto gallery = root.value("gallery", "gallery");
    int64_t utc = root.value("utc_ms", int64_t{IsWavReceiver(device) ? 0 : -1});
    if (file_ && utc < 0)
      throw std::runtime_error("replayed wideband IQ requires utc_ms");
    if (utc > 100000000000000LL) throw std::runtime_error("invalid UTC origin");
    worker_ = std::async(std::launch::async, [this, mode, frequency, device,
                                              receiver, db, gallery, utc] {
      IqReader read = [this](const char*, int64_t, int, const Samples& samples,
                             const auto& gap) { Consume(samples, gap); };
      if (mode == "SSTV")
        ReceiveSstv(device.c_str(), frequency, out_rate_, gallery.c_str(),
                    read);
      else
        ReceiveMode(mode, device.c_str(), frequency, receiver.c_str(),
                    db.c_str(), utc, read);
    });
  }
  ~Slice() {
    if (worker_.valid()) {
      nng_close(rx_.socket);
      worker_.wait();
    }
  }
  void Gap() {
    if (file_) throw std::runtime_error("gap in recorded IQ");
    history_.clear();
    if (fractional_) fractional_->Reset();
    phase_ = 0;
    ++generation_;
  }
  void Push(const std::complex<float>* samples, size_t n) {
    CheckWorker();
    received_ = true;
    input_.resize(n);
    for (size_t i = 0; i < n; ++i)
      input_[i] = {samples[i].real(), samples[i].imag()};
    size_t tail = history_.size();
    history_.resize(tail + n);
    phase_ =
        shift_math_cc(input_.data(), history_.data() + tail, n, shift_, phase_);
    if (fractional_) {
      fractional_->Process(history_, output_);
    } else {
      output_.resize(history_.size() / step_ + 1);
      int count =
          fir_decimate_cc(history_.data(), output_.data(), history_.size(),
                          step_, taps_.data(), taps_.size());
      output_.resize(count);
      history_.erase(history_.begin(), history_.begin() + count * step_);
    }
    const auto count = output_.size();
    if (!count) return;
    nng_msg* msg = nullptr;
    Check(nng_msg_alloc(&msg, sizeof(Header) + count * sizeof(complexf)));
    Header h{seq_++, generation_};
    auto bytes = static_cast<char*>(nng_msg_body(msg));
    std::memcpy(bytes, &h, sizeof(h));
    std::memcpy(bytes + sizeof(h), output_.data(), count * sizeof(complexf));
    Send(msg, file_);
  }
  void Finish() {
    if (file_ && received_) {
      const std::vector<std::complex<float>> tail(filter_length_ / 2);
      Push(tail.data(), tail.size());
    }
    nng_msg* end = nullptr;
    Check(nng_msg_alloc(&end, 0));
    Send(end, true);
    worker_.get();
    if (drops_) std::cerr << "Dropped " << drops_ << " slice blocks\n";
  }

 private:
  void CheckWorker() {
    if (worker_.wait_for(std::chrono::seconds(0)) ==
        std::future_status::ready) {
      worker_.get();
      throw std::runtime_error("slice decoder stopped before end of IQ");
    }
  }
  void Send(nng_msg* msg, bool wait) {
    std::unique_ptr<nng_msg, decltype(&nng_msg_free)> guard(msg, nng_msg_free);
    for (;;) {
      const int rc = nng_sendmsg(tx_.socket, msg, 0);
      if (!rc) {
        guard.release();
        return;
      }
      if (rc != NNG_ETIMEDOUT) Check(rc);
      if (!wait) {
        ++drops_;
        return;
      }
      CheckWorker();
    }
  }
  void Consume(const Samples& samples, const std::function<void()>& gap) {
    uint64_t expected = 0, generation = 0;
    std::vector<std::complex<float>> block;
    for (;;) {
      nng_msg* m = nullptr;
      Check(nng_recvmsg(rx_.socket, &m, 0));
      std::unique_ptr<nng_msg, decltype(&nng_msg_free)> guard(m, nng_msg_free);
      size_t bytes = nng_msg_len(m);
      if (!bytes) return;
      if (bytes < sizeof(Header) || (bytes - sizeof(Header)) % sizeof(complexf))
        throw std::runtime_error("malformed slice block");
      Header h;
      auto buf = static_cast<const char*>(nng_msg_body(m));
      std::memcpy(&h, buf, sizeof(h));
      if (h.sequence != expected || h.generation != generation) gap();
      generation = h.generation;
      expected = h.sequence + 1;
      block.resize((bytes - sizeof(h)) / sizeof(complexf));
      static_assert(sizeof(std::complex<float>) == sizeof(complexf));
      std::memcpy(static_cast<void*>(block.data()), buf + sizeof(h),
                  bytes - sizeof(h));
      samples(block.data(), block.size());
    }
  }
  bool file_;
  bool received_ = false;
  int step_, out_rate_, filter_length_;
  std::unique_ptr<FractionalDecimator> fractional_;
  float shift_, phase_ = 0;
  std::vector<float> taps_;
  std::vector<complexf> input_, history_, output_;
  uint64_t generation_ = 0, seq_ = 0, drops_ = 0;
  Socket tx_, rx_;
  std::future<void> worker_;
};
}  // namespace
void Wideband(const char* config) {
  std::ifstream in(config);
  Wideband(Json::parse(in));
}
void Wideband(const Json& root) {
  std::string device = root.at("device"), id = root.at("receiver");
  int rate = root.at("rate");
  int64_t center = root.at("center_hz");
  if (id.empty() || rate < 12000 || rate > 6144000 || center <= 0 ||
      center > 1000000000000LL || !root.at("slices").is_array())
    throw std::runtime_error("invalid receiver configuration");
  const bool repeat = root.value("repeat", false);
  if (repeat && !IsWavReceiver(device))
    throw std::runtime_error("repeat is available only for WAV receivers");
  Spectrum spectrum(root.at("database"), id, rate, center);
  do {
    // Construct fresh slices on every pass to prevent a slot or text frame
    // from spanning the discontinuity between the end and start of the WAV.
    std::vector<std::unique_ptr<Slice>> slices;
    for (const auto& cfg : root.at("slices"))
      slices.push_back(
          std::make_unique<Slice>(cfg, rate, center, device, id, root));

    const auto started = std::chrono::steady_clock::now();
    uint64_t frames = 0;
    ReadReceiver(
        device.c_str(), center, rate, root.value("gain_db", double(NAN)),
        root.value("ppm", double(NAN)),
        [&](const std::complex<float>* data, size_t n) {
          if (repeat) {
            frames += n;
            auto elapsed = std::chrono::duration<double>(
                static_cast<double>(frames) / rate);
            std::this_thread::sleep_until(started + elapsed);
          }
          spectrum.Observe(data, n);
          for (auto& slice : slices) slice->Push(data, n);
        },
        [&] {
          for (auto& slice : slices) slice->Gap();
        });
    for (auto& slice : slices) slice->Finish();
  } while (repeat);
}
}  // namespace skim
