#include <nng/nng.h>
#include <nng/protocol/reqrep0/rep.h>

#include <boost/dll/runtime_symbol_info.hpp>
#include <boost/dll/shared_library.hpp>
#include <chrono>
#include <stdexcept>

#include "decoder/result_wire.h"
#include "decoder/session.h"
#include "decoder/slot.h"
#include "dsp/fft_lock.h"

namespace skim {
namespace {
constexpr auto kWait = std::chrono::milliseconds(25);

SlotMode ParseMode(const std::string& mode) {
  if (mode == "FT4") return SlotMode::Ft4;
  if (mode == "FT8") return SlotMode::Ft8;
  if (mode == "WSPR") return SlotMode::Wspr;
  throw std::runtime_error("supported slot modes are FT4, FT8 and WSPR");
}

boost::dll::shared_library LoadDecoder(SlotMode mode) {
  std::string name = mode == SlotMode::Wspr ? "decoderwspr" : "decoderftx";
  return boost::dll::shared_library(
      boost::dll::program_location().parent_path() /
      ("lib" + name + SKIM_MODULE_SUFFIX));
}
}  // namespace

struct SlotDecoderSession::State {
  State(SlotMode mode, const DecoderRoute& route, int rate, SampleType type)
      : module(LoadDecoder(mode)), cancelled(route.stop) {
    const auto* decoder =
        module.get<const SlotDecoder*(SlotMode)>("skim_slot_decoder_v2")(mode);
    if (!decoder) throw std::runtime_error("slot decoder mode unavailable");
    const auto& metadata = decoder->metadata();
    if (metadata.supported_mode != mode || metadata.required_type != type ||
        rate < metadata.min_samplerate || rate > metadata.max_samplerate)
      throw std::runtime_error(
          "slot decoder input is incompatible with metadata");
    worker = decoder->start(route.decoder_id, queue, route.result_endpoint,
                            stop, FftPlannerMutex());
  }

  ~State() {
    stop = true;
    if (worker.valid()) worker.wait();
  }

  void CheckRunning() {
    if (cancelled.load()) throw std::runtime_error("slot collection stopped");
    if (worker.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
      worker.get();
      throw std::runtime_error("slot decoder stopped before end of input");
    }
  }

  boost::dll::shared_library module;
  const std::atomic_bool& cancelled;
  std::atomic_bool stop{false};
  DecoderInput queue{1};
  std::future<void> worker;
};

SlotDecoderSession::SlotDecoderSession(const std::string& mode,
                                       const DecoderRoute& route,
                                       int sample_rate, SampleType sample_type)
    : state_(std::make_unique<State>(ParseMode(mode), route, sample_rate,
                                     sample_type)) {}

SlotDecoderSession::~SlotDecoderSession() = default;

bool SlotDecoderSession::Submit(DecoderSlot slot, bool wait) {
  std::optional<DecoderSlot> item(std::move(slot));
  state_->CheckRunning();
  if (!wait) return state_->queue.try_enqueue(std::move(item));
  while (!state_->queue.wait_enqueue_timed(std::move(item), kWait))
    state_->CheckRunning();
  return true;
}

void SlotDecoderSession::Finish() {
  while (!state_->queue.wait_enqueue_timed(std::nullopt, kWait))
    state_->CheckRunning();
  state_->worker.get();
}

void CollectDecoderResults(
    const std::function<void(const std::string&, const std::atomic_bool&)>&
        produce,
    const std::function<void(DecoderResult)>& collect) {
  using namespace decoder_wire;
  Socket socket(nng_rep0_open);
  Check(nng_socket_set_ms(socket.socket, NNG_OPT_RECVTIMEO, kWait.count()));
  Check(nng_socket_set_ms(socket.socket, NNG_OPT_SENDTIMEO, 5000));
  const std::string endpoint = "inproc://skimmaster-results-" +
                               std::to_string(nng_socket_id(socket.socket));
  Check(nng_listen(socket.socket, endpoint.c_str(), nullptr, 0));
  std::atomic_bool stop{false};
  auto producer =
      std::async(std::launch::async, [&] { produce(endpoint, stop); });
  std::exception_ptr error;
  try {
    while (producer.wait_for(std::chrono::seconds(0)) !=
           std::future_status::ready) {
      auto message = Receive(socket.socket, true);
      if (!message) continue;
      if (!error) {
        try {
          collect(Decode(message.get()));
        } catch (...) {
          error = std::current_exception();
          stop = true;
        }
      }
      Send(socket.socket, error ? "rejected" : "");
    }
  } catch (...) {
    stop = true;
    if (!error) error = std::current_exception();
  }
  if (error) std::rethrow_exception(error);
  producer.get();
}

void ResolveSlotFrequencies(std::vector<Spot>& spots, int64_t reference_hz) {
  for (auto& spot : spots) {
    spot.frequency += reference_hz;
    if (spot.wspr) spot.wspr->dial = reference_hz;
  }
}

std::vector<Spot> DecodeAudio(const std::string& mode, std::vector<float> audio,
                              int64_t dial, int64_t utc_ms) {
  if (audio.size() != static_cast<size_t>(SlotSamples(mode)))
    throw std::runtime_error("incomplete slot");
  std::vector<Spot> spots;
  CollectDecoderResults(
      [&](const std::string& endpoint, const std::atomic_bool& stop) {
        SlotDecoderSession session(mode, {"file", endpoint, stop}, 12000,
                                   SampleType::MonoAudio);
        session.Submit({utc_ms, std::move(audio)}, true);
        session.Finish();
      },
      [&](DecoderResult result) {
        spots = std::move(result.spots);
        ResolveSlotFrequencies(spots, dial);
      });
  return spots;
}
}  // namespace skim
