#pragma once

#include <nng/nng.h>
#include <nng/protocol/reqrep0/req.h>

#include <chrono>
#include <stdexcept>
#include <utility>

#include "core/plugin_export.h"
#include "decoder/api.h"
#include "decoder/result_wire.h"

namespace skim {
class SmBuiltinSlotDecoder final : public SmSlotDecoderPlugin {
 public:
  using DecodeSamples = std::vector<SmSpot> (*)(SmSlotMode,
                                                const std::vector<float>&,
                                                std::mutex&);

  SmBuiltinSlotDecoder(SmSlotDecoderMetadata metadata, DecodeSamples decode)
      : metadata_(std::move(metadata)), decode_(decode) {}

  const SmSlotDecoderMetadata& metadata() const noexcept override {
    return metadata_;
  }

  std::future<void> start(std::string decoder_id, DecoderInput& input,
                          std::string endpoint, const std::atomic_bool& stop,
                          std::mutex& fft_planner) const override {
    return std::async(std::launch::async, [this, id = std::move(decoder_id),
                                           endpoint = std::move(endpoint),
                                           &input, &stop, &fft_planner] {
      using namespace decoder_wire;
      SmResultSocket output(nng_req0_open);
      Check(nng_socket_set_ms(output.socket, NNG_OPT_SENDTIMEO, 5000));
      Check(nng_socket_set_ms(output.socket, NNG_OPT_RECVTIMEO, 5000));
      // A successful reply confirms that the collector consumed this slot.
      Check(nng_socket_set_ms(output.socket, NNG_OPT_REQ_RESENDTIME, 60000));
      Check(nng_dial(output.socket, endpoint.c_str(), nullptr, 0));
      while (!stop.load()) {
        std::optional<SmDecoderSlot> slot;
        if (!input.wait_dequeue_timed(slot, std::chrono::milliseconds(25)))
          continue;
        if (!slot || stop.load()) return;
        const auto& audio = std::get<std::vector<float>>(slot->samples);
        auto spots = decode_(metadata_.supported_mode, audio, fft_planner);
        Send(output.socket, Encode({id, slot->utc_ms, std::move(spots)}));
        auto reply = Receive(output.socket);
        if (nng_msg_len(reply.get()) != 0)
          throw std::runtime_error("slot result rejected by collector");
      }
    });
  }

 private:
  const SmSlotDecoderMetadata metadata_;
  const DecodeSamples decode_;
};
}  // namespace skim
