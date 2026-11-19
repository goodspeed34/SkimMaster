#include "app/live.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string_view>

#include "core/clock_health.h"
#include "decoder/slot.h"
#include "decoder/sstv.h"
#include "decoder/text_modes.h"
#include "store/store.h"

namespace skim {
namespace {
int64_t UtcMilliseconds() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

void ReceiveText(const std::string& mode, const char* device, int64_t center,
                 const char* receiver, const char* database, int64_t first_ms,
                 const IqReader& read) {
  const bool cw = mode == "CW";
  const int rate = cw ? 48000 : 12000;
  const bool live = !IsReplayReceiver(device);
  if (!live && first_ms < 0) first_ms = 0;
  int64_t origin = first_ms;
  DecodeText(
      cw,
      [&](const Samples& samples, const std::function<void()>& gap) {
        read(
            device, center, rate,
            [&](const std::complex<float>* data, size_t count) {
              if (origin < 0)
                origin = UtcMilliseconds() -
                         static_cast<int64_t>(count / (rate / 1000));
              samples(data, count);
            },
            [&] {
              if (!live) throw std::runtime_error("gap in recorded IQ");
              origin = -1;
              gap();
            });
      },
      [&](Spot spot, uint64_t index) {
        spot.frequency += center;
        auto utc = origin + static_cast<int64_t>(index / (rate / 1000));
        SaveSlot(database, receiver, mode.c_str(), utc, {spot}, live);
        std::cout << spot.frequency << ' ' << spot.message << std::endl;
      },
      [] {});
}
}  // namespace

void ReceiveSstv(const char* device, int64_t dial_hz, int rate,
                 const char* gallery, const IqReader& read) {
  DecodeSstv(rate, gallery, [&](const Samples& samples, const auto& gap) {
    read(device, dial_hz, rate, samples, gap);
  });
}

void ReceiveMode(const std::string& mode, const char* device, int64_t dial,
                 const char* receiver, const char* database, int64_t first_ms,
                 const IqReader& read) {
  if (mode == "CW" || mode == "RTTY")
    return ReceiveText(mode, device, dial, receiver, database, first_ms, read);
  constexpr int rate = 12000;
  const int length = SlotSamples(mode);
  std::vector<float> slot;
  slot.reserve(length);
  std::future<void> decoder;
  int64_t position = -1;
  bool contiguous = false;
  auto reset = [&] {
    slot.clear();
    position = -1;
    contiguous = false;
  };
  // Tune halfway into the USB passband. Rotate the complex slice back to
  // positive audio before taking its real part, rejecting the lower sideband.
  // A 129-tap low pass limits the complex slice to roughly +/- 1.5 kHz.
  constexpr int taps = 129;
  std::array<float, taps> filter{};
  double gain = 0;
  for (int i = 0; i < taps; ++i) {
    int n = i - taps / 2;
    filter[i] =
        (n ? std::sin(2 * 3.141592653589793 * 0.13 * n) /
                 (3.141592653589793 * n)
           : 0.26) *
        (0.54 - 0.46 * std::cos(2 * 3.141592653589793 * i / (taps - 1)));
    gain += filter[i];
  }
  for (auto& tap : filter) tap /= gain;
  std::array<std::complex<float>, taps> history{};
  int head = 0;
  int filter_fill = 0;
  const bool wav = IsWavReceiver(device);
  const bool file = IsReplayReceiver(device);
  if (wav && first_ms < 0) first_ms = 0;
  if (file && first_ms < 0)
    throw std::runtime_error("stdin IQ requires its UTC start in milliseconds");
  auto consume = [&](const std::complex<float>* buf, size_t count) {
    if (!file && !ClockSynchronized()) {
      reset();
      return;
    }
    int64_t observed = UtcMilliseconds() * 12 - static_cast<int64_t>(count);
    if (!file && position >= 0 && std::abs(observed - position) > rate / 2)
      reset();
    if (position < 0) {
      position = file ? first_ms * 12 : observed;
      history = {};
      head = 0;
      filter_fill = file ? taps / 2 : 0;
    }
    for (size_t i = 0; i < count; ++i, ++position) {
      history[head] = buf[i];
      head = (head + 1) % taps;
      auto sample_index = position - taps / 2;
      if (filter_fill < taps) ++filter_fill;
      if (sample_index < 0 || filter_fill < taps) continue;
      if (sample_index % length == 0) {
        slot.clear();
        contiguous = true;
      }
      if (!contiguous) continue;
      std::complex<float> band{};
      for (int j = 0; j < taps; ++j)
        band += history[(head + j) % taps] * filter[j];
      auto osc = std::polar(1.0f, static_cast<float>(2 * 3.141592653589793 *
                                                     (sample_index % 8) / 8));
      slot.push_back((osc * band).real());
      if (slot.size() != static_cast<size_t>(length)) continue;
      int64_t utc = (sample_index + 1 - length) / 12;
      if (decoder.valid() &&
          decoder.wait_for(std::chrono::seconds(0)) !=
              std::future_status::ready &&
          !file) {
        std::cerr << "Decoder busy, skipped UTC slot " << utc << '\n';
      } else {
        if (decoder.valid()) decoder.get();
        decoder = std::async(
            std::launch::async, [mode, dial, receiver, database, file,
                                 audio = std::move(slot), utc] {
              auto spots = DecodeAudio(mode, audio, dial);
              SaveSlot(database, receiver, mode.c_str(), utc, spots, !file);
              for (const auto& spot : spots)
                std::cout << spot.frequency << ' ' << spot.message << std::endl;
            });
      }
      contiguous = false;
    }
  };
  read(device, dial + 1500, rate, consume, [&] {
    if (file) throw std::runtime_error("gap in recorded IQ");
    reset();
  });
  if (file && position >= 0) {
    const std::array<std::complex<float>, taps / 2> tail{};
    consume(tail.data(), tail.size());
  }
  if (decoder.valid()) decoder.get();
}
}  // namespace skim
