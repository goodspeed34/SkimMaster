#include "decoder/sstv.h"

#include <png.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <complex>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace skim {
namespace {
constexpr double kPi = 3.141592653589793;
class SmImageReceiver {
 public:
  SmImageReceiver(int rate, const char* gallery)
      : rate_(rate),
        gallery_(gallery),
        pixels_(320 * 256 * 3),
        rotation_(std::polar(1.0, -2 * kPi * 1900 / rate)),
        gain_(1 - std::exp(-2 * kPi * 900 / rate)) {
    std::filesystem::create_directories(gallery_);
  }
  void Gap() {
    row_ = -1;
    bit_ = -1;
    leader_ = sync_ = 0;
    line_.clear();
    filters_ = {};
    previous_ = {};
  }
  void Sample(std::complex<float> iq) {
    auto z = static_cast<std::complex<double>>(iq) * oscillator_;
    oscillator_ *= rotation_;
    for (auto& filter : filters_) {
      filter += gain_ * (z - filter);
      z = filter;
    }
    double hz = 1900 + std::arg(std::conj(previous_) * z) * rate_ / (2 * kPi);
    previous_ = z;
    if (!std::isfinite(hz) || std::norm(z) < 1e-14) hz = 0;
    if (row_ < 0) {
      SmHeader(hz);
      return;
    }
    if (!line_.empty()) {
      line_.push_back(hz);
      if (line_.size() >= line_size_) {
        Paint();
        line_.clear();
        if (row_ == height_) {
          Save();
          Gap();
        }
      }
      return;
    }
    if (++waiting_ > rate_ / 10) {
      Gap();
      return;
    }
    if (std::abs(hz - 1200) < 65) {
      ++sync_;
      return;
    }
    if (sync_ >= rate_ * 0.003 && sync_ <= rate_ * 0.040) {
      line_.push_back(hz);
      waiting_ = 0;
    }
    sync_ = 0;
  }

 private:
  void SmHeader(double hz) {
    if (bit_ < 0) {
      if (std::abs(hz - 1900) < 65) {
        leader_ = std::min(rate_, leader_ + 1);
        return;
      }
      if (leader_ > rate_ / 5) {
        bit_ = 0;
        elapsed_ = code_ = parity_ = 0;
      }
      leader_ = 0;
      return;
    }
    if (++elapsed_ != std::lround(rate_ * 0.030 * (bit_ + 0.5))) return;
    if (bit_ == 0 || bit_ == 9) {
      if (std::abs(hz - 1200) > 65) {
        Gap();
        return;
      }
    } else {
      int value = hz < 1200;
      if (std::abs(hz - (value ? 1100 : 1300)) > 65) {
        Gap();
        return;
      }
      parity_ ^= value;
      if (bit_ <= 7) code_ |= value << (bit_ - 1);
    }
    if (++bit_ == 10) {
      if (parity_) {
        Gap();
        return;
      }
      first_scottie_ = false;
      height_ = 256;
      switch (code_) {
        case 44:
          name_ = "Martin1";
          scan_ = 0.146432;
          break;
        case 40:
          name_ = "Martin2";
          scan_ = 0.073216;
          break;
        case 60:
          name_ = "Scottie1";
          scan_ = 0.138240;
          first_scottie_ = true;
          break;
        case 56:
          name_ = "Scottie2";
          scan_ = 0.088064;
          first_scottie_ = true;
          break;
        case 8:
          name_ = "Robot36";
          scan_ = 0.088;
          height_ = 240;
          break;
        case 12:
          name_ = "Robot72";
          scan_ = 0.138;
          height_ = 240;
          break;
        default:
          Gap();
          return;
      }
      scan_ *= rate_;
      porch_ = (first_scottie_ ? .0015 : .000572) * rate_;
      line_size_ = std::floor((first_scottie_ ? 2 : 3) * (porch_ + scan_));
      if (height_ == 240)
        line_size_ = std::floor((code_ == 8 ? .141 : .291) * rate_);
      row_ = waiting_ = sync_ = 0;
    }
  }
  static unsigned char Byte(double value) {
    return static_cast<unsigned char>(std::clamp(std::round(value), 0., 255.));
  }
  double Pixel(double start, double width, int x) const {
    auto index = std::min(line_.size() - 1,
                          static_cast<size_t>(start + (x + .5) * width / 320));
    return (line_[index] - 1500) * 255 / 800;
  }
  void ColorScan(int row, int component, double start) {
    for (int x = 0; x < 320; ++x)
      pixels_[(row * 320 + x) * 3 + component] = Byte(Pixel(start, scan_, x));
  }
  void Yuv(int row, int x, double y, double cr, double cb) {
    auto p = &pixels_[(row * 320 + x) * 3];
    p[0] = Byte(y + 1.402 * (cr - 128));
    p[1] = Byte(y - .344136 * (cb - 128) - .714136 * (cr - 128));
    p[2] = Byte(y + 1.772 * (cb - 128));
  }
  void Paint() {
    if (height_ == 240) {
      const double start = .003 * rate_, chroma = start + scan_ + .006 * rate_;
      for (int x = 0; x < 320; ++x) {
        auto y = Pixel(start, scan_, x), c = Pixel(chroma, scan_ / 2, x);
        if (code_ == 12)
          Yuv(row_, x, y, c,
              Pixel(chroma + scan_ / 2 + .006 * rate_, scan_ / 2, x));
        else if (row_ % 2 == 0) {
          previous_y_[x] = y;
          previous_cr_[x] = c;
        } else {
          Yuv(row_ - 1, x, previous_y_[x], previous_cr_[x], c);
          Yuv(row_, x, y, previous_cr_[x], c);
        }
      }
      ++row_;
      return;
    }
    auto span = scan_ + porch_;
    if (code_ == 60 || code_ == 56) {
      double start = porch_;
      if (!first_scottie_) {
        ColorScan(row_, 0, start);
        ++row_;
        start += span;
      }
      first_scottie_ = false;
      if (row_ < height_) {
        ColorScan(row_, 1, start);
        ColorScan(row_, 2, start + span);
      }
      line_size_ = std::floor((row_ == height_ - 1 ? 1 : 3) * span);
    } else {
      for (int c = 0; c < 3; ++c)
        ColorScan(row_, (c + 1) % 3, porch_ + c * span);
      ++row_;
    }
  }
  void Save() {
    auto stamp = std::chrono::system_clock::now().time_since_epoch();
    auto name = std::to_string(
                    std::chrono::duration_cast<std::chrono::microseconds>(stamp)
                        .count()) +
                "-" + name_ + ".png";
    auto path = gallery_ / name;
    auto tmp = path.string() + ".partial";
    png_image image{};
    image.version = PNG_IMAGE_VERSION;
    image.width = 320;
    image.height = height_;
    image.format = PNG_FORMAT_RGB;
    if (!png_image_write_to_file(&image, tmp.c_str(), 0, pixels_.data(), 0,
                                 nullptr)) {
      std::filesystem::remove(tmp);
      throw std::runtime_error(image.message);
    }
    std::filesystem::rename(tmp, path);
    std::cout << path.string() << std::endl;
  }
  int rate_;
  std::filesystem::path gallery_;
  std::vector<unsigned char> pixels_;
  std::complex<double> rotation_, oscillator_{1, 0}, previous_{};
  double gain_;
  std::array<std::complex<double>, 3> filters_{};
  int row_ = -1, bit_ = -1, elapsed_ = 0, code_ = 0, leader_ = 0, sync_ = 0,
      waiting_ = 0, parity_ = 0;
  std::string name_;
  int height_ = 256;
  bool first_scottie_ = false;
  std::array<double, 320> previous_y_{}, previous_cr_{};
  double scan_ = 0, porch_ = 0;
  size_t line_size_ = 0;
  std::vector<double> line_;
};
}  // namespace
void DecodeSstv(int rate, const char* gallery, const IqStream& stream) {
  if (rate < 12000 || rate > 384000)
    throw std::runtime_error("SSTV needs 12 to 384 kHz IQ");
  SmImageReceiver decoder(rate, gallery);
  stream(
      [&](const std::complex<float>* data, size_t count) {
        for (size_t i = 0; i < count; ++i) decoder.Sample(data[i]);
      },
      [&] { decoder.Gap(); });
}
}  // namespace skim
