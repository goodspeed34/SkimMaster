#pragma once

// Shared state and helpers for the control panel route modules.

#include <httplib.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <regex>
#include <string>
#include <vector>

#include "app/config.h"
#include "app/instance.h"

struct SmReferencePage;  // Generated in web/panel_assets.h.

namespace skim {
namespace web {
using Json = nlohmann::json;

constexpr int64_t kMaxFrequency = 1000000000000LL;
constexpr int64_t kMaxIndex = 1024 * 1024;

// SQL for the RF reference of slice s on worker w, given json_each over the
// worker plan's slices: the slice center for CW and RTTY, else the dial.
extern const char kSliceFrequencySql[];
// SQL formatting a worker row's CPU and memory, or an em dash.
extern const char kWorkerUsageSql[];

std::string Escape(const char* value);
std::optional<int64_t> FormInteger(const httplib::Request& req, const char* key,
                                   int64_t min, int64_t max,
                                   bool optional = false);
std::optional<double> FormDecimal(const httplib::Request& req, const char* key);
Json DriverOptions();
std::string DisplayField(const Json& row, const char* key,
                         const char* fallback = "");
std::optional<SmSliceSettings> FormSlice(const httplib::Request& req);
std::string EditReceiverUrl(const std::string& receiver);
std::string UtcMinute(int minute);
int FormTime(const httplib::Request& req, const char* key);
// Runs a read-only query. With html set, every cell is HTML-escaped. The
// connection provides band(hz), which names the amateur band holding hz.
Json Query(const char* database, const char* sql,
           const std::vector<std::string>& parameters = {}, bool html = true);
std::string CsvDocument(const Json& columns, const Json& rows);
int64_t DaysFromCivil(int year, unsigned month, unsigned day);
std::optional<int64_t> GalleryDateStart(const std::string& value,
                                        const char* field);
std::string GalleryUtc(int64_t timestamp_us);
const SmReferencePage* FindPage(const std::string& slug);
std::string PanelSlug(const std::string& path);

struct SmSettingsSnapshot {
  std::optional<SmInstanceSettings> settings;
  std::shared_ptr<const SmInstance> instance;
  std::string error;
};

// Keeps the last valid station settings so read-only pages can still render
// while the TOML file is being repaired.
class SmSettingsCache {
 public:
  explicit SmSettingsCache(std::filesystem::path path)
      : path_(std::move(path)) {
    Read();
  }

  SmSettingsSnapshot Read();

 private:
  std::filesystem::path path_;
  std::mutex mutex_;
  std::optional<SmInstanceSettings> settings_;
  std::shared_ptr<const SmInstance> instance_;
  std::string error_;
};

// Everything a route needs. It lives for the whole HTTP listener.
struct SmWebContext {
  SmWebContext(const char* config_path, const char* database_path,
               const char* gallery_path, std::string session_token)
      : config(config_path),
        database(database_path),
        gallery(gallery_path),
        token(std::move(session_token)),
        settings_cache(config_path) {}

  // Rejects a form whose CSRF token does not match this listener.
  bool ValidateForm(const httplib::Request& req, httplib::Response& res) const;
  // Renders a panel page with the shared header, navigation and help.
  void Render(const SmReferencePage& page, Json model,
              httplib::Response& res) const;

  const char* config;
  const char* database;
  const char* gallery;
  const std::string token;
  std::mutex config_writes;
  std::mutex probe_mutex;
  const std::chrono::steady_clock::time_point panel_start =
      std::chrono::steady_clock::now();
  SmSettingsCache settings_cache;
  const std::string image_pattern =
      R"(([0-9]+-(?:Martin[12]|Scottie[12]|Robot36|Robot72)\.png))";
  const std::regex image_filename{image_pattern};
};

void RenderServices(SmWebContext& ctx, const std::string& slug,
                    const httplib::Request& req, httplib::Response& res,
                    const std::string& error = "");
void RenderWizard(SmWebContext& ctx, const httplib::Request& req,
                  httplib::Response& res, const std::string& error = "");
void RenderReceiverEditor(SmWebContext& ctx, const httplib::Request& req,
                          httplib::Response& res,
                          const std::string& error = "");
void RenderSchedules(SmWebContext& ctx, const httplib::Request& req,
                     httplib::Response& res, const std::string& error = "");
void RenderGallery(SmWebContext& ctx, const SmReferencePage& page,
                   const httplib::Request& req, httplib::Response& res,
                   Json model);

void RegisterReceiverRoutes(httplib::Server& server, SmWebContext& ctx);
void RegisterScheduleRoutes(httplib::Server& server, SmWebContext& ctx);
void RegisterServiceRoutes(httplib::Server& server, SmWebContext& ctx);
void RegisterAdminRoutes(httplib::Server& server, SmWebContext& ctx);
void RegisterGalleryRoutes(httplib::Server& server, SmWebContext& ctx);
void RegisterPageRoutes(httplib::Server& server, SmWebContext& ctx);
}  // namespace web
}  // namespace skim
