#include <stdexcept>

#include "receiver/plugin_api.h"

namespace {
class SmExternalReceiver final : public skim::SmReceiverPlugin {
 public:
  const skim::SmReceiverPluginMetadata& metadata() const noexcept override {
    static constexpr skim::SmReceiverPluginMetadata metadata{"fixture"};
    return metadata;
  }

  void Probe(const std::string&,
             const skim::SmReceiverCandidate& found) const override {
    found("loop", "External IQ fixture", 12000);
  }

  void Capture(const skim::SmReceiverCaptureRequest& request,
               const skim::Samples& samples,
               const std::function<void()>&) const override {
    if (request.device != "loop" || request.rate != 12000)
      throw std::runtime_error("expected resolved fixture selector and rate");
    const std::complex<float> block[] = {{1.0f, 0.0f}};
    samples(block, 1);
  }
};
}  // namespace

extern "C" SKIM_PLUGIN_EXPORT const skim::SmReceiverPlugin*
skim_receiver_v2() noexcept {
  static const SmExternalReceiver receiver;
  return &receiver;
}

extern "C" SKIM_PLUGIN_EXPORT const skim::SmReceiverPluginDescriptor*
skim_receiver_descriptor_v2() noexcept {
  static constexpr skim::SmReceiverPluginDescriptor descriptor{
      "fixture:", "External IQ fixture", "", false, false, false};
  return &descriptor;
}
