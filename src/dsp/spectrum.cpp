#include "dsp/spectrum.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <nlohmann/json.hpp>

#include "core/database.h"

namespace skim {
namespace {
constexpr double pi = 3.14159265358979323846;
}
void InitSpectrum(const char* path) {
  Database db(path);
  db.Exec(
      "CREATE TABLE IF NOT EXISTS spectra(receiver TEXT,utc INTEGER,center_hz "
      "INTEGER,rate INTEGER,bins TEXT,PRIMARY "
      "KEY(receiver,utc))");
}
Spectrum::Spectrum(std::string database, std::string receiver, int rate,
                   int64_t center)
    : database_(std::move(database)),
      receiver_(std::move(receiver)),
      rate_(rate),
      center_(center) {
  InitSpectrum(database_.c_str());
  for (size_t i = 0; i < window_.size(); ++i)
    window_[i] = 0.5 - 0.5 * std::cos(2 * pi * i / window_.size());
}
void Spectrum::Observe(const std::complex<float>* samples, size_t count) {
  auto utc = std::chrono::duration_cast<std::chrono::seconds>(
                 std::chrono::system_clock::now().time_since_epoch())
                 .count();
  if (utc < next_ || count < 256) return;
  next_ = utc + 1;
  // A radix-2 FFT over a Hann window. Display power relative to a full-scale
  // complex sinusoid. Physical dBm requires receiver-specific calibration.
  std::array<std::complex<double>, 256> bins;
  size_t j = 0;
  for (size_t i = 0; i < bins.size(); ++i) {
    bins[j] = static_cast<std::complex<double>>(samples[i]) * window_[i];
    size_t bit = 128;
    while (bit && (j & bit)) {
      j ^= bit;
      bit >>= 1;
    }
    j ^= bit;
  }
  for (int width = 2; width <= 256; width *= 2) {
    auto step = std::polar(1.0, -2 * pi / width);
    for (int start = 0; start < 256; start += width) {
      std::complex<double> w = 1;
      for (int k = 0; k < width / 2; ++k) {
        auto a = bins[start + k], b = w * bins[start + k + width / 2];
        bins[start + k] = a + b;
        bins[start + k + width / 2] = a - b;
        w *= step;
      }
    }
  }
  std::array<int, 256> levels;
  for (size_t i = 0; i < levels.size(); ++i) {
    double power = std::norm(bins[(i + 128) % 256]) / (128. * 128);
    levels[i] = std::isfinite(power)
                    ? std::lround(std::clamp(
                          10 * std::log10(std::max(power, 1e-14)), -140., 0.))
                    : -140;
  }
  Database db(database_.c_str());
  auto q = db.Prepare(
      "INSERT OR REPLACE INTO spectra(receiver,utc,center_hz,rate,bins) "
      "VALUES(?,?,?,?,?)");
  db.Text(q.get(), 1, receiver_);
  db.Check(sqlite3_bind_int64(q.get(), 2, utc));
  db.Check(sqlite3_bind_int64(q.get(), 3, center_));
  db.Check(sqlite3_bind_int(q.get(), 4, rate_));
  db.Text(q.get(), 5, nlohmann::json(levels).dump());
  db.Done(q.get());
  if (utc % 60 == 0) db.Exec("DELETE FROM spectra WHERE utc<unixepoch()-3600");
}
}  // namespace skim
