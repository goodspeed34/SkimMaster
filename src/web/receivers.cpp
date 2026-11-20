#include <httplib.h>
#include <sqlite3.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <inja/inja.hpp>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <regex>
#include <stdexcept>

#include "app/slice_tuning.h"
#include "app/station.h"
#include "receiver/receiver.h"
#include "web/common.h"
#include "web/panel_assets.h"

namespace skim {
namespace web {
void RenderReceiverEditor(SmWebContext& ctx, const httplib::Request& req,
                          httplib::Response& res, const std::string& error) {
  res.set_header("Cache-Control", "no-store");
  auto settings = LoadSettings(ctx.config);
  auto receivers = Json::array();
  auto fields = Json{{"enabled", true}, {"repeat", true}, {"rate", "192000"}};
  for (auto key : {"receiver", "device", "center_hz", "gain_db", "ppm",
                   "utc_ms", "mode", "dial_hz", "slice_center"})
    fields[key] = "";
  std::string action, original;
  if (req.method == "POST") {
    action = req.get_param_value("action");
    original = req.get_param_value("original");
  } else if (req.has_param("delete")) {
    original = req.get_param_value("delete");
    action = "delete";
  } else if (req.has_param("edit") || req.has_param("add")) {
    original = req.get_param_value("edit");
    action = "save";
  }
  if (req.method == "GET" && req.has_param("slice_index"))
    action = "slice-save";
  const auto group = req.get_param_value("slice_group");
  const auto slice_index = req.get_param_value("slice_index");
  const auto schedule_index = req.get_param_value("schedule_index");
  auto attached = Json::array();
  auto slice_fields = Json{{"mode", ""}, {"slice_center", ""}, {"dial_hz", ""}};
  bool found = original.empty() && action != "delete";
  std::string probe_driver;
  const auto instance = SmInstance::FromSettings(settings);
  for (const auto* source : instance->receivers()) {
    const auto& receiver = source->settings();
    const auto& name = source->id();
    const auto& device = source->device();
    const auto& driver = *source->driver();
    receivers.push_back({{"name", Escape(name.c_str())},
                         {"device", Escape(device.c_str())},
                         {"driver", driver.label},
                         {"enabled", source->enabled() ? "Yes" : "No"},
                         {"rate", source->rate()}});
    if (name != original) continue;
    std::optional<int64_t> tuned;
    for (const auto& row : Query(ctx.database,
                                 "SELECT id,json_extract(plan,'$.center_hz') "
                                 "FROM workers WHERE kind='receiver'"))
      if (row[0] == Escape(name.c_str()) && row[1] != "")
        tuned = std::stoll(row[1].get<std::string>());
    if (!tuned) tuned = source->configured_center();
    const auto schedules = receiver.value("schedules", Json::array());
    for (const auto* slice : source->slices()) {
      const char* key = "slices";
      std::string label = "Manual", parent;
      auto center = tuned;
      if (slice->group() == SmSliceGroup::Attached) {
        key = "attached_slices";
        label = "Attached";
      } else if (slice->group() == SmSliceGroup::Scheduled) {
        const auto index = slice->schedule_index();
        const auto& schedule = schedules.at(index);
        label = schedule.value("name", "");
        if (label.empty()) label = "Schedule " + std::to_string(index + 1);
        label = Escape(label.c_str());
        parent = std::to_string(index);
        center = schedule.at("center_hz").get<int64_t>();
      }
      const auto mode = ModeName(slice->mode());
      const auto frequency = slice->configured_reference();
      const bool skimmer = IsSkimmer(mode);
      const auto reference = frequency
                                 ? std::to_string(*frequency) +
                                       (skimmer ? " Hz center" : " Hz dial")
                                 : "Receiver center";
      const auto coverage = !center                ? "Awaiting tuning"
                            : slice->fits(*center) ? "In bandwidth"
                                                   : "Outside bandwidth";
      attached.push_back({{"mode", mode},
                          {"reference", reference},
                          {"coverage", coverage},
                          {"group", key},
                          {"label", label},
                          {"index", slice->index()},
                          {"schedule", parent}});
      if (group == key && schedule_index == parent &&
          slice_index == std::to_string(slice->index())) {
        slice_fields["mode"] = mode;
        if (frequency)
          slice_fields[skimmer ? "slice_center" : "dial_hz"] =
              std::to_string(*frequency);
      }
    }
    found = true;
    probe_driver = driver.id;
    for (auto key : {"receiver", "device", "rate", "center_hz", "gain_db",
                     "ppm", "utc_ms"})
      fields[key] = DisplayField(receiver, key);
    fields["enabled"] = receiver.value("enabled", true);
    fields["repeat"] = receiver.value("repeat", false);
  }
  if (!found && error.empty()) {
    res.status = 404;
    action.clear();
  }
  bool slice_missing =
      action == "slice-save" && slice_fields.at("mode") == "" && error.empty();
  if (slice_missing) {
    res.status = 404;
    action.clear();
  }
  if (req.method == "POST" && action == "save") {
    for (auto key : {"receiver", "device", "rate", "center_hz", "gain_db",
                     "ppm", "utc_ms", "mode", "dial_hz", "slice_center"})
      fields[key] = Escape(req.get_param_value(key).c_str());
    fields["enabled"] = req.get_param_value("enabled") == "on";
    fields["repeat"] = req.get_param_value("repeat") == "on";
  }
  if (req.method == "POST" && action == "slice-save")
    for (auto key : {"mode", "slice_center", "dial_hz"})
      slice_fields[key] = Escape(req.get_param_value(key).c_str());
  Json model = {
      {"admin", true},
      {"empty", false},
      {"csrf", ctx.token},
      {"revision", req.method == "POST"
                       ? Escape(req.get_param_value("revision").c_str())
                       : settings.revision},
      {"attached", std::move(attached)},
      {"receiver_list", std::move(receivers)},
      {"receiver_action", action},
      {"receiver_error", slice_missing             ? "Slice not found."
                         : found || !error.empty() ? Escape(error.c_str())
                                                   : "Receiver not found."},
      {"original", Escape(original.c_str())},
      {"fields", std::move(fields)},
      {"slice_fields", std::move(slice_fields)},
      {"slice_group", Escape(group.c_str())},
      {"slice_index", Escape(slice_index.c_str())},
      {"schedule_index", Escape(schedule_index.c_str())},
      {"receiver_modes", {"CW", "RTTY", "FT8", "FT4", "WSPR", "SSTV"}},
      {"drivers", DriverOptions()},
      {"probe_driver", probe_driver},
  };
  ctx.Render(*FindPage("receiver-edit"), model, res);
}

void RegisterReceiverRoutes(httplib::Server& server, SmWebContext& ctx) {
  server.Get("/receiver-edit", [&](const auto& req, auto& res) {
    RenderReceiverEditor(ctx, req, res);
  });
  server.Post("/receiver-edit", [&](const auto& req, auto& res) {
    if (!ctx.ValidateForm(req, res)) return;
    try {
      std::optional<SmReceiverSettings> receiver;
      const auto action = req.get_param_value("action");
      if (action == "slice-save" || action == "slice-delete") {
        auto slice = action == "slice-save" ? FormSlice(req) : std::nullopt;
        if (action == "slice-save" && !slice)
          throw std::runtime_error("select a decoder mode");
        auto index = *FormInteger(req, "slice_index", 0, kMaxIndex);
        auto schedule = FormInteger(req, "schedule_index", 0, kMaxIndex, true);
        auto original = req.get_param_value("original");
        std::lock_guard<std::mutex> lock(ctx.config_writes);
        UpdateSlice(ctx.config, req.get_param_value("revision"), original,
                    req.get_param_value("slice_group"), schedule, index, slice);
        res.set_redirect(EditReceiverUrl(original), 303);
        return;
      }
      if (action == "save") {
        auto& r = receiver.emplace();
        r.name = req.get_param_value("receiver");
        r.device = req.get_param_value("device");
        r.rate = *FormInteger(req, "rate", 12000, 6144000);
        r.enabled = req.get_param_value("enabled") == "on";
        r.center = FormInteger(req, "center_hz", 1, 1000000000000LL, true);
        r.gain = FormDecimal(req, "gain_db");
        r.ppm = FormDecimal(req, "ppm");
        r.repeat = req.get_param_value("repeat") == "on";
        r.utc = FormInteger(req, "utc_ms", 0, 100000000000000LL, true);
        r.slice_center =
            FormInteger(req, "slice_center", 1, 1000000000000LL, true);
        r.mode = req.get_param_value("mode");
        if (!r.mode.empty() && !IsSkimmer(r.mode))
          r.dial = FormInteger(req, "dial_hz", 1, 1000000000000LL, true);
      } else if (action != "delete") {
        throw std::runtime_error("invalid receiver action");
      }
      std::lock_guard<std::mutex> lock(ctx.config_writes);
      UpdateReceiver(ctx.config, req.get_param_value("revision"),
                     req.get_param_value("original"), receiver);
      res.set_redirect("/receiver-edit", 303);
    } catch (const std::exception& error) {
      res.status = 400;
      RenderReceiverEditor(ctx, req, res, error.what());
    }
  });
  server.Post("/receiver-test", [&](const auto& req, auto& res) {
    if (!ctx.ValidateForm(req, res)) return;
    std::unique_lock<std::mutex> lock(ctx.probe_mutex, std::try_to_lock);
    if (!lock) {
      res.status = 409;
      res.set_content("A receiver scan or test is already running.",
                      "text/plain");
      return;
    }
    try {
      auto device = req.get_param_value("device");
      if (device.empty() || device == "-")
        throw std::runtime_error("Enter a device parameter.");
      auto center = *FormInteger(req, "center_hz", 1, kMaxFrequency);
      auto rate = *FormInteger(req, "rate", 12000, 6144000);
      auto gain = FormDecimal(req, "gain_db");
      auto ppm = FormDecimal(req, "ppm");
      if ((gain && std::abs(*gain) > 200) || (ppm && std::abs(*ppm) > 1000))
        throw std::runtime_error("Invalid gain or frequency correction.");
      if (IsFileReceiver(device)) {
        device = ResolveReceiverDevice(
            device, std::filesystem::path(ctx.config).parent_path());
      } else {
        for (const auto& row : Query(ctx.database,
                                     "SELECT json_extract(plan,'$.device') "
                                     "FROM workers WHERE kind='receiver'"))
          if (row[0] == Escape(device.c_str()))
            throw std::runtime_error(
                "Disable this receiver and wait for its allocation to stop "
                "before testing.");
      }
      Json settings{{"device", device}, {"center_hz", center}, {"rate", rate}};
      if (gain) settings["gain_db"] = *gain;
      if (ppm) settings["ppm"] = *ppm;
      TestReceiverCapture(settings);
      res.set_content("IQ samples received with these settings.", "text/plain");
    } catch (const std::exception& error) {
      res.status = 400;
      res.set_content(error.what(), "text/plain");
    }
  });
  server.Post("/receiver-probe", [&](const auto& req, auto& res) {
    if (!ctx.ValidateForm(req, res)) return;
    std::unique_lock<std::mutex> lock(ctx.probe_mutex, std::try_to_lock);
    if (!lock) {
      res.status = 409;
      res.set_content("A receiver scan is already running. Try again shortly.",
                      "text/plain");
      return;
    }
    try {
      auto devices = ProbeReceiver(
          req.get_param_value("driver"), req.get_param_value("target"),
          std::filesystem::path(ctx.config).parent_path());
      auto rows = Json::array();
      for (auto& device : devices)
        rows.push_back({{"device", device.device},
                        {"label", device.label},
                        {"rate", device.rate}});
      res.set_content(rows.dump(-1, ' ', false, Json::error_handler_t::replace),
                      "application/json");
    } catch (const std::exception& error) {
      res.status = 400;
      res.set_content(error.what(), "text/plain");
    }
  });
}
}  // namespace web
}  // namespace skim
