#include "app/live.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string_view>

#include "app/instance.h"
#include "core/clock_health.h"
#include "decoder/session.h"
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

std::shared_ptr<const SmSlice> StandaloneSlice(const std::string& mode,
                                               const char* device, int64_t dial,
                                               int rate, const char* receiver) {
  const auto instance = SmInstance::FromReceiverPlan({
      {"receiver", receiver},
      {"device", device},
      {"rate", rate},
      {"center_hz", dial},
      {"slices", nlohmann::json::array({{{"mode", mode},
                                         {"dial_hz", dial},
                                         {"slice_id", "receive"},
                                         {"decoder_id", receiver}}})},
  });
  return instance->find_slice("receive");
}
}  // namespace

void ReceiveSstv(const char* device, int64_t dial, int rate,
                 const char* gallery, const IqReader& read) {
  const auto slice = StandaloneSlice("SSTV", device, dial, rate, "sstv");
  ReceiveSstv(*slice, dial, rate, gallery, read);
}

void ReceiveSstv(const SmSlice& slice, int64_t dial, int rate,
                 const char* gallery, const IqReader& read) {
  DecodeSstv(rate, gallery, [&](const Samples& samples, const auto& gap) {
    read(slice.receiver().device().c_str(), dial, rate, samples, gap);
  });
}

void ReceiveMode(const std::string& mode, const char* device, int64_t dial,
                 const char* receiver, const char* database, int64_t first_ms,
                 const IqReader& read) {
  const auto slice = StandaloneSlice(mode, device, dial,
                                     mode == "CW" ? 48000 : 12000, receiver);
  ReceiveSlice(*slice, dial, database, first_ms, read);
}

void ReceiveSlice(const SmSlice& slice, int64_t dial, const char* database,
                  int64_t first_ms, const IqReader& read,
                  const DecoderRoute* route) {
  const auto& receiver = slice.receiver();
  const auto* device = receiver.device().c_str();
  const auto* mode = ModeName(slice.mode());
  if (slice.mode() == SmMode::Cw || slice.mode() == SmMode::Rtty)
    return ReceiveText(mode, device, dial, receiver.id().c_str(), database,
                       first_ms, read);

  const auto* slot_decoder = slice.decoder().slot_decoder();
  if (!slot_decoder) throw std::runtime_error("expected a slot decoder");
  if (!route) {
    return CollectDecoderResults(
        [&](const std::string& endpoint, const std::atomic_bool& stop) {
          DecoderRoute local{slot_decoder->id(), endpoint, stop};
          ReceiveSlice(slice, dial, database, first_ms, read, &local);
        },
        [&](DecoderResult result) {
          PublishSlot(std::move(result), dial, receiver.id().c_str(), mode,
                      database, !IsReplayReceiver(device));
        });
  }
  const int rate = slot_decoder->input_sample_rate();
  const int length = SlotSamples(mode);
  std::vector<float> slot;
  slot.reserve(length);
  auto decoder = slot_decoder->Start(*route);
  size_t dropped_slots = 0;
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
    if (route->stop.load()) throw std::runtime_error("slot collection stopped");
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
      if (!decoder->Submit({utc, std::move(slot)}, file)) {
        ++dropped_slots;
        std::cerr << "Decoder busy, skipped UTC slot " << utc << '\n';
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
  decoder->Finish();
  if (dropped_slots)
    std::cerr << "Dropped " << dropped_slots << " slots for decoder "
              << slot_decoder->id() << '\n';
}
void PublishSlot(DecoderResult result, int64_t dial, const char* receiver,
                 const char* mode, const char* database, bool live) {
  ResolveSlotFrequencies(result.spots, dial);
  SaveSlot(database, receiver, mode, result.utc_ms, result.spots, live);
  for (const auto& spot : result.spots)
    std::cout << spot.frequency << ' ' << spot.message << std::endl;
}
}  // namespace skim
