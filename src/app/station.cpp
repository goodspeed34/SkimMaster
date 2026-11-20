#include "app/station.h"

#include <algorithm>
#include <boost/asio/io_context.hpp>
#include <boost/dll/runtime_symbol_info.hpp>
#include <boost/interprocess/sync/file_lock.hpp>
#include <boost/process/v2/process.hpp>
#include <csignal>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <regex>
#include <set>
#include <thread>

#include "app/config.h"
#include "app/instance.h"
#include "app/slice_tuning.h"
#include "app/station_lock.h"
#include "app/wideband.h"
#include "core/database.h"
#include "receiver/receiver.h"
#include "report/cluster.h"
#include "report/reporting.h"
#include "store/store.h"
#include "store/workers.h"

namespace skim {
namespace {
using Json = nlohmann::json;
using Process = boost::process::v2::process;
volatile std::sig_atomic_t stopping = 0;
void Signal(int) { stopping = 1; }
void Init(SmDatabase& db) {
  db.Exec(
      "CREATE TABLE IF NOT EXISTS station_runtime(id INTEGER PRIMARY KEY "
      "CHECK(id=1),generation INTEGER NOT NULL)");
  db.Exec("INSERT OR IGNORE INTO station_runtime VALUES(1,0)");
  db.Exec(
      "CREATE TABLE IF NOT EXISTS workers(id TEXT PRIMARY KEY,plan TEXT NOT "
      "NULL,status TEXT NOT NULL)");
  db.Exec(
      "CREATE TABLE IF NOT EXISTS system_log(utc INTEGER NOT NULL,component "
      "TEXT NOT NULL,message TEXT NOT NULL)");
  db.Exec(
      "CREATE TABLE IF NOT EXISTS schedule_overrides(receiver TEXT PRIMARY KEY,"
      "schedule TEXT NOT NULL, expires INTEGER NOT NULL)");
  db.Exec(
      "CREATE TABLE IF NOT EXISTS worker_restarts(id TEXT PRIMARY KEY,"
      "generation INTEGER NOT NULL)");
}

Json ReadOverrides(SmDatabase& db) {
  auto rows = db.Prepare(
      "SELECT receiver,schedule,expires FROM schedule_overrides "
      "WHERE expires > unixepoch()");
  auto overrides = Json::object();
  int result;
  while ((result = sqlite3_step(rows.get())) == SQLITE_ROW) {
    const auto id =
        reinterpret_cast<const char*>(sqlite3_column_text(rows.get(), 0));
    const auto raw =
        reinterpret_cast<const char*>(sqlite3_column_text(rows.get(), 1));
    auto schedule = Json::parse(raw, nullptr, false);
    if (schedule.is_object())
      overrides[id] = {{"schedule", std::move(schedule)},
                       {"expires", sqlite3_column_int64(rows.get(), 2)}};
  }
  if (result != SQLITE_DONE)
    throw std::runtime_error("cannot read schedule overrides");
  return overrides;
}
void Log(SmDatabase& db, const std::string& component,
         const std::string& message) {
  std::cerr << "[" + component + "] " + message + "\n";
  auto q = db.Prepare("INSERT INTO system_log VALUES(unixepoch(),?,?)");
  db.Text(q.get(), 1, component);
  db.Text(q.get(), 2, message);
  db.Done(q.get());
}
void Validate(const Json& cfg) {
  std::set<std::string> ids, devices, slice_ids, decoder_ids;
  if (!cfg.is_object() || !cfg.at("receivers").is_array())
    throw std::runtime_error("receivers must be an array");
  auto services = cfg.value("services", Json::object());
  if (!services.is_object())
    throw std::runtime_error("services must be an object");
  for (auto it = services.begin(); it != services.end(); ++it) {
    std::string name = it.key();
    if (name != "cluster" && name != "rbn" && name != "psk" &&
        name != "wsprnet")
      throw std::runtime_error("unknown reporting service");
    auto options = it.value();
    if (!options.is_object())
      throw std::runtime_error("reporting service options must be a table");
    if (options.contains("enabled") && !options.at("enabled").is_boolean())
      throw std::runtime_error("reporting service enabled must be a boolean");
    if (!options.value("enabled", false)) continue;
    if (!std::regex_match(options.at("callsign").get<std::string>(),
                          std::regex("[A-Z0-9/-]{2,16}")) ||
        !std::regex_match(options.at("grid").get<std::string>(),
                          std::regex("[A-Ra-r]{2}[0-9]{2}([A-Xa-x]{2})?")))
      throw std::runtime_error(
          "reporting service requires station callsign and grid");
    if (name == "rbn") {
      if (options.at("sign_in").get<std::string>().empty())
        throw std::runtime_error("RBN sign_in required");
      options.at("band_limits").get<std::string>();
    }
    for (auto key : {"port", "backup_port"}) {
      if (!options.contains(key)) continue;
      const auto& port = options.at(key);
      if (!port.is_number_integer() || port < 1 || port > 65535)
        throw std::runtime_error("invalid service port");
    }
  }
  for (const auto& r : cfg.at("receivers")) {
    if (r.contains("enabled") && !r.at("enabled").is_boolean())
      throw std::runtime_error("receiver enabled must be a boolean");
    std::string id = r.at("receiver"), dev = r.at("device");
    if (id.empty() || id[0] == '@' || dev.empty() || dev == "-" ||
        !ids.insert(id).second || !devices.insert(dev).second)
      throw std::runtime_error(
          "each station receiver needs a unique ID and device");
    if (!r.at("rate").is_number_integer() || r.at("rate") < 12000 ||
        r.at("rate") > 6144000)
      throw std::runtime_error("invalid receiver sample rate");
    auto check_center = [](const Json& center) {
      if (!center.is_number_integer() || center < 1 || center > 1000000000000LL)
        throw std::runtime_error("invalid receiver center");
    };
    if (r.contains("center_hz")) check_center(r.at("center_hz"));
    if (IsWavReceiver(dev) &&
        (dev.size() == 4 || r.contains("gain_db") || r.contains("ppm")))
      throw std::runtime_error(
          "WAV receiver requires a path and has no gain or PPM settings");
    if (r.contains("repeat") &&
        (!IsWavReceiver(dev) || !r.at("repeat").is_boolean()))
      throw std::runtime_error("repeat must be a boolean on a WAV receiver");
    if (r.contains("utc_ms")) {
      auto utc = r.at("utc_ms");
      if (!IsWavReceiver(dev) || !utc.is_number_integer() || utc < 0 ||
          utc > 100000000000000LL)
        throw std::runtime_error(
            "utc_ms must be a nonnegative WAV start time in milliseconds");
    }
    auto check_slices = [&](const Json& configured, int64_t center) {
      if (!configured.is_array())
        throw std::runtime_error("expected an array of decoder slices");
      for (const auto& slice : configured) {
        auto check_id = [&](const char* key, std::set<std::string>& seen) {
          if (!slice.contains(key)) return;
          auto id = slice.at(key).get<std::string>();
          if (id.empty() || !seen.insert(id).second)
            throw std::runtime_error(std::string("empty or duplicate ") + key);
        };
        check_id("slice_id", slice_ids);
        check_id("decoder_id", decoder_ids);
        std::string mode = slice.at("mode");
        const bool skimmer = IsSkimmer(mode);
        bool timed = IsTimedDecoder(mode);
        if (!timed && !skimmer && mode != "SSTV")
          throw std::runtime_error("invalid decoder mode");
        auto frequency = SliceReference(slice, mode, center);
        if (frequency <= 0 || frequency > 1000000000000LL)
          throw std::runtime_error(skimmer ? "invalid slice center"
                                           : "invalid slice dial");
      }
    };
    if (r.contains("attached_slices"))
      check_slices(r.at("attached_slices"), r.value("center_hz", int64_t{1}));
    auto checkplan = [&](const Json& p) {
      check_center(p.at("center_hz"));
      if (p.contains("slices")) check_slices(p.at("slices"), p.at("center_hz"));
    };
    if (r.contains("gain_db") && std::abs(r.at("gain_db").get<double>()) > 200)
      throw std::runtime_error("invalid gain");
    if (r.contains("ppm") && std::abs(r.at("ppm").get<double>()) > 1000)
      throw std::runtime_error("invalid frequency correction");
    if (r.contains("slices")) checkplan(r);
    const auto schedules = r.value("schedules", Json::array());
    if (!schedules.is_array())
      throw std::runtime_error("expected an array of schedules");
    for (const auto& w : schedules) {
      int start = w.at("start_minute"), end = w.at("end_minute");
      w.at("priority").get<int>();
      w.value("name", "");
      w.value("enabled", true);
      int days = w.value("days", 127);
      if (days < 1 || days > 127)
        throw std::runtime_error("select at least one valid schedule day");
      if (start < 0 || start >= 1440 || end < 0 || end >= 1440)
        throw std::runtime_error("invalid UTC schedule minutes");
      checkplan(w);
    }
  }
}
}  // namespace

Json SmInstance::WorkerPlans(const char* database, const char* gallery,
                             const std::filesystem::path& config_dir,
                             int minute, int weekday,
                             const Json& overrides) const {
  Json jobs = Json::object();
  for (const auto* receiver : receivers()) {
    if (!receiver->enabled()) continue;
    const auto& id = receiver->id();
    const auto& r = receiver->settings();
    const auto schedules = r.value("schedules", Json::array());
    const Json* winner = nullptr;
    std::optional<size_t> schedule_index;
    int64_t override_expiry = 0;
    if (overrides.contains(id)) {
      const auto& override = overrides.at(id);
      auto selected = std::find(schedules.begin(), schedules.end(),
                                override.at("schedule"));
      if (selected != schedules.end()) {
        winner = &*selected;
        schedule_index = static_cast<size_t>(selected - schedules.begin());
        override_expiry = override.at("expires");
      }
    }
    if (!winner && r.contains("slices")) winner = &r;
    if (!winner) {
      for (size_t index = 0; index < schedules.size(); ++index) {
        const auto& w = schedules[index];
        if (!w.value("enabled", true)) continue;
        int start = w.at("start_minute"), end = w.at("end_minute");
        bool active =
            start == end || (start < end ? minute >= start && minute < end
                                         : minute >= start || minute < end);
        int day = start > end && minute < end ? (weekday + 6) % 7 : weekday;
        active = active && (w.value("days", 127) & (1 << day));
        if (active && (!winner || w.at("priority") < winner->at("priority"))) {
          winner = &w;
          schedule_index = index;
        }
      }
    }
    if (!winner) continue;
    auto device = receiver->device();
    if (IsWavReceiver(device)) {
      auto path =
          ResolveWavPath(std::string_view(device).substr(4), config_dir);
      device = "wav:" + path.string();
    }
    const int64_t center = winner->at("center_hz");
    auto active = Json::array();
    const auto selected_group =
        schedule_index ? SmSliceGroup::Scheduled : SmSliceGroup::Manual;
    for (const auto* slice : receiver->slices()) {
      if (slice->group() == selected_group &&
          (!schedule_index || slice->schedule_index() == *schedule_index) &&
          slice->fits(center))
        active.push_back(slice->WorkerPlan());
    }
    for (const auto* slice : receiver->slices())
      if (slice->group() == SmSliceGroup::Attached && slice->fits(center))
        active.push_back(slice->WorkerPlan());
    jobs[id] = {{"receiver", id},       {"receiver_id", id},
                {"device", device},     {"rate", receiver->rate()},
                {"center_hz", center},  {"slices", std::move(active)},
                {"database", database}, {"gallery", gallery}};
    for (auto key : {"gain_db", "ppm", "utc_ms", "repeat"})
      if (r.contains(key)) jobs[id][key] = r.at(key);
    if (override_expiry) jobs[id]["override_expires"] = override_expiry;
  }
  for (const auto* service : services())
    if (service->enabled())
      jobs["@" + service->id()] = {{"service", service->id()},
                                   {"options", service->options()}};
  return jobs;
}

namespace {
struct SmJob {
  std::unique_ptr<Process> process;
  Json plan;
  bool finished = false;
  std::chrono::steady_clock::time_point retry{};
  ~SmJob();
};
void Stop(SmJob& job) {
  if (!job.process) return;
  boost::system::error_code requested;
  if (job.process->running()) job.process->request_exit(requested);
  for (int i = 0; !requested && i < 20 && job.process->running(); ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  if (job.process->running()) job.process->terminate();
  job.process->wait();
  job.process.reset();
}
SmJob::~SmJob() {
  try {
    Stop(*this);
  } catch (const std::exception& error) {
    std::cerr << "Worker shutdown: " << error.what() << '\n';
  }
}
}  // namespace
void ValidateStation(const Json& config) { Validate(config); }
void RestartStation(const char* path) {
  SmDatabase db(path);
  Init(db);
  db.Exec("UPDATE station_runtime SET generation=generation+1 WHERE id=1");
}
void RestartServiceWorker(const char* path, const std::string& service) {
  static const std::set<std::string> allowed{"cluster", "psk", "rbn",
                                             "wsprnet"};
  if (!allowed.count(service))
    throw std::runtime_error("unknown reporting service");
  SmDatabase db(path);
  Init(db);
  auto request = db.Prepare(
      "INSERT INTO worker_restarts(id,generation) VALUES(?,1) "
      "ON CONFLICT(id) DO UPDATE SET generation=generation+1");
  db.Text(request.get(), 1, "@" + service);
  db.Done(request.get());
}
void SetScheduleOverride(const char* path, const std::string& receiver,
                         const Json& schedule, int minutes) {
  SmDatabase db(path);
  Init(db);
  db.Exec("DELETE FROM schedule_overrides WHERE expires <= unixepoch()");
  if (minutes == 0) {
    auto clear = db.Prepare("DELETE FROM schedule_overrides WHERE receiver=?");
    db.Text(clear.get(), 1, receiver);
    db.Done(clear.get());
  } else {
    auto set = db.Prepare(
        "INSERT OR REPLACE INTO schedule_overrides "
        "VALUES(?,?,unixepoch()+?*60)");
    db.Text(set.get(), 1, receiver);
    db.Text(set.get(), 2, schedule.dump());
    db.Check(sqlite3_bind_int(set.get(), 3, minutes));
    db.Done(set.get());
  }
}
Json ScheduleOverrides(const char* path) {
  SmDatabase db(path);
  Init(db);
  return ReadOverrides(db);
}
void TestReceiverCapture(const Json& settings) {
  boost::asio::io_context io;
  Process capture(io, boost::dll::program_location(),
                  {"--test-receiver", settings.dump()});
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (capture.running()) {
    if (std::chrono::steady_clock::now() >= deadline) {
      capture.terminate();
      capture.wait();
      throw std::runtime_error("No IQ samples received within 3 seconds.");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  capture.wait();
  if (capture.exit_code() != 0)
    throw std::runtime_error(
        "IQ capture failed. Check the device, sample rate, and tuner "
        "settings.");
}
void RunStationWorker(const char* path, const std::string& id) {
  auto plan = Json::parse(SmWorkerStore(path).Plan(id));
  if (plan.contains("service")) {
    auto svc = plan.at("service").get<std::string>();
    auto& opts = plan.at("options");
    if (svc == "cluster")
      ServeCluster(path,
                   opts.at("callsign").get_ref<const std::string&>().c_str(),
                   opts.at("grid").get_ref<const std::string&>().c_str(),
                   opts.value("port", 7300));
    else
      RunReporter(svc.c_str(), path, opts);
  } else
    Wideband(plan);
}
void SmInstance::Run(const std::filesystem::path& config) {
  auto startup = LoadSettings(config);
  const auto database = OperationalDatabasePath(startup.database);
  const auto database_name = database.string();
  const char* path = database_name.c_str();
  const char* gallery = startup.gallery.c_str();
  auto lockfile = StationLockPath(database).string();
  std::ofstream(lockfile, std::ios::app).close();
  boost::interprocess::file_lock lock(lockfile.c_str());
  if (!lock.try_lock()) throw std::runtime_error("station already running");
  // Persist legacy IDs after taking the station lock.
  const auto settings = PersistDecoderIds(config, startup);
  auto instance = FromSettings(settings);
  auto revision = settings.revision;
  SaveSlot(path, "", "", 0, {});
  SmDatabase db(path);
  Init(db);
  SmWorkerStore workers(path);
  workers.Clear();
  Log(db, "controller", "Station started with " + config.string());
  boost::asio::io_context io;
  auto exe = boost::dll::program_location().parent_path() /
#ifdef _WIN32
             "SkimMasterTools.exe";
#else
             "SkimMasterTools";
#endif
  std::map<std::string, SmJob> jobs;
  std::signal(SIGTERM, Signal);
  std::signal(SIGINT, Signal);
#ifdef SIGBREAK
  std::signal(SIGBREAK, Signal);
#endif
  auto start = [&](std::vector<std::string> args) {
    return std::make_unique<Process>(io, exe, args);
  };
  SmJob panel;
  std::string last_reload_error;
  int64_t generation = -1;
  std::map<std::string, int64_t> service_restarts;
  while (!stopping) {
    if (panel.process && !panel.process->running()) {
      panel.process->wait();
      Log(db, "panel",
          "Exited (" + std::to_string(panel.process->exit_code()) +
              "), retrying");
      panel.process.reset();
      panel.retry = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    }
    if (!panel.process && std::chrono::steady_clock::now() >= panel.retry) {
      try {
        panel.process =
            start({"--station-panel", config.string(), startup.address,
                   std::to_string(startup.port), path, gallery});
      } catch (const boost::system::system_error& error) {
        Log(db, "panel", "Launch failed: " + std::string(error.what()));
        panel.retry =
            std::chrono::steady_clock::now() + std::chrono::seconds(10);
      }
    }
    try {
      auto settings = LoadSettings(config);
      if (settings.revision != revision) {
        settings = PersistDecoderIds(config, std::move(settings));
        instance = FromSettings(settings);
        revision = std::move(settings.revision);
        Log(db, "controller", "Reloaded station TOML");
      }
      last_reload_error.clear();
    } catch (const std::exception& error) {
      if (last_reload_error != error.what()) {
        last_reload_error = error.what();
        Log(db, "controller", "TOML reload failed: " + last_reload_error);
      }
    }
    auto seconds = std::time(nullptr);
    auto weekday = (seconds / 86400 + 3) % 7;
    auto plans = instance->WorkerPlans(path, gallery, config.parent_path(),
                                       (seconds / 60) % 1440, weekday,
                                       ReadOverrides(db));
    std::set<std::string> restart_requested;
    auto restart_rows = db.Prepare("SELECT id,generation FROM worker_restarts");
    int restart_result;
    while ((restart_result = sqlite3_step(restart_rows.get())) == SQLITE_ROW) {
      const auto* id = reinterpret_cast<const char*>(
          sqlite3_column_text(restart_rows.get(), 0));
      if (!id) continue;
      const std::string name(id);
      const auto token = sqlite3_column_int64(restart_rows.get(), 1);
      if (token > service_restarts[name]) {
        service_restarts[name] = token;
        restart_requested.insert(name);
      }
    }
    if (restart_result != SQLITE_DONE)
      throw std::runtime_error("cannot read service restart requests");
    restart_rows.reset();
    auto runtime =
        db.Prepare("SELECT generation FROM station_runtime WHERE id=1");
    if (sqlite3_step(runtime.get()) != SQLITE_ROW)
      throw std::runtime_error("missing station runtime state");
    auto next_generation = sqlite3_column_int64(runtime.get(), 0);
    runtime.reset();
    bool restart = next_generation != generation;
    generation = next_generation;
    for (auto it = jobs.begin(); it != jobs.end();) {
      if (restart || restart_requested.count(it->first) ||
          !plans.contains(it->first) ||
          it->second.plan != plans.at(it->first)) {
        Stop(it->second);
        workers.Remove(it->first);
        Log(db, it->first, "Stopped allocation");
        it = jobs.erase(it);
      } else
        ++it;
    }
    for (auto it = plans.begin(); it != plans.end(); ++it) {
      auto& job = jobs[it.key()];
      auto state = [&](const std::string& status) {
        workers.Save(it.key(), it.value().dump(), status);
        Log(db, it.key(), status);
      };
      if (job.process && !job.process->running()) {
        job.process->wait();
        auto code = job.process->exit_code();
        job.finished = code == 0 && IsWavReceiver(job.plan.value("device", ""));
        state(job.finished ? "WAV playback complete"
                           : "Exited (" + std::to_string(code) + "), retrying");
        job.process.reset();
        job.retry = std::chrono::steady_clock::now() + std::chrono::seconds(10);
      }
      if (!job.process && !job.finished &&
          std::chrono::steady_clock::now() >= job.retry) {
        job.plan = it.value();
        state("Starting allocation");
        try {
          job.process = start({"--station-worker", path, it.key()});
        } catch (const boost::system::system_error& error) {
          state("Launch failed: " + std::string(error.what()));
          job.retry =
              std::chrono::steady_clock::now() + std::chrono::seconds(10);
          continue;
        }
        state("Worker launched");
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
  }
  for (auto& item : jobs) Stop(item.second);
  Stop(panel);
  workers.Clear();
  Log(db, "controller", "Station stopped");
}
}  // namespace skim
