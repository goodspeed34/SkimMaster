#include "decoder/text_modes.h"

#include <fftw3.h>
#include <morsedec.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <memory>
#include <regex>
#include <stdexcept>

#include "dsp/fft_lock.h"

namespace skim {
namespace {
constexpr double kPi = 3.141592653589793;
constexpr int kRttyRate = 12000;

struct SmRttyOptions {
  double shift_hz;
  double baud;
  bool reverse;
  size_t max_channels;
  float threshold;
};

// ITA2 asynchronous, least-significant data bit first. The space tone sits
// shift_hz below the mark tone; reverse swaps them.
class SmRtty {
 public:
  SmRtty(double mark, const SmRttyOptions& options)
      : bit_(kRttyRate / options.baud),
        reverse_(options.reverse),
        history_{std::vector<std::complex<double>>(
                     std::max<size_t>(1, std::lround(bit_ / 2))),
                 std::vector<std::complex<double>>(
                     std::max<size_t>(1, std::lround(bit_ / 2)))} {
    rotation_[0] =
        std::polar(1., -2 * kPi * (mark - options.shift_hz) / kRttyRate);
    rotation_[1] = std::polar(1., -2 * kPi * mark / kRttyRate);
  }
  template <class Emit>
  void Process(std::complex<float> iq, Emit emit) {
    // A half-bit correlator distinguishes mark and space before bit sampling.
    for (int c = 0; c < 2; ++c) {
      auto value = static_cast<std::complex<double>>(iq) * osc_[c];
      sums_[c] += value - history_[c][head_];
      history_[c][head_] = value;
      osc_[c] *= rotation_[c];
    }
    head_ = (head_ + 1) % history_[0].size();
    bool mark = (std::norm(sums_[1]) > std::norm(sums_[0])) != reverse_;
    if (next_ < 0) {
      if (last_mark_ && !mark) {
        next_ = 1.5 * bit_;
        bits_ = code_ = 0;
      }
    } else if (--next_ <= 0) {
      if (bits_ < 5) {
        if (mark) code_ |= 1 << bits_;
        ++bits_;
        next_ += bit_;
      } else {
        if (mark) Character(emit);
        next_ = -1;
      }
    }
    last_mark_ = mark;
  }

 private:
  template <class Emit>
  void Character(Emit emit) {
    static constexpr char letters[] = "\0E\nA SIU\rDRJNFCKTZLWHYPQOBG\0MXV\0";
    static constexpr char figures[] =
        "\0"
        "3\n- '87\r$4\a,!:(5\")2#6019?&\0./;\0";
    if (code_ == 27 || code_ == 31) {
      figures_ = code_ == 27;
      return;
    }
    char c = (figures_ ? figures : letters)[code_];
    if (c == '\n' || c == '\r' || line_.size() >= 256) {
      if (!line_.empty()) emit(line_);
      line_.clear();
    }
    if (c >= 32 && c <= 126) line_ += c;
  }
  const double bit_;
  const bool reverse_;
  int bits_ = 0, code_ = 0;
  size_t head_ = 0;
  bool last_mark_ = false, figures_ = false;
  double next_ = -1;
  std::array<std::complex<double>, 2> sums_{}, osc_{{1, 1}}, rotation_{};
  // A half bit of each tone for the sliding correlator.
  std::array<std::vector<std::complex<double>>, 2> history_;
  std::string line_;
};

// Accumulate energy in mark/space pairs over several symbols before opening a
// channel. Replay the acquisition window into each newly detected channel.
class SmRttyBank {
 public:
  explicit SmRttyBank(const SmRttyOptions& options) : options_(options) {
    Reset();
    std::lock_guard<std::mutex> lock(FftPlannerMutex());
    plan_ = fftwf_plan_dft_1d(kBins,
                              reinterpret_cast<fftwf_complex*>(frame_.data()),
                              reinterpret_cast<fftwf_complex*>(fft_.data()),
                              FFTW_FORWARD, FFTW_ESTIMATE);
    if (!plan_)
      throw std::runtime_error("cannot initialize RTTY spectral search");
    for (int i = 0; i < kBins; ++i)
      window_[i] = .5f - .5f * std::cos(2 * kPi * i / kBins);
  }
  ~SmRttyBank() {
    std::lock_guard<std::mutex> lock(FftPlannerMutex());
    fftwf_destroy_plan(plan_);
  }
  void Reset() {
    channels_.clear();
    age_.clear();
    fill_ = frames_ = 0;
    power_.fill(0);
  }
  template <class Emit>
  void Process(std::complex<float> iq, Emit emit) {
    for (auto& [hz, decoder] : channels_)
      decoder.Process(iq, [&](const std::string& line) { emit(line, hz); });
    acquisition_[frames_ * kBins + fill_] = iq;
    frame_[fill_] = iq * window_[fill_];
    if (++fill_ < kBins) return;
    fill_ = 0;
    fftwf_execute(plan_);
    for (int i = 0; i < kBins; ++i)
      power_[i] += std::norm(fft_[(i + kBins / 2) % kBins]);
    if (++frames_ != 3) return;
    frames_ = 0;
    auto sorted = power_;
    std::nth_element(sorted.begin(), sorted.begin() + kBins / 2, sorted.end());
    float threshold =
        std::max({1e-8f, sorted[kBins / 2] * options_.threshold,
                  *std::max_element(power_.begin(), power_.end()) * .001f});
    std::vector<std::pair<float, int>> peaks;
    const int shift =
        static_cast<int>(std::lround(options_.shift_hz * kBins / kRttyRate));
    for (int i = shift + 1; i < kBins - 1; ++i) {
      if (power_[i] < power_[i - 1] || power_[i] < power_[i + 1]) continue;
      // Require a peak at both tones. Allow one bin for FFT rounding.
      float space_power = 0;
      for (int j = std::max(1, i - shift - 1); j <= i - shift + 1; ++j)
        if (power_[j] >= power_[j - 1] && power_[j] >= power_[j + 1])
          space_power = std::max(space_power, power_[j]);
      float score = std::min(power_[i], space_power);
      if (score > threshold)
        peaks.push_back(
            {score, std::lround((i - kBins / 2) * double(kRttyRate) / kBins)});
    }
    std::sort(peaks.rbegin(), peaks.rend());
    for (auto& age : age_) ++age.second;
    // Nearby peaks can demodulate the same signal through the half-bit
    // correlator. Reuse the strongest channel within its first null, at
    // twice the baud rate (91 Hz at 45.45 baud).
    const int separation = static_cast<int>(std::lround(2 * options_.baud));
    for (auto [score, hz] : peaks) {
      auto near = channels_.lower_bound(hz - separation);
      if (near != channels_.end() && near->first <= hz + separation) {
        age_[near->first] = 0;
        continue;
      }
      if (channels_.size() >= options_.max_channels) continue;
      auto [channel, added] = channels_.try_emplace(hz, hz, options_);
      if (!added) continue;
      for (const auto& sample : acquisition_)
        channel->second.Process(
            sample, [&](const std::string& line) { emit(line, hz); });
      age_[hz] = 0;
    }
    for (auto it = age_.begin(); it != age_.end();) {
      if (it->second > 12) {
        channels_.erase(it->first);
        it = age_.erase(it);
      } else
        ++it;
    }
    power_.fill(0);
  }

 private:
  static constexpr int kBins = 2048;
  const SmRttyOptions options_;
  fftwf_plan plan_{};
  int fill_ = 0, frames_ = 0;
  std::array<float, kBins> window_{}, power_{};
  std::array<std::complex<float>, kBins> frame_{}, fft_{};
  std::array<std::complex<float>, 3 * kBins> acquisition_{};
  std::map<int, SmRtty> channels_;
  std::map<int, int> age_;
};

class SmMorse {
 public:
  explicit SmMorse(const SmSettingValues& options) {
    morsedec_config config;
    md_config_init(&config);
    config.sample_rate = 48000;
    config.min_frequency = static_cast<float>(options.at("low_hz"));
    config.max_frequency = static_cast<float>(options.at("high_hz"));
    if (config.min_frequency >= config.max_frequency)
      throw std::runtime_error("CW low_hz must be below high_hz");
    config.max_channels = static_cast<size_t>(options.at("max_channels"));
    config.initial_wpm = static_cast<float>(options.at("initial_wpm"));
    std::lock_guard<std::mutex> lock(FftPlannerMutex());
    decoder_.reset(md_new_iq_with_threads(&config, 1));
    if (!decoder_) throw std::runtime_error("CW decoder initialization failed");
  }
  ~SmMorse() {
    std::lock_guard<std::mutex> lock(FftPlannerMutex());
    decoder_.reset();
  }
  void Reset() {
    md_reset(decoder_.get());
    last_.clear();
  }
  template <class Emit>
  void Process(const std::complex<float>* data, size_t count, Emit emit) {
    if (!md_process_iq(decoder_.get(), reinterpret_cast<const float*>(data),
                       count))
      throw std::runtime_error("CW decoder failed");
    auto active = md_get_channel_count(decoder_.get());
    std::map<uint64_t, uint64_t> next;
    for (size_t i = 0; i < active; ++i) {
      morsedec_channel channel{};
      if (!md_get_channel(decoder_.get(), i, &channel)) continue;
      const morsedec_character* chars = nullptr;
      size_t n = 0;
      if (!md_get_characters(decoder_.get(), channel.id, &chars, &n)) continue;
      auto last = last_[channel.id];
      std::string word;
      for (size_t j = 0; j < n; ++j) {
        word += static_cast<char>(chars[j].character);
        if (chars[j].word_end && j + 1 < n) {
          static const std::regex callsign(
              "[A-Z0-9/]*[A-Z0-9][0-9][A-Z]+(/[A-Z0-9]+)?");
          if (chars[j].ordinal > last &&
              chars[j].word_type == MORSEDEC_WORD_CALLSIGN &&
              std::regex_match(word, callsign))
            emit(word, std::llround(channel.frequency), channel.snr,
                 channel.wpm);
          last = std::max(last, chars[j].ordinal);
          word.clear();
        }
      }
      next[channel.id] = last;
    }
    last_ = std::move(next);
  }

 private:
  std::unique_ptr<morsedec_decoder_t, decltype(&md_delete)> decoder_{nullptr,
                                                                     md_delete};
  std::map<uint64_t, uint64_t> last_;
};
}  // namespace

const std::vector<SmSetting>& TextDecoderSettings(bool cw) {
  using Kind = SmSettingKind;
  static const std::vector<SmSetting> morse{
      {"low_hz", "Lowest offset (Hz)",
       "Lower edge of the CW search, relative to the slice center.",
       Kind::Integer, -23000, -24000, 23999},
      {"high_hz", "Highest offset (Hz)",
       "Upper edge of the CW search, relative to the slice center.",
       Kind::Integer, 23000, -23999, 24000},
      {"max_channels", "Channels", "Signals tracked at once across the slice.",
       Kind::Integer, 32, 1, 256},
      {"initial_wpm", "Initial speed (WPM)",
       "Starting speed estimate for new signals. 0 uses the decoder default "
       "of about 23 WPM.",
       Kind::Integer, 0, 0, 60},
  };
  static const std::vector<SmSetting> rtty{
      {"shift_hz",
       "Shift (Hz)",
       "Spacing between the mark and space tones.",
       Kind::Choice,
       170,
       0,
       0,
       {85, 170, 425, 850}},
      {"baud",
       "Baud rate",
       "ITA2 symbol rate.",
       Kind::Choice,
       45.45,
       0,
       0,
       {45.45, 50, 75}},
      {"reverse", "Reverse", "Swap the mark and space tones.", Kind::Boolean, 0,
       0, 1},
      {"max_channels", "Channels", "Signals decoded at once across the slice.",
       Kind::Integer, 16, 1, 64},
      {"threshold", "Detection threshold",
       "Tone pair power over the median spectrum needed to open a channel. "
       "Lower values find weaker signals and more false channels.",
       Kind::Decimal, 32, 4, 256},
  };
  return cw ? morse : rtty;
}

void DecodeText(bool cw, const SmSettingValues& options, const IqStream& stream,
                const TextSpot& on_spot, const std::function<void()>& gap) {
  uint64_t index = 0;
  std::unique_ptr<SmMorse> morse;
  if (cw) morse = std::make_unique<SmMorse>(options);
  std::unique_ptr<SmRttyBank> rtty;
  const double baud = cw ? 0 : options.at("baud");
  if (!cw)
    rtty = std::make_unique<SmRttyBank>(
        SmRttyOptions{options.at("shift_hz"), baud, options.at("reverse") != 0,
                      static_cast<size_t>(options.at("max_channels")),
                      static_cast<float>(options.at("threshold"))});
  stream(
      [&](const std::complex<float>* buf, size_t count) {
        auto emit = [&](std::string text, int64_t offset, std::string call,
                        std::optional<double> snr,
                        std::optional<double> speed) {
          on_spot({offset, std::move(text), std::move(call), {}, snr, speed},
                  index);
        };
        if (cw) {
          morse->Process(
              buf, count,
              [&](const std::string& call, int64_t hz, double snr,
                  double speed) { emit(call, hz, call, snr, speed); });
          index += count;
        } else
          for (size_t i = 0; i < count; ++i, ++index) {
            rtty->Process(buf[i], [&](const std::string& line, int mark) {
              static const std::regex cq(
                  R"(\b(?:CQ|DE) +([A-Z0-9/]*[0-9][A-Z]+)\b)");
              std::smatch match;
              bool identified = std::regex_search(line, match, cq);
              if (identified) emit(line, mark, match[1].str(), {}, baud);
            });
          }
      },
      [&] {
        if (cw)
          morse->Reset();
        else
          rtty->Reset();
        gap();
        index = 0;
      });
}
}  // namespace skim
