#include <nng/nng.h>
#include <nng/protocol/reqrep0/req.h>

#include <chrono>

#include "decoder/engines.h"
#include "decoder/result_wire.h"

namespace skim {
namespace {
class Decoder final : public SlotDecoder {
 public:
  explicit Decoder(SlotDecoderMetadata metadata)
      : metadata_(std::move(metadata)) {}

  const SlotDecoderMetadata& metadata() const noexcept override {
    return metadata_;
  }

  std::future<void> start(std::string decoder_id, DecoderInput& input,
                          std::string endpoint, const std::atomic_bool& stop,
                          std::mutex& fft_planner) const override {
    return std::async(std::launch::async, [this, id = std::move(decoder_id),
                                           endpoint = std::move(endpoint),
                                           &input, &stop, &fft_planner] {
      using namespace decoder_wire;
      Socket output(nng_req0_open);
      Check(nng_socket_set_ms(output.socket, NNG_OPT_SENDTIMEO, 5000));
      Check(nng_socket_set_ms(output.socket, NNG_OPT_RECVTIMEO, 5000));
      // A successful reply confirms that the collector consumed this slot.
      Check(nng_socket_set_ms(output.socket, NNG_OPT_REQ_RESENDTIME, 60000));
      Check(nng_dial(output.socket, endpoint.c_str(), nullptr, 0));
      while (!stop.load()) {
        std::optional<DecoderSlot> slot;
        if (!input.wait_dequeue_timed(slot, std::chrono::milliseconds(25)))
          continue;
        if (!slot || stop.load()) return;
        auto& audio = std::get<std::vector<float>>(slot->samples);
#ifdef SKIM_WSPR
        std::vector<Spot> spots;
        {
          std::lock_guard<std::mutex> lock(fft_planner);
          spots = DecodeWspr(audio);
        }
#else
        (void)fft_planner;
        auto mode = metadata_.supported_mode == SlotMode::Ft4 ? "FT4" : "FT8";
        auto spots = DecodeFtx(mode, audio);
#endif
        Send(output.socket, Encode({id, slot->utc_ms, std::move(spots)}));
        auto reply = Receive(output.socket);
        if (nng_msg_len(reply.get()) != 0)
          throw std::runtime_error("slot result rejected by collector");
      }
    });
  }

 private:
  const SlotDecoderMetadata metadata_;
};
}  // namespace
}  // namespace skim

#ifdef _WIN32
#define SKIM_EXPORT __declspec(dllexport)
#else
#define SKIM_EXPORT __attribute__((visibility("default")))
#endif

extern "C" SKIM_EXPORT const skim::SlotDecoder* skim_slot_decoder_v2(
    skim::SlotMode mode) {
  using namespace skim;
#ifdef SKIM_WSPR
  static const Decoder wspr(
      {"WSPR", SKIM_VERSION, "Guenael Jouchet (rtlsdr-wsprd)",
       "https://github.com/Guenael/rtlsdr-wsprd", SlotMode::Wspr, 12000, 12000,
       SampleType::MonoAudio});
  return mode == SlotMode::Wspr ? &wspr : nullptr;
#else
  static const Decoder ft4({"FT4", SKIM_VERSION, "Karlis Goba (ft8_lib)",
                            "https://github.com/kgoba/ft8_lib", SlotMode::Ft4,
                            12000, 12000, SampleType::MonoAudio});
  static const Decoder ft8({"FT8", SKIM_VERSION, "Karlis Goba (ft8_lib)",
                            "https://github.com/kgoba/ft8_lib", SlotMode::Ft8,
                            12000, 12000, SampleType::MonoAudio});
  if (mode == SlotMode::Ft4) return &ft4;
  if (mode == SlotMode::Ft8) return &ft8;
  return nullptr;
#endif
}
