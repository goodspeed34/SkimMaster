// Decoder settings forms on the CW, RTTY and digital decoder pages.

#include <cmath>
#include <cstdio>
#include <mutex>
#include <stdexcept>
#include <string>

#include "decoder/settings.h"
#include "web/common.h"

namespace skim {
namespace web {
namespace {
std::string NumberText(double value) {
  char text[32];
  if (value == std::floor(value) && std::abs(value) < 1e15)
    std::snprintf(text, sizeof(text), "%lld", static_cast<long long>(value));
  else
    std::snprintf(text, sizeof(text), "%.10g", value);
  return text;
}

const char* KindName(SmSettingKind kind) {
  switch (kind) {
    case SmSettingKind::Integer:
      return "integer";
    case SmSettingKind::Decimal:
      return "decimal";
    case SmSettingKind::Boolean:
      return "boolean";
    case SmSettingKind::Choice:
      return "choice";
  }
  return "integer";
}

std::vector<std::string> PageModes(const std::string& slug) {
  if (slug == "cw-skimmer") return {"CW"};
  if (slug == "rtty-skimmer") return {"RTTY"};
  if (slug == "digital-decoders") return {"FT8", "FT4", "WSPR"};
  return {};
}

Json SettingsForm(const std::string& mode, const Json& configured) {
  Json form{{"mode", mode}, {"fields", Json::array()}};
  const std::vector<SmSetting>* schema = nullptr;
  SmSettingValues values;
  try {
    schema = &DecoderSettings(mode);
    values = ResolveDecoderSettings(mode, configured);
  } catch (const std::exception& error) {
    form["unavailable"] = Escape(error.what());
    return form;
  }
  form["unavailable"] = "";
  for (const auto& setting : *schema) {
    const double value = values.at(setting.key);
    Json field{{"key", Escape(setting.key.c_str())},
               {"label", Escape(setting.label.c_str())},
               {"help", Escape(setting.help.c_str())},
               {"kind", KindName(setting.kind)},
               {"value", NumberText(value)},
               {"default", NumberText(setting.default_value)},
               {"min", NumberText(setting.min)},
               {"max", NumberText(setting.max)},
               {"checked", value != 0},
               {"choices", Json::array()}};
    for (double choice : setting.choices)
      field["choices"].push_back(
          {{"value", NumberText(choice)},
           {"selected", std::abs(choice - value) < 1e-9}});
    form["fields"].push_back(std::move(field));
  }
  return form;
}
}  // namespace

void AddDecoderSettings(const std::string& slug,
                        const SmInstanceSettings& settings, Json& model) {
  const auto decoders = settings.topology.value("decoders", Json::object());
  auto forms = Json::array();
  for (const auto& mode : PageModes(slug))
    forms.push_back(SettingsForm(mode, decoders.value(mode, Json(nullptr))));
  model["decoder_settings"] = std::move(forms);
  model["revision"] = settings.revision;
}

void RegisterDecoderRoutes(httplib::Server& server, SmWebContext& ctx) {
  server.Get("/data/live-text", [&](const httplib::Request& req,
                                    httplib::Response& res) {
    const auto mode = req.get_param_value("mode");
    if (mode != "CW" && mode != "RTTY") {
      res.status = 400;
      res.set_content("select CW or RTTY", "text/plain; charset=utf-8");
      return;
    }
    res.set_header("Cache-Control", "no-store");
    res.set_content(Query(ctx.database, kLiveTextSql,
                          {mode, req.get_param_value("receiver")}, false)
                        .dump(),
                    "application/json");
  });
  server.Post("/decoder-settings", [&](const httplib::Request& req,
                                       httplib::Response& res) {
    if (!ctx.ValidateForm(req, res)) return;
    const auto mode = req.get_param_value("mode");
    const std::string page = mode == "CW"     ? "cw-skimmer"
                             : mode == "RTTY" ? "rtty-skimmer"
                                              : "digital-decoders";
    try {
      if (PageModes(page).empty() ||
          (mode != "CW" && mode != "RTTY" && mode != "FT8" && mode != "FT4" &&
           mode != "WSPR"))
        throw std::runtime_error("select a decoder mode");
      SmSettingValues values;
      for (const auto& setting : DecoderSettings(mode)) {
        if (setting.kind == SmSettingKind::Boolean) {
          values[setting.key] = req.has_param(setting.key) ? 1 : 0;
          continue;
        }
        auto value = FormDecimal(req, setting.key.c_str());
        if (!value) throw std::runtime_error("enter " + setting.label);
        if (!SettingAccepts(setting, *value))
          throw std::runtime_error(setting.label + " is out of range");
        values[setting.key] = *value;
      }
      std::lock_guard<std::mutex> lock(ctx.config_writes);
      UpdateDecoderSettings(ctx.config, req.get_param_value("revision"), mode,
                            values);
      res.set_redirect("/" + page + "?saved=" + mode, 303);
    } catch (const std::exception& error) {
      res.status = 400;
      res.set_content(std::string("Decoder settings rejected: ") + error.what(),
                      "text/plain; charset=utf-8");
    }
  });
}
}  // namespace web
}  // namespace skim
