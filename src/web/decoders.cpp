// Decoder settings forms on the CW, RTTY and digital decoder pages.

#include <charconv>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>

#include "app/archive.h"
#include "decoder/recording.h"
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
std::string PageFor(const std::string& mode) {
  return mode == "CW"     ? "cw-skimmer"
         : mode == "RTTY" ? "rtty-skimmer"
                          : "digital-decoders";
}

std::string Optional(const std::optional<double>& value, const char* format) {
  if (!value) return "";
  char text[32];
  std::snprintf(text, sizeof(text), format, *value);
  return text;
}

void Decode(SmWebContext& ctx, const httplib::Request& req,
            httplib::Response& res) {
  const auto field = [&](const char* key) {
    return req.has_file(key) ? req.get_file_value(key).content : std::string();
  };
  const auto mode = field("mode");
  const auto hz = field("reference_hz");
  const auto page = PageFor(mode);
  Json model{{"decode_mode", Escape(mode.c_str())},
             {"decode_hz", Escape(hz.c_str())},
             {"decode_error", ""},
             {"decode_rows", Json::array()},
             {"decode_channels", Json::array()}};
  try {
    std::unique_lock<std::mutex> lock(ctx.decode, std::try_to_lock);
    if (!lock)
      throw std::runtime_error(
          "Another recording is being decoded. Try again.");
    int64_t reference = 0;
    const auto parsed =
        std::from_chars(hz.data(), hz.data() + hz.size(), reference);
    if (hz.empty() || parsed.ec != std::errc() ||
        parsed.ptr != hz.data() + hz.size())
      throw std::runtime_error(
          mode == "CW" || mode == "RTTY"
              ? "Enter the recording's RF center in Hz."
              : "Enter the recording's USB dial frequency in Hz.");
    if (!req.has_file("recording") ||
        req.get_file_value("recording").content.empty())
      throw std::runtime_error("Choose a WAV recording to decode.");
    const auto settings = LoadSettings(ctx.config);
    const auto options = ResolveDecoderSettings(
        mode, settings.topology.value("decoders", Json::object())
                  .value(mode, Json()));
    const auto file = TemporaryFile("recording.wav");
    {
      std::ofstream out(*file, std::ios::binary | std::ios::trunc);
      const auto& content = req.get_file_value("recording").content;
      out.write(content.data(), static_cast<std::streamsize>(content.size()));
      if (!out) throw std::runtime_error("could not stage the recording");
    }
    auto result =
        DecodeRecording(mode, file->string().c_str(), reference, options);
    for (const auto& spot : result.spots)
      model["decode_rows"].push_back(
          {std::to_string(spot.frequency), Escape(spot.callsign.c_str()),
           Escape(spot.grid.c_str()), Optional(spot.snr, "%.0f dB"),
           Escape(spot.message.c_str())});
    for (const auto& channel : result.channels)
      model["decode_channels"].push_back(
          {std::to_string(channel.frequency),
           Optional(channel.speed, mode == "CW" ? "%.0f WPM" : "%g Bd"),
           Optional(channel.snr, "%.1f dB"), Escape(channel.text.c_str())});
  } catch (const std::exception& error) {
    res.status = 400;
    model["decode_error"] = Escape(error.what());
  }
  RenderDecoderPageWith(ctx, page, req, res, std::move(model));
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
  server.Post("/decode",
              [&](const httplib::Request& req, httplib::Response& res) {
                if (!req.is_multipart_form_data()) {
                  res.status = 400;
                  res.set_content("Upload the recording as a form file.",
                                  "text/plain; charset=utf-8");
                  return;
                }
                if (!ctx.ValidateForm(req, res)) return;
                Decode(ctx, req, res);
              });
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
