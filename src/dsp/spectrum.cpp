#include "dsp/spectrum.h"

#include <fftw3.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <nlohmann/json.hpp>
#include <utility>

#include "core/database.h"
#include "core/schema.h"
#include "dsp/fft_lock.h"

namespace skim {
namespace {
constexpr double kPi = 3.14159265358979323846;
// Frames averaged into each one-second spectrum.
constexpr int kFramesPerSecond = 16;

int64_t UtcSeconds() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

double Median(std::vector<double>& values) {
  auto middle = values.begin() + values.size() / 2;
  std::nth_element(values.begin(), middle, values.end());
  return *middle;
}
}  // namespace

double NoiseFloor(const std::vector<double>& power) {
  if (power.empty()) return 0;
  const size_t edge = power.size() / 10;
  std::vector<double> central(power.begin() + edge, power.end() - edge);
  if (central.empty()) central = power;
  std::vector<double> sorted = central;
  const double limit = Median(sorted) * 4;  // 6 dB
  std::vector<double> quiet;
  quiet.reserve(central.size());
  for (double value : central)
    if (value <= limit) quiet.push_back(value);
  return Median(quiet);
}

struct SmSpectrum::SmState {
  SmState(std::string database, std::string receiver, int rate, int64_t center)
      : database(std::move(database)),
        receiver(std::move(receiver)),
        rate(rate),
        center(center),
        window(kSpectrumFft),
        power(kSpectrumFft) {
    for (int i = 0; i < kSpectrumFft; ++i) {
      window[i] =
          static_cast<float>(0.5 - 0.5 * std::cos(2 * kPi * i / kSpectrumFft));
      window_sum += window[i];
      window_energy += double(window[i]) * window[i];
    }
    std::lock_guard<std::mutex> lock(FftPlannerMutex());
    buffer = static_cast<fftwf_complex*>(
        fftwf_malloc(sizeof(fftwf_complex) * kSpectrumFft));
    if (!buffer) throw std::bad_alloc();
    plan = fftwf_plan_dft_1d(kSpectrumFft, buffer, buffer, FFTW_FORWARD,
                             FFTW_ESTIMATE);
  }
  ~SmState() {
    std::lock_guard<std::mutex> lock(FftPlannerMutex());
    fftwf_destroy_plan(plan);
    fftwf_free(buffer);
  }

  void Frame(const std::complex<float>* samples) {
    for (int i = 0; i < kSpectrumFft; ++i) {
      buffer[i][0] = samples[i].real() * window[i];
      buffer[i][1] = samples[i].imag() * window[i];
    }
    fftwf_execute(plan);
    for (int i = 0; i < kSpectrumFft; ++i)
      power[i] += double(buffer[i][0]) * buffer[i][0] +
                  double(buffer[i][1]) * buffer[i][1];
    ++frames;
  }

  void Publish(int64_t utc) {
    // FFT order puts 0 Hz first; shift so the row runs from -rate/2 up.
    std::vector<double> shifted(kSpectrumFft);
    for (int i = 0; i < kSpectrumFft; ++i)
      shifted[i] = power[(i + kSpectrumFft / 2) % kSpectrumFft] / frames;
    std::fill(power.begin(), power.end(), 0.0);
    frames = 0;
    // A full-scale sinusoid reaches (window sum)^2 in one bin.
    const double tone = window_sum * window_sum;
    constexpr int group = kSpectrumFft / kSpectrumBins;
    std::vector<int> levels(kSpectrumBins);
    for (int i = 0; i < kSpectrumBins; ++i) {
      double sum = 0;
      for (int j = 0; j < group; ++j) sum += shifted[i * group + j];
      const double level = 10 * std::log10(std::max(sum / group / tone, 1e-16));
      levels[i] =
          std::isfinite(level)
              ? static_cast<int>(std::lround(std::clamp(level, -160., 0.)))
              : -160;
    }
    SmDatabase db(database.c_str());
    auto row = db.Prepare(
        "INSERT OR REPLACE INTO spectra(receiver,utc,center_hz,rate,bins) "
        "VALUES(?,?,?,?,?)");
    db.Text(row.get(), 1, receiver);
    db.Check(sqlite3_bind_int64(row.get(), 2, utc));
    db.Check(sqlite3_bind_int64(row.get(), 3, center));
    db.Check(sqlite3_bind_int(row.get(), 4, rate));
    db.Text(row.get(), 5, nlohmann::json(levels).dump());
    db.Done(row.get());

    const int64_t minute = utc / 60 * 60;
    if (minute_start == 0) minute_start = minute;
    if (minute != minute_start) {
      if (minute_count) {
        auto floor = db.Prepare(
            "INSERT OR REPLACE INTO noise_floor"
            "(receiver,utc,center_hz,rate,floor_db,samples) "
            "VALUES(?,?,?,?,?,?)");
        db.Text(floor.get(), 1, receiver);
        db.Check(sqlite3_bind_int64(floor.get(), 2, minute_start));
        db.Check(sqlite3_bind_int64(floor.get(), 3, center));
        db.Check(sqlite3_bind_int(floor.get(), 4, rate));
        db.Check(sqlite3_bind_double(
            floor.get(), 5, 10 * std::log10(minute_sum / minute_count)));
        db.Check(sqlite3_bind_int(floor.get(), 6, minute_count));
        db.Done(floor.get());
      }
      minute_start = minute;
      minute_sum = 0;
      minute_count = 0;
      db.Exec("DELETE FROM spectra WHERE utc<unixepoch()-3600");
      if (minute % 3600 == 0)
        db.Exec("DELETE FROM noise_floor WHERE utc<unixepoch()-7*86400");
    }
    // White noise of density N per Hz gives N * rate * window energy per bin.
    const double density =
        NoiseFloor(shifted) / (window_energy * static_cast<double>(rate));
    if (std::isfinite(density) && density > 0) {
      minute_sum += density;
      ++minute_count;
    }
  }

  const std::string database, receiver;
  const int rate;
  const int64_t center;
  std::vector<float> window;
  double window_sum = 0, window_energy = 0;
  fftwf_complex* buffer = nullptr;
  fftwf_plan plan = nullptr;
  std::vector<double> power;
  int frames = 0;
  int64_t second = 0;
  int64_t minute_start = 0;
  double minute_sum = 0;
  int minute_count = 0;
};

SmSpectrum::SmSpectrum(std::string database, std::string receiver, int rate,
                       int64_t center)
    : state_(std::make_unique<SmState>(std::move(database), std::move(receiver),
                                       rate, center)) {
  InitStationSchema(state_->database.c_str());
}
SmSpectrum::SmSpectrum(SmSpectrum&&) noexcept = default;
SmSpectrum& SmSpectrum::operator=(SmSpectrum&&) noexcept = default;
SmSpectrum::~SmSpectrum() = default;

void SmSpectrum::Observe(const std::complex<float>* samples, size_t count,
                         int64_t utc_seconds) {
  auto& s = *state_;
  const auto utc = utc_seconds < 0 ? UtcSeconds() : utc_seconds;
  if (utc != s.second) {
    if (s.frames) s.Publish(s.second);
    s.second = utc;
  }
  // Average up to kFramesPerSecond frames from the start of each second.
  for (size_t offset = 0;
       s.frames < kFramesPerSecond && offset + kSpectrumFft <= count;
       offset += kSpectrumFft)
    s.Frame(samples + offset);
}
}  // namespace skim
