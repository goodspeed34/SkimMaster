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
#include "core/process_usage.h"
#include "core/schema.h"
#include "decoder/settings.h"
#include "receiver/receiver.h"
#include "report/cluster.h"
#include "report/reporting.h"
#include "store/workers.h"

namespace skim {
namespace {
using Json = nlohmann::json;
using Process = boost::process::v2::process;
volatile std::sig_atomic_t stopping = 0;
void Signal(int) { stopping = 1; }
void Init(SmDatabase& db) { MigrateStationSchema(db.get()); }

constexpr auto kRetryDelay = std::chrono::seconds(10);
constexpr auto kUsageInterval = std::chrono::seconds(5);

struct SmRuntime {
  int64_t generation;
  int64_t exec_generation;
};

SmRuntime ReadRuntime(SmDatabase& db) {
  auto query = db.Prepare(
      "SELECT generation,exec_generation FROM station_runtime WHERE id=1");
  if (sqlite3_step(query.get()) != SQLITE_ROW)
    throw std::runtime_error("missing station runtime state");
  return {sqlite3_column_int64(query.get(), 0),
          sqlite3_column_int64(query.get(), 1)};
}

std::map<std::string, int64_t> ReadRestartRequests(SmDatabase& db) {
  auto rows = db.Prepare("SELECT id,generation FROM worker_restarts");
  std::map<std::string, int64_t> requests;
  int result;
  while ((result = sqlite3_step(rows.get())) == SQLITE_ROW) {
    const auto* id =
        reinterpret_cast<const char*>(sqlite3_column_text(rows.get(), 0));
    if (id) requests[id] = sqlite3_column_int64(rows.get(), 1);
  }
  if (result != SQLITE_DONE)
    throw std::runtime_error("cannot read service restart requests");
  return requests;
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
  const auto decoders = cfg.value("decoders", Json::object());
  if (!decoders.is_object())
    throw std::runtime_error("decoders must be a table of modes");
  for (auto it = decoders.begin(); it != decoders.end(); ++it) {
    const auto& mode = it.key();
    if (mode != "CW" && mode != "RTTY" && mode != "FT8" && mode != "FT4" &&
        mode != "WSPR" && mode != "SSTV")
      throw std::runtime_error("unknown decoder mode in decoders: " + mode);
    ResolveDecoderSettings(mode, it.value());
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
    if (IsFileReceiver(dev) && (dev.size() == DriverFor(dev).prefix.size() ||
                                r.contains("gain_db") || r.contains("ppm")))
      throw std::runtime_error(
          "WAV receiver requires a path and has no gain or PPM settings");
    if (r.contains("repeat") &&
        (!IsFileReceiver(dev) || !r.at("repeat").is_boolean()))
      throw std::runtime_error("repeat must be a boolean on a WAV receiver");
    if (r.contains("utc_ms")) {
      auto utc = r.at("utc_ms");
      if (!IsFileReceiver(dev) || !utc.is_number_integer() || utc < 0 ||
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
        if (slice.contains("settings"))
          ResolveDecoderSettings(mode, slice.at("settings"));
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
    device = ResolveReceiverDevice(device, config_dir);
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
  SmWorkerRecord record;
  SmCpuMeter cpu;
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
void LogStation(const char* path, const std::string& component,
                const std::string& message) {
  SmDatabase db(path);
  Init(db);
  Log(db, component, message);
}
void RestartStation(const char* path) {
  SmDatabase db(path);
  Init(db);
  db.Exec("UPDATE station_runtime SET generation=generation+1 WHERE id=1");
}
void RequestFullRestart(const char* path) {
  SmDatabase db(path);
  Init(db);
  db.Exec(
      "UPDATE station_runtime SET exec_generation=exec_generation+1 "
      "WHERE id=1");
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
SmRunOutcome SmInstance::Run(const std::filesystem::path& config) {
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
  stopping = 0;
  std::signal(SIGTERM, Signal);
  std::signal(SIGINT, Signal);
#ifdef SIGBREAK
  std::signal(SIGBREAK, Signal);
#endif
  const auto now = [] { return static_cast<int64_t>(std::time(nullptr)); };
  const auto start = [&](std::vector<std::string> args) {
    return std::make_unique<Process>(io, exe, args);
  };
  // Records a lifecycle change for a job and mirrors it to the system log.
  const auto transition = [&](SmJob& job, SmWorkerState state,
                              const std::string& status) {
    job.record.state = state;
    job.record.status = status;
    if (state != SmWorkerState::Running) {
      job.record.pid.reset();
      job.record.cpu_percent.reset();
      job.record.rss_bytes.reset();
    }
    workers.Save(job.record);
    Log(db, job.record.id == "@panel" ? "panel" : job.record.id, status);
  };
  // Launches a job's process, or schedules a retry if that fails.
  const auto launch = [&](SmJob& job, std::vector<std::string> args) {
    job.cpu = SmCpuMeter{};
    try {
      job.process = start(std::move(args));
    } catch (const boost::system::system_error& error) {
      transition(job, SmWorkerState::Failed,
                 "Launch failed: " + std::string(error.what()));
      job.retry = std::chrono::steady_clock::now() + kRetryDelay;
      return;
    }
    if (job.record.started_at) ++job.record.restarts;
    job.record.pid = job.process->id();
    job.record.started_at = now();
    transition(job, SmWorkerState::Running, "Worker launched");
  };
  // Collects an exited process. Returns its exit code.
  const auto reap = [&](SmJob& job) {
    job.process->wait();
    const int code = job.process->exit_code();
    job.process.reset();
    job.retry = std::chrono::steady_clock::now() + kRetryDelay;
    return code;
  };

  SmJob controller;
  controller.record.id = "@controller";
  controller.record.kind = SmWorkerKind::Controller;
  controller.record.pid = CurrentProcessId();
  controller.record.started_at = now();
  controller.record.state = SmWorkerState::Running;
  controller.record.status = "Supervising";
  workers.Save(controller.record);

  SmJob panel;
  panel.record.id = "@panel";
  panel.record.kind = SmWorkerKind::Panel;

  std::string last_reload_error;
  int64_t generation = -1;
  const int64_t exec_generation = ReadRuntime(db).exec_generation;
  std::map<std::string, int64_t> service_restarts;
  auto next_sample = std::chrono::steady_clock::now();
  auto outcome = SmRunOutcome::Stopped;
  while (!stopping) {
    if (panel.process && !panel.process->running()) {
      const int code = reap(panel);
      transition(panel, SmWorkerState::Exited,
                 "Exited (" + std::to_string(code) + "), retrying");
    }
    if (!panel.process && std::chrono::steady_clock::now() >= panel.retry)
      launch(panel, {"--station-panel", config.string(), startup.address,
                     std::to_string(startup.port), path, gallery});
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
    for (const auto& [name, token] : ReadRestartRequests(db))
      if (token > service_restarts[name]) {
        service_restarts[name] = token;
        restart_requested.insert(name);
      }
    const auto runtime = ReadRuntime(db);
    if (runtime.exec_generation != exec_generation) {
      Log(db, "controller", "Full station restart requested");
      outcome = SmRunOutcome::Restart;
      break;
    }
    bool restart = runtime.generation != generation;
    generation = runtime.generation;
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
      if (job.record.id.empty()) {
        job.record.id = it.key();
        job.record.kind = it.value().contains("service")
                              ? SmWorkerKind::Service
                              : SmWorkerKind::Receiver;
      }
      if (job.process && !job.process->running()) {
        const int code = reap(job);
        job.finished =
            code == 0 && IsFileReceiver(job.plan.value("device", ""));
        if (job.finished)
          transition(job, SmWorkerState::Finished, "WAV playback complete");
        else
          transition(job, SmWorkerState::Exited,
                     "Exited (" + std::to_string(code) + "), retrying");
      }
      if (!job.process && !job.finished &&
          std::chrono::steady_clock::now() >= job.retry) {
        job.plan = it.value();
        job.record.plan = job.plan.dump();
        transition(job, SmWorkerState::Starting, "Starting allocation");
        launch(job, {"--station-worker", path, it.key()});
      }
    }
    if (std::chrono::steady_clock::now() >= next_sample) {
      next_sample = std::chrono::steady_clock::now() + kUsageInterval;
      const double wall =
          std::chrono::duration<double>(
              std::chrono::steady_clock::now().time_since_epoch())
              .count();
      auto sample = [&](SmJob& job) {
        if (!job.record.pid || job.record.state != SmWorkerState::Running)
          return;
        const auto usage = SampleProcess(*job.record.pid);
        if (!usage) return;
        job.record.cpu_percent = job.cpu.Update(usage->cpu_seconds, wall);
        job.record.rss_bytes = usage->memory_bytes;
        workers.SaveUsage(job.record.id, job.record.cpu_percent,
                          job.record.rss_bytes);
      };
      sample(controller);
      sample(panel);
      for (auto& item : jobs) sample(item.second);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
  }
  for (auto& item : jobs) Stop(item.second);
  Stop(panel);
  workers.Clear();
  Log(db, "controller",
      outcome == SmRunOutcome::Restart ? "Station restarting"
                                       : "Station stopped");
  return outcome;
}
}  // namespace skim
