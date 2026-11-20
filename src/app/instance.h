#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace skim {
class SmInstance;
struct SmInstanceSettings;
class SmReceiver;
struct SmReceiverDriver;
class SmSlice;
class SmSlotDecoder;
class SmSlotDecoderSession;
struct SmDecoderRoute;
enum class SmSampleType;

enum class SmMode { Cw, Rtty, Sstv, Ft4, Ft8, Wspr };
SmMode ParseSmMode(std::string_view name);
const char* ModeName(SmMode mode);

class SmDecoder {
 public:
  SmDecoder(const SmDecoder&) = delete;
  virtual ~SmDecoder() = default;

  const std::string& id() const { return id_; }
  const SmSlice& slice() const { return slice_; }
  SmMode mode() const { return mode_; }
  virtual const SmSlotDecoder* slot_decoder() const { return nullptr; }

 protected:
  friend class SmSlice;
  SmDecoder(const SmSlice& slice, std::string id, SmMode mode);

 private:
  const SmSlice& slice_;
  std::string id_;
  SmMode mode_;
};

class SmSlotDecoder final : public SmDecoder {
 public:
  const SmSlotDecoder* slot_decoder() const override { return this; }

  int input_sample_rate() const;
  SmSampleType input_sample_type() const;

  // The receiver worker owns the live session for the duration of a pass.
  std::unique_ptr<SmSlotDecoderSession> Start(
      const SmDecoderRoute& route) const;

 private:
  friend class SmSlice;
  SmSlotDecoder(const SmSlice& slice, std::string id, SmMode mode);
};

enum class SmSliceGroup { Manual, Attached, Scheduled };

class SmSlice {
 public:
  SmSlice(const SmSlice&) = delete;

  const std::string& id() const { return id_; }
  const SmReceiver& receiver() const { return receiver_; }
  const SmDecoder& decoder() const { return *decoder_; }
  SmMode mode() const { return decoder().mode(); }
  int output_rate() const { return mode() == SmMode::Cw ? 48000 : 12000; }
  SmSliceGroup group() const { return group_; }
  size_t index() const { return index_; }
  size_t schedule_index() const { return schedule_index_; }
  std::optional<int64_t> configured_reference() const { return reference_; }
  int64_t reference_hz(int64_t receiver_center) const;
  int64_t capture_hz(int64_t receiver_center) const;
  bool fits(int64_t receiver_center) const;
  nlohmann::json WorkerPlan() const;

 private:
  friend class SmInstance;
  SmSlice(const SmReceiver& receiver, const nlohmann::json& settings,
          std::string fallback, SmSliceGroup group, size_t index,
          size_t schedule_index);

  const SmReceiver& receiver_;
  std::string id_;
  SmSliceGroup group_;
  size_t index_;
  size_t schedule_index_;
  std::optional<int64_t> reference_;
  nlohmann::json settings_;
  std::unique_ptr<SmDecoder> decoder_;
};

class SmReceiver {
 public:
  SmReceiver(const SmReceiver&) = delete;

  const std::string& id() const { return id_; }
  const std::string& device() const { return device_; }
  // Stdin is a built-in source and has no plugin driver.
  const SmReceiverDriver* driver() const;
  int rate() const { return rate_; }
  bool enabled() const { return enabled_; }
  std::optional<int64_t> configured_center() const { return center_; }
  const SmInstance& instance() const { return instance_; }
  const std::vector<const SmSlice*>& slices() const { return slices_; }
  std::shared_ptr<const SmSlice> find_slice(std::string_view id) const;
  const nlohmann::json& settings() const { return settings_; }

  // Used by receiver workers with the tuning fixed by their worker plan.
  void RunWideband() const;

 private:
  friend class SmInstance;
  SmReceiver(const SmInstance& instance, const nlohmann::json& settings);

  const SmInstance& instance_;
  std::string id_;
  std::string device_;
  int rate_;
  bool enabled_;
  std::optional<int64_t> center_;
  nlohmann::json settings_;
  std::vector<std::unique_ptr<SmSlice>> owned_slices_;
  std::vector<const SmSlice*> slices_;
};

class SmService {
 public:
  SmService(const SmService&) = delete;

  const std::string& id() const { return id_; }
  const SmInstance& instance() const { return instance_; }
  bool enabled() const { return options_.value("enabled", false); }
  const nlohmann::json& options() const { return options_; }

 private:
  friend class SmInstance;
  SmService(const SmInstance& instance, std::string id, nlohmann::json options);

  const SmInstance& instance_;
  std::string id_;
  nlohmann::json options_;
};

// Each instance is an immutable graph in one process. Finds retain the graph;
// enumeration and parent access borrow from it while a handle is held.
class SmInstance : public std::enable_shared_from_this<SmInstance> {
 public:
  SmInstance(const SmInstance&) = delete;

  static std::shared_ptr<const SmInstance> FromSettings(
      const SmInstanceSettings& settings);
  static std::shared_ptr<const SmInstance> FromReceiverPlan(
      const nlohmann::json& plan);
  static void Run(const std::filesystem::path& config);

  const std::vector<const SmReceiver*>& receivers() const { return receivers_; }
  const std::vector<const SmService*>& services() const { return services_; }
  std::shared_ptr<const SmReceiver> find_receiver(std::string_view id) const;
  std::shared_ptr<const SmSlice> find_slice(std::string_view id) const;
  std::shared_ptr<const SmDecoder> find_decoder(std::string_view id) const;
  std::shared_ptr<const SmService> find_service(std::string_view id) const;
  // Resolves the allocation for a UTC minute of the day and weekday (Monday
  // is 0). Returns one plan per active receiver and enabled service, keyed by
  // worker ID.
  nlohmann::json WorkerPlans(const char* database, const char* gallery,
                             const std::filesystem::path& config_dir,
                             int minute, int weekday,
                             const nlohmann::json& overrides) const;

 private:
  SmInstance() = default;
  SmReceiver& AddReceiver(const nlohmann::json& settings);
  void AddSlices(SmReceiver& receiver, const nlohmann::json& slices,
                 SmSliceGroup group, std::string fallback_prefix,
                 size_t schedule_index = 0);

  std::map<std::string, std::unique_ptr<SmReceiver>, std::less<>> receiver_ids_;
  std::map<std::string, const SmSlice*, std::less<>> slice_ids_;
  std::map<std::string, const SmDecoder*, std::less<>> decoder_ids_;
  std::map<std::string, std::unique_ptr<SmService>, std::less<>> service_ids_;
  std::vector<const SmReceiver*> receivers_;
  std::vector<const SmService*> services_;
};
}  // namespace skim
