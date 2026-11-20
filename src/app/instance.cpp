#include "app/instance.h"

#include <array>
#include <cstdlib>
#include <stdexcept>
#include <utility>

#include "app/config.h"
#include "app/slice_tuning.h"
#include "decoder/session.h"
#include "decoder/settings.h"
#include "receiver/receiver.h"

namespace skim {
namespace {
using Json = nlohmann::json;

constexpr std::array kModes{
    std::pair{SmMode::Cw, "CW"},     std::pair{SmMode::Rtty, "RTTY"},
    std::pair{SmMode::Sstv, "SSTV"}, std::pair{SmMode::Ft4, "FT4"},
    std::pair{SmMode::Ft8, "FT8"},   std::pair{SmMode::Wspr, "WSPR"},
};
}  // namespace

SmMode ParseSmMode(std::string_view name) {
  for (const auto& [mode, label] : kModes)
    if (name == label) return mode;
  throw std::runtime_error("unknown slice mode");
}

const char* ModeName(SmMode mode) {
  for (const auto& [value, label] : kModes)
    if (mode == value) return label;
  throw std::runtime_error("unknown slice mode");
}

SmDecoder::SmDecoder(const SmSlice& slice, std::string id, SmMode mode)
    : slice_(slice), id_(std::move(id)), mode_(mode) {}

SmSlotDecoder::SmSlotDecoder(const SmSlice& slice, std::string id, SmMode mode)
    : SmDecoder(slice, std::move(id), mode) {}

int SmSlotDecoder::input_sample_rate() const { return slice().output_rate(); }

SmSampleType SmSlotDecoder::input_sample_type() const {
  return SmSampleType::MonoAudio;
}

std::unique_ptr<SmSlotDecoderSession> SmSlotDecoder::Start(
    const SmDecoderRoute& route) const {
  return std::make_unique<SmSlotDecoderSession>(
      ModeName(mode()), SmDecoderRoute{id(), route.result_endpoint, route.stop},
      input_sample_rate(), input_sample_type(), options());
}

SmSlice::SmSlice(const SmReceiver& receiver, const Json& settings,
                 std::string fallback, SmSliceGroup group, size_t index,
                 size_t schedule_index)
    : receiver_(receiver),
      id_(settings.value("slice_id", fallback)),
      group_(group),
      index_(index),
      schedule_index_(schedule_index),
      settings_(settings) {
  const auto name = settings.at("mode").get<std::string>();
  const auto mode = ParseSmMode(name);
  if (!IsSkimmer(name) || settings.contains("center_hz") ||
      settings.contains("dial_hz"))
    reference_ = SliceReference(settings, name, 0);

  auto decoder_id = settings.value("decoder_id", std::move(fallback));
  if (IsTimedDecoder(name))
    decoder_.reset(new SmSlotDecoder(*this, std::move(decoder_id), mode));
  else
    decoder_.reset(new SmDecoder(*this, std::move(decoder_id), mode));
}

int64_t SmSlice::reference_hz(int64_t receiver_center) const {
  return reference_.value_or(receiver_center);
}

int64_t SmSlice::capture_hz(int64_t receiver_center) const {
  return reference_hz(receiver_center) + (decoder().slot_decoder() ? 1500 : 0);
}

bool SmSlice::fits(int64_t receiver_center) const {
  return 2 * std::abs(capture_hz(receiver_center) - receiver_center) +
             output_rate() <=
         receiver().rate();
}

const Json& SmSlice::decoder_settings() const {
  if (const auto found = settings_.find("settings"); found != settings_.end())
    return *found;
  return receiver().instance().decoder_settings(mode());
}

SmSettingValues SmDecoder::options() const {
  return ResolveDecoderSettings(ModeName(mode()), slice().decoder_settings());
}

const Json& SmInstance::decoder_settings(SmMode mode) const {
  static const Json empty = Json::object();
  const auto found = decoders_.find(ModeName(mode));
  return found != decoders_.end() ? *found : empty;
}

Json SmSlice::WorkerPlan() const {
  auto plan = settings_;
  if (const auto& decoders = decoder_settings(); !decoders.empty())
    plan["settings"] = decoders;
  plan["slice_id"] = id();
  plan["decoder_id"] = decoder().id();
  plan["receiver_id"] = receiver().id();
  return plan;
}

SmReceiver::SmReceiver(const SmInstance& instance, const Json& settings)
    : instance_(instance),
      id_(settings.at("receiver")),
      device_(settings.at("device")),
      rate_(settings.at("rate")),
      enabled_(settings.value("enabled", true)),
      settings_(settings) {
  if (settings.contains("center_hz"))
    center_ = settings.at("center_hz").get<int64_t>();
}

const SmReceiverDriver* SmReceiver::driver() const {
  return device() == "-" ? nullptr : &DriverFor(device());
}

std::shared_ptr<const SmSlice> SmReceiver::find_slice(
    std::string_view id) const {
  auto slice = instance().find_slice(id);
  return slice && &slice->receiver() == this ? slice : nullptr;
}

SmService::SmService(const SmInstance& instance, std::string id, Json options)
    : instance_(instance), id_(std::move(id)), options_(std::move(options)) {}

SmReceiver& SmInstance::AddReceiver(const Json& settings) {
  auto receiver = std::unique_ptr<SmReceiver>(new SmReceiver(*this, settings));
  auto* value = receiver.get();
  if (value->id().empty() ||
      !receiver_ids_.emplace(value->id(), std::move(receiver)).second)
    throw std::runtime_error("empty or duplicate receiver ID");
  receivers_.push_back(value);
  return *value;
}

void SmInstance::AddSlices(SmReceiver& receiver, const Json& slices,
                           SmSliceGroup group, std::string fallback_prefix,
                           size_t schedule_index) {
  if (!slices.is_array())
    throw std::runtime_error("expected an array of decoder slices");
  for (size_t index = 0; index < slices.size(); ++index) {
    const auto& settings = slices[index];
    auto fallback = fallback_prefix + std::to_string(index);
    auto slice = std::unique_ptr<SmSlice>(new SmSlice(
        receiver, settings, std::move(fallback), group, index, schedule_index));
    auto* value = slice.get();
    if (value->id().empty() || !slice_ids_.emplace(value->id(), value).second)
      throw std::runtime_error("empty or duplicate slice_id");
    const auto& decoder = value->decoder();
    if (decoder.id().empty() ||
        !decoder_ids_.emplace(decoder.id(), &decoder).second)
      throw std::runtime_error("empty or duplicate decoder_id");
    receiver.owned_slices_.push_back(std::move(slice));
    receiver.slices_.push_back(value);
  }
}

std::shared_ptr<const SmInstance> SmInstance::FromSettings(
    const SmInstanceSettings& settings) {
  const auto& topology = settings.topology;
  auto graph = std::shared_ptr<SmInstance>(new SmInstance);
  graph->decoders_ = topology.value("decoders", Json::object());
  for (const auto& configured : topology.at("receivers")) {
    auto& receiver = graph->AddReceiver(configured);
    // Legacy configurations can be viewed before the supervisor persists IDs.
    auto prefix =
        std::string(1, '\0') + std::to_string(graph->receivers_.size()) + ':';
    graph->AddSlices(receiver, configured.value("slices", Json::array()),
                     SmSliceGroup::Manual, prefix + "manual:");
    graph->AddSlices(receiver,
                     configured.value("attached_slices", Json::array()),
                     SmSliceGroup::Attached, prefix + "attached:");
    const auto schedules = configured.value("schedules", Json::array());
    for (size_t index = 0; index < schedules.size(); ++index)
      graph->AddSlices(
          receiver, schedules[index].value("slices", Json::array()),
          SmSliceGroup::Scheduled, prefix + std::to_string(index) + ':', index);
  }
  const auto services = topology.value("services", Json::object());
  for (const auto& [name, options] : services.items()) {
    auto service =
        std::unique_ptr<SmService>(new SmService(*graph, name, options));
    graph->services_.push_back(service.get());
    graph->service_ids_.emplace(name, std::move(service));
  }
  return graph;
}

std::shared_ptr<const SmInstance> SmInstance::FromReceiverPlan(
    const Json& plan) {
  auto graph = std::shared_ptr<SmInstance>(new SmInstance);
  auto& receiver = graph->AddReceiver(plan);
  if (plan.value("receiver_id", receiver.id()) != receiver.id())
    throw std::runtime_error("invalid receiver identity in worker plan");
  const auto& slices = plan.at("slices");
  if (!slices.is_array())
    throw std::runtime_error("expected an array of decoder slices");
  for (const auto& slice : slices)
    if (slice.value("receiver_id", receiver.id()) != receiver.id())
      throw std::runtime_error("invalid slice receiver in worker plan");
  graph->AddSlices(receiver, slices, SmSliceGroup::Manual, "standalone-");
  return graph;
}

std::shared_ptr<const SmReceiver> SmInstance::find_receiver(
    std::string_view id) const {
  const auto found = receiver_ids_.find(id);
  return found == receiver_ids_.end()
             ? nullptr
             : std::shared_ptr<const SmReceiver>(shared_from_this(),
                                                 found->second.get());
}

std::shared_ptr<const SmSlice> SmInstance::find_slice(
    std::string_view id) const {
  const auto found = slice_ids_.find(id);
  return found == slice_ids_.end() ? nullptr
                                   : std::shared_ptr<const SmSlice>(
                                         shared_from_this(), found->second);
}

std::shared_ptr<const SmDecoder> SmInstance::find_decoder(
    std::string_view id) const {
  const auto found = decoder_ids_.find(id);
  return found == decoder_ids_.end() ? nullptr
                                     : std::shared_ptr<const SmDecoder>(
                                           shared_from_this(), found->second);
}

std::shared_ptr<const SmService> SmInstance::find_service(
    std::string_view id) const {
  const auto found = service_ids_.find(id);
  return found == service_ids_.end()
             ? nullptr
             : std::shared_ptr<const SmService>(shared_from_this(),
                                                found->second.get());
}
}  // namespace skim
