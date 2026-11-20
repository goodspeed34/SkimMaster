#include "app/instance.h"

#include <stdexcept>

namespace {
using Json = nlohmann::json;

void Check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
}  // namespace

int main() {
  using namespace skim;
  const Json timed = {{"slice_id", "timed"},
                      {"decoder_id", "ft8"},
                      {"mode", "FT8"},
                      {"dial_hz", 14074000}};
  const Json text = {
      {"slice_id", "text"}, {"decoder_id", "cw"}, {"mode", "CW"}};
  Json east = {{"receiver", "east"},
               {"device", "rtltcp:localhost:1234"},
               {"rate", 96000},
               {"center_hz", 14078000},
               {"slices", Json::array({timed})}};
  const Json west = {{"receiver", "west"},
                     {"device", "rtltcp:localhost:1235"},
                     {"rate", 96000},
                     {"enabled", false},
                     {"attached_slices", Json::array({text})}};
  Json topology = {{"receivers", Json::array({east, west})}};
  topology["services"]["cluster"]["enabled"] = false;
  auto original = SmInstance::FromTopology(topology);
  auto decoder = original->find_decoder("ft8");
  Check(decoder && decoder->slot_decoder(), "FT8 needs a slot decoder");
  Check(decoder->mode() == SmMode::Ft8, "decoder mode mismatch");
  const auto& slice = decoder->slice();
  const auto& receiver = slice.receiver();
  Check(receiver.id() == "east" && &receiver.instance() == original.get(),
        "decoder parent navigation failed");
  Check(original->find_receiver("east").get() == &receiver &&
            receiver.find_slice("timed").get() == &slice &&
            &slice.decoder() == decoder.get(),
        "root, receiver, and slice should agree on object identity");
  Check(!receiver.find_slice("text") && !original->find_decoder("absent"),
        "invalid lookup should return empty");
  auto cw = original->find_decoder("cw");
  Check(cw && !cw->slot_decoder() && !cw->slice().receiver().enabled() &&
            !cw->slice().configured_reference() &&
            cw->slice().reference_hz(7000000) == 7000000,
        "disabled receiver should retain its center-following streaming slice");
  auto service = original->find_service("cluster");
  Check(
      service && &service->instance() == original.get() && !service->enabled(),
      "service navigation failed");

  east["slices"] = Json::array({slice.WorkerPlan()});
  auto worker = SmInstance::FromReceiverPlan(east);
  const auto worker_decoder = worker->find_decoder("ft8");
  Check(worker_decoder &&
            worker_decoder->slice().receiver().id() == receiver.id() &&
            worker_decoder->slice().reference_hz(14078000) == 14074000,
        "worker plan should preserve links and RF reference");

  topology["receivers"][0]["slices"][0]["dial_hz"] = 14075000;
  const auto reloaded = SmInstance::FromTopology(topology);
  cw.reset();
  service.reset();
  original.reset();
  Check(decoder->slice().receiver().instance().find_decoder("ft8") == decoder &&
            decoder->slice().reference_hz(14078000) == 14074000 &&
            reloaded->find_decoder("ft8")->slice().reference_hz(14078000) ==
                14075000,
        "reload should preserve old handles and resolve stable IDs in the new "
        "graph");

  topology["receivers"][1]["attached_slices"][0]["decoder_id"] = "ft8";
  try {
    SmInstance::FromTopology(topology);
  } catch (const std::runtime_error& error) {
    Check(std::string_view(error.what()) == "empty or duplicate decoder_id",
          "unexpected graph validation failure");
    return 0;
  }
  throw std::runtime_error("accepted duplicate decoder ID");
}
