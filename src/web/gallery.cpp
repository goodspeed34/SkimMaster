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

#include "web/common.h"
#include "web/panel_assets.h"

namespace skim {
namespace web {
void RenderGallery(SmWebContext& ctx, const SmReferencePage& page,
                   const httplib::Request& req, httplib::Response& res,
                   Json model) {
  struct SmImageRecord {
    int64_t timestamp_us;
    std::string name;
    std::string mode;
    std::uintmax_t bytes;
    std::string captured;
  };
  const auto selected_mode = req.get_param_value("mode");
  const auto from_text = req.get_param_value("from");
  const auto to_text = req.get_param_value("to");
  const bool valid_mode =
      selected_mode.empty() || selected_mode == "Martin1" ||
      selected_mode == "Martin2" || selected_mode == "Scottie1" ||
      selected_mode == "Scottie2" || selected_mode == "Robot36" ||
      selected_mode == "Robot72";
  std::optional<int64_t> from_us, to_us;
  std::string gallery_error;
  if (!valid_mode) {
    res.status = 400;
    gallery_error = "Select a valid SSTV mode.";
  }
  try {
    from_us = GalleryDateStart(from_text, "start");
    to_us = GalleryDateStart(to_text, "end");
    if (to_us) *to_us += 86400LL * 1000000LL;
    if (from_us && to_us && *from_us >= *to_us)
      throw std::runtime_error("start date must not be after end date");
  } catch (const std::exception& error) {
    res.status = 400;
    gallery_error = error.what();
  }
  std::vector<SmImageRecord> records;
  for (const auto& entry : std::filesystem::directory_iterator(ctx.gallery)) {
    auto name = entry.path().filename().string();
    if (!gallery_error.empty() ||
        !std::filesystem::is_regular_file(entry.symlink_status()) ||
        !std::regex_match(name, ctx.image_filename))
      continue;
    const auto dash = name.find('-');
    int64_t timestamp_us = 0;
    const auto parsed =
        std::from_chars(name.data(), name.data() + dash, timestamp_us);
    if (parsed.ec != std::errc{} || parsed.ptr != name.data() + dash ||
        (from_us && timestamp_us < *from_us) ||
        (to_us && timestamp_us >= *to_us))
      continue;
    auto mode = name.substr(dash + 1, name.size() - dash - 5);
    if (!selected_mode.empty() && mode != selected_mode) continue;
    std::error_code size_error;
    const auto bytes = std::filesystem::file_size(entry.path(), size_error);
    if (size_error) continue;
    std::string captured;
    try {
      captured = GalleryUtc(timestamp_us);
    } catch (const std::runtime_error&) {
      continue;
    }
    records.push_back({timestamp_us, std::move(name), std::move(mode), bytes,
                       std::move(captured)});
  }
  std::sort(records.begin(), records.end(),
            [](const auto& left, const auto& right) {
              return left.timestamp_us != right.timestamp_us
                         ? left.timestamp_us > right.timestamp_us
                         : left.name > right.name;
            });
  auto gallery_rows = Json::array();
  auto csv_rows = Json::array();
  for (const auto& record : records) {
    csv_rows.push_back({record.name, record.mode, record.captured,
                        std::to_string(record.bytes)});
    if (gallery_rows.size() < 100)
      gallery_rows.push_back({{"name", record.name},
                              {"mode", record.mode},
                              {"captured", record.captured},
                              {"bytes", record.bytes}});
  }
  model["gallery_columns"] = {"Filename", "Mode", "Captured UTC", "Bytes"};
  model["gallery_rows"] = std::move(gallery_rows);
  model["gallery_empty"] = records.empty();
  model["gallery_mode"] = Escape(selected_mode.c_str());
  model["gallery_from"] = Escape(from_text.c_str());
  model["gallery_to"] = Escape(to_text.c_str());
  model["gallery_error"] = Escape(gallery_error.c_str());
  model["gallery_notice"] =
      req.get_param_value("deleted") == "1" ? "SSTV image deleted." : "";
  model["csrf"] = ctx.token;
  if (req.get_param_value("format") == "csv") {
    if (!gallery_error.empty()) {
      res.set_content(gallery_error, "text/plain; charset=utf-8");
      return;
    }
    res.set_header("Content-Disposition",
                   "attachment; filename=sstv-gallery.csv");
    res.set_content(CsvDocument(model.at("gallery_columns"), csv_rows),
                    "text/csv; charset=utf-8");
    return;
  }
  model["empty"] = false;
  ctx.Render(page, std::move(model), res);
}

void RegisterGalleryRoutes(httplib::Server& server, SmWebContext& ctx) {
  server.Get("/image/" + ctx.image_pattern, [&](const httplib::Request& req,
                                                httplib::Response& res) {
    auto path = std::filesystem::path(ctx.gallery) / req.matches[1].str();
    if (!std::filesystem::is_regular_file(
            std::filesystem::symlink_status(path))) {
      res.status = 404;
      return;
    }
    res.set_file_content(path.string(), "image/png");
  });
  server.Post("/gallery-delete", [&](const httplib::Request& req,
                                     httplib::Response& res) {
    if (!ctx.ValidateForm(req, res)) return;
    try {
      const auto name = req.get_param_value("filename");
      if (!std::regex_match(name, ctx.image_filename))
        throw std::runtime_error("select a valid SSTV image");
      const auto path = std::filesystem::path(ctx.gallery) / name;
      std::error_code status_error;
      const auto status = std::filesystem::symlink_status(path, status_error);
      if (status_error || status.type() != std::filesystem::file_type::regular)
        throw std::runtime_error(
            "SSTV image is missing or is not a regular file");
      std::error_code remove_error;
      if (!std::filesystem::remove(path, remove_error) || remove_error)
        throw std::runtime_error("could not delete SSTV image");

      std::string target = "/sstv-gallery?deleted=1";
      auto preserve = [&](const char* key) {
        const auto value = req.get_param_value(key);
        if (!value.empty())
          target += std::string("&") + key + "=" +
                    httplib::detail::encode_query_param(value);
      };
      preserve("mode");
      preserve("from");
      preserve("to");
      res.set_redirect(target, 303);
    } catch (const std::exception& error) {
      res.status = 400;
      res.set_content(error.what(), "text/plain; charset=utf-8");
    }
  });
}
}  // namespace web
}  // namespace skim
