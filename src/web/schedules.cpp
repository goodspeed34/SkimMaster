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

#include "app/station.h"
#include "web/common.h"
#include "web/panel_assets.h"

namespace skim {
namespace web {
void RenderSchedules(SmWebContext& ctx, const httplib::Request& req,
                     httplib::Response& res, const std::string& error) {
  res.set_header("Cache-Control", "no-store");
  const auto settings = LoadSettings(ctx.config);
  const auto overrides = ScheduleOverrides(ctx.database);
  const auto selected = req.get_param_value("receiver");
  const auto index = req.get_param_value("index");
  const char* days[] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
  auto rows = Json::array();
  auto sources = Json::array();
  auto weekdays = Json::array();
  auto fields =
      Json{{"name", ""},      {"start", "00:00"}, {"end", "00:00"},
           {"priority", "1"}, {"center_hz", ""},  {"enabled", true},
           {"mode", ""},      {"dial_hz", ""},    {"slice_center", ""}};
  bool editing = false;
  int day_mask = 127;
  for (const auto& receiver : settings.topology.at("receivers")) {
    const auto name = receiver.at("receiver").get<std::string>();
    const auto escaped = Escape(name.c_str());
    sources.push_back(escaped);
    const auto schedules = receiver.value("schedules", Json::array());
    if (selected == name && index.empty()) {
      editing = true;
      fields["center_hz"] = DisplayField(receiver, "center_hz");
    }
    bool override_shown = false;
    for (size_t i = 0; i < schedules.size(); ++i) {
      const auto& schedule = schedules.at(i);
      const int mask = schedule.value("days", 127);
      std::string day_list;
      for (int day = 0; day < 7; ++day) {
        if (!(mask & (1 << day))) continue;
        if (!day_list.empty()) day_list += ", ";
        day_list += days[day];
      }
      auto name_label = DisplayField(schedule, "name");
      if (name_label.empty()) name_label = "Schedule " + std::to_string(i + 1);
      const auto start = UtcMinute(schedule.at("start_minute"));
      const auto end = UtcMinute(schedule.at("end_minute"));
      const bool enabled = schedule.value("enabled", true);
      const bool can_run = receiver.value("enabled", true);
      const bool overridden = can_run && !override_shown &&
                              overrides.contains(name) &&
                              overrides.at(name).at("schedule") == schedule;
      override_shown = override_shown || overridden;
      rows.push_back(
          {{"receiver", escaped},
           {"index", i},
           {"name", name_label},
           {"start", start},
           {"end", end},
           {"days", mask == 127 ? "Daily" : day_list},
           {"priority", schedule.at("priority")},
           {"center_hz", schedule.at("center_hz")},
           {"enabled", enabled},
           {"can_run", can_run},
           {"overridden", overridden},
           {"slice_count", schedule.value("slices", Json::array()).size()}});
      if (selected == name && index == std::to_string(i)) {
        editing = true;
        fields["name"] = DisplayField(schedule, "name");
        fields["start"] = start;
        fields["end"] = end;
        fields["center_hz"] = DisplayField(schedule, "center_hz");
        fields["priority"] = DisplayField(schedule, "priority");
        fields["enabled"] = enabled;
        day_mask = mask;
      }
    }
  }
  if (!selected.empty() && !editing && error.empty()) res.status = 404;
  const bool submitted =
      req.method == "POST" && req.get_param_value("action") == "save";
  if (submitted) {
    for (auto key : {"name", "start", "end", "center_hz", "priority", "mode",
                     "dial_hz", "slice_center"})
      fields[key] = Escape(req.get_param_value(key).c_str());
    fields["enabled"] = req.has_param("enabled");
  }
  for (int day = 0; day < 7; ++day) {
    const auto key = "day" + std::to_string(day);
    const bool checked = submitted ? req.has_param(key)
                                   : static_cast<bool>(day_mask & (1 << day));
    weekdays.push_back(
        {{"key", key}, {"label", days[day]}, {"checked", checked}});
  }
  auto revision = submitted ? Escape(req.get_param_value("revision").c_str())
                            : settings.revision;
  Json model = {
      {"admin", true},
      {"empty", false},
      {"csrf", ctx.token},
      {"revision", std::move(revision)},
      {"schedule_rows", std::move(rows)},
      {"sources", std::move(sources)},
      {"selected", Escape(selected.c_str())},
      {"manage_slices_url", Escape(EditReceiverUrl(selected).c_str())},
      {"schedule_index", Escape(index.c_str())},
      {"editing_schedule", editing},
      {"schedule_error",
       res.status == 404 ? "Schedule not found." : Escape(error.c_str())},
      {"schedule_fields", std::move(fields)},
      {"weekdays", std::move(weekdays)},
      {"receiver_modes", {"CW", "RTTY", "FT8", "FT4", "WSPR", "SSTV"}}};
  ctx.Render(*FindPage("band-schedules"), model, res);
}

void RegisterScheduleRoutes(httplib::Server& server, SmWebContext& ctx) {
  server.Get("/band-schedules", [&](const auto& req, auto& res) {
    RenderSchedules(ctx, req, res);
  });
  server.Post("/band-schedules", [&](const auto& req, auto& res) {
    if (!ctx.ValidateForm(req, res)) return;
    try {
      const auto action = req.get_param_value("action");
      const auto receiver = req.get_param_value("receiver");
      const auto revision = req.get_param_value("revision");
      const auto index = FormInteger(req, "index", 0, kMaxIndex, true);
      std::lock_guard<std::mutex> lock(ctx.config_writes);
      if (action == "run" || action == "clear") {
        const auto settings = LoadSettings(ctx.config);
        if (settings.revision != revision)
          throw std::runtime_error("configuration changed; reload and retry");
        if (action == "clear") {
          SetScheduleOverride(ctx.database, receiver, Json(), 0);
        } else {
          const auto& sources = settings.topology.at("receivers");
          const auto found = std::find_if(
              sources.begin(), sources.end(), [&](const Json& source) {
                return source.at("receiver") == receiver;
              });
          if (found == sources.end() || !found->value("enabled", true))
            throw std::runtime_error(
                "enable the receiver before running a schedule");
          auto schedules = found->value("schedules", Json::array());
          if (!index || static_cast<size_t>(*index) >= schedules.size())
            throw std::runtime_error("select a valid schedule");
          SetScheduleOverride(ctx.database, receiver, schedules.at(*index), 30);
        }
      } else {
        std::optional<SmScheduleSettings> schedule;
        if (action == "save") {
          int days = 0;
          for (int i = 0; i < 7; ++i)
            if (req.has_param("day" + std::to_string(i))) days |= 1 << i;
          auto name = req.get_param_value("name");
          if (name.size() > 128 ||
              name.find_first_of("\r\n") != std::string::npos)
            throw std::runtime_error("invalid schedule name");
          schedule = SmScheduleSettings{
              name,
              FormTime(req, "start"),
              FormTime(req, "end"),
              static_cast<int>(*FormInteger(req, "priority", INT_MIN, INT_MAX)),
              *FormInteger(req, "center_hz", 1, kMaxFrequency),
              days,
              req.has_param("enabled"),
              FormSlice(req)};
        } else if (action != "delete" && action != "duplicate") {
          throw std::runtime_error("invalid schedule action");
        }
        UpdateSchedule(ctx.config, revision, receiver, index,
                       action == "duplicate", schedule);
      }
      res.set_redirect("/band-schedules", 303);
    } catch (const std::exception& error) {
      res.status = 400;
      RenderSchedules(ctx, req, res, error.what());
    }
  });
}
}  // namespace web
}  // namespace skim
