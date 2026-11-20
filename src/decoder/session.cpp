#include "decoder/session.h"

#include <nng/nng.h>
#include <nng/protocol/reqrep0/rep.h>

#include <boost/dll/shared_library.hpp>
#include <chrono>
#include <stdexcept>

#include "core/plugin_modules.h"
#include "decoder/result_wire.h"
#include "dsp/fft_lock.h"

namespace skim {
namespace {
constexpr auto kWait = std::chrono::milliseconds(25);

SmSlotMode ParseMode(const std::string& mode) {
  if (mode == "FT4") return SmSlotMode::Ft4;
  if (mode == "FT8") return SmSlotMode::Ft8;
  if (mode == "WSPR") return SmSlotMode::Wspr;
  throw std::runtime_error("supported slot modes are FT4, FT8 and WSPR");
}

struct SmLoadedSlotDecoder {
  boost::dll::shared_library module;
  const SmSlotDecoderPlugin* plugin;
};

SmLoadedSlotDecoder LoadDecoder(SmSlotMode mode) {
  for (const auto& path : PluginModules("libdecoder")) {
    boost::dll::fs::error_code error;
    boost::dll::shared_library module(path, error);
    if (error || !module.has("skim_slot_decoder_v2")) continue;
    const auto* plugin = module.get<const SmSlotDecoderPlugin*(SmSlotMode)>(
        "skim_slot_decoder_v2")(mode);
    if (plugin) return {std::move(module), plugin};
  }
  throw std::runtime_error(
      "no installed slot decoder supports the requested mode");
}
}  // namespace

struct SmSlotDecoderSession::SmState {
  SmState(SmSlotMode mode, const SmDecoderRoute& route, int rate,
          SmSampleType type)
      : module(LoadDecoder(mode)), cancelled(route.stop) {
    const auto* decoder = module.plugin;
    const auto& metadata = decoder->metadata();
    if (metadata.supported_mode != mode || metadata.required_type != type ||
        rate < metadata.min_samplerate || rate > metadata.max_samplerate)
      throw std::runtime_error(
          "slot decoder input is incompatible with metadata");
    worker = decoder->start(route.decoder_id, queue, route.result_endpoint,
                            stop, FftPlannerMutex());
  }

  ~SmState() {
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

  SmLoadedSlotDecoder module;
  const std::atomic_bool& cancelled;
  std::atomic_bool stop{false};
  DecoderInput queue{1};
  std::future<void> worker;
};

SmSlotDecoderSession::SmSlotDecoderSession(const std::string& mode,
                                           const SmDecoderRoute& route,
                                           int sample_rate,
                                           SmSampleType sample_type)
    : state_(std::make_unique<SmState>(ParseMode(mode), route, sample_rate,
                                       sample_type)) {}

SmSlotDecoderSession::~SmSlotDecoderSession() = default;

bool SmSlotDecoderSession::Submit(SmDecoderSlot slot, bool wait) {
  std::optional<SmDecoderSlot> item(std::move(slot));
  state_->CheckRunning();
  if (!wait) return state_->queue.try_enqueue(std::move(item));
  while (!state_->queue.wait_enqueue_timed(std::move(item), kWait))
    state_->CheckRunning();
  return true;
}

void SmSlotDecoderSession::Finish() {
  while (!state_->queue.wait_enqueue_timed(std::nullopt, kWait))
    state_->CheckRunning();
  state_->worker.get();
}

void CollectDecoderResults(
    const std::function<void(const std::string&, const std::atomic_bool&)>&
        produce,
    const std::function<void(SmDecoderResult)>& collect) {
  using namespace decoder_wire;
  SmResultSocket socket(nng_rep0_open);
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

}  // namespace skim
