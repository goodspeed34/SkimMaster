// Unit tests for station logic that the end-to-end suites only reach
// indirectly: schedule resolution, slice coverage, TOML validation and the
// panel's form and export helpers.

#include <httplib.h>
#include <sqlite3.h>

#include <cmath>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "app/config.h"
#include "app/instance.h"
#include "app/slice_tuning.h"
#include "app/station.h"
#include "core/bands.h"
#include "core/process_usage.h"
#include "core/schema.h"
#include "web/common.h"

namespace {
using Json = nlohmann::json;
using skim::SmInstance;

int failures = 0;

void Expect(bool condition, const std::string& message) {
  if (condition) return;
  ++failures;
  std::cerr << "FAIL: " << message << '\n';
}

template <typename T>
void ExpectEqual(const T& actual, const T& expected,
                 const std::string& message) {
  if (actual == expected) return;
  ++failures;
  std::cerr << "FAIL: " << message << ": got " << Json(actual).dump()
            << ", expected " << Json(expected).dump() << '\n';
}

void ExpectThrows(const std::function<void()>& action,
                  const std::string& message) {
  try {
    action();
  } catch (const std::exception&) {
    return;
  }
  ++failures;
  std::cerr << "FAIL: expected an exception: " << message << '\n';
}

constexpr int kMonday = 0;
constexpr int kTuesday = 1;
constexpr int kSunday = 6;

int Minute(int hour, int minute = 0) { return hour * 60 + minute; }

Json Schedule(const char* name, int start, int end, int priority,
              int64_t center, int days = 127) {
  return {{"name", name},
          {"start_minute", start},
          {"end_minute", end},
          {"priority", priority},
          {"center_hz", center},
          {"days", days},
          {"slices", Json::array({{{"mode", "FT8"}, {"dial_hz", center}}})}};
}

Json Receiver(Json schedules) {
  return {{"receiver", "hf"},
          {"device", "rtltcp:127.0.0.1:1234"},
          {"rate", 48000},
          {"schedules", std::move(schedules)}};
}

std::shared_ptr<const SmInstance> Instance(const Json& receivers) {
  skim::SmInstanceSettings settings{};
  settings.topology["receivers"] = receivers;
  settings.topology["services"] = Json::object();
  return SmInstance::FromSettings(settings);
}

Json Plans(const Json& receivers, int minute, int weekday,
           const Json& overrides = Json::object()) {
  return Instance(receivers)->WorkerPlans("db.sqlite", "gallery", ".", minute,
                                          weekday, overrides);
}

// The center of the winning plan, or 0 when the receiver is idle.
int64_t WinningCenter(const Json& receivers, int minute, int weekday,
                      const Json& overrides = Json::object()) {
  auto plans = Plans(receivers, minute, weekday, overrides);
  if (!plans.contains("hf")) return 0;
  return plans.at("hf").at("center_hz");
}

void TestScheduleWindows() {
  const Json day = Json::array(
      {Receiver({Schedule("day", Minute(8), Minute(18), 1, 14074000)})});
  ExpectEqual<int64_t>(WinningCenter(day, Minute(8), kMonday), 14074000,
                       "a window includes its start minute");
  ExpectEqual<int64_t>(WinningCenter(day, Minute(17, 59), kMonday), 14074000,
                       "a window includes its last minute");
  ExpectEqual<int64_t>(WinningCenter(day, Minute(18), kMonday), 0,
                       "a window excludes its end minute");
  ExpectEqual<int64_t>(WinningCenter(day, Minute(7, 59), kMonday), 0,
                       "a window is idle before its start");

  const Json full = Json::array(
      {Receiver({Schedule("full", Minute(6), Minute(6), 1, 7074000)})});
  ExpectEqual<int64_t>(WinningCenter(full, Minute(5, 59), kSunday), 7074000,
                       "equal start and end cover the whole day");

  // 22:00-02:00 on Monday only. After midnight it belongs to Monday's run.
  const Json night = Json::array({Receiver(
      {Schedule("night", Minute(22), Minute(2), 1, 3573000, 1 << kMonday)})});
  ExpectEqual<int64_t>(WinningCenter(night, Minute(23), kMonday), 3573000,
                       "an overnight window runs on its start day");
  ExpectEqual<int64_t>(WinningCenter(night, Minute(1), kTuesday), 3573000,
                       "an overnight window continues into the next day");
  ExpectEqual<int64_t>(WinningCenter(night, Minute(1), kMonday), 0,
                       "the early hours of the start day belong to Sunday");
  ExpectEqual<int64_t>(WinningCenter(night, Minute(23), kTuesday), 0,
                       "the day mask applies to the start day");

  const Json sunday_night = Json::array({Receiver(
      {Schedule("sunday", Minute(23), Minute(1), 1, 1840000, 1 << kSunday)})});
  ExpectEqual<int64_t>(WinningCenter(sunday_night, Minute(0, 30), kMonday),
                       1840000, "Sunday's overnight window wraps to Monday");
}

void TestSchedulePriority() {
  const Json ranked = Json::array(
      {Receiver({Schedule("low", Minute(0), Minute(0), 5, 14074000),
                 Schedule("high", Minute(10), Minute(12), 2, 21074000),
                 Schedule("tie", Minute(10), Minute(12), 2, 28074000)})});
  ExpectEqual<int64_t>(WinningCenter(ranked, Minute(11), kMonday), 21074000,
                       "a lower priority number wins, list order breaks ties");
  ExpectEqual<int64_t>(WinningCenter(ranked, Minute(13), kMonday), 14074000,
                       "the fallback schedule runs outside the others");

  auto disabled = ranked;
  disabled[0]["schedules"][1]["enabled"] = false;
  ExpectEqual<int64_t>(WinningCenter(disabled, Minute(11), kMonday), 28074000,
                       "a disabled schedule never wins");
}

void TestManualAndOverride() {
  auto receivers = Json::array(
      {Receiver({Schedule("day", Minute(0), Minute(0), 1, 14074000)})});
  receivers[0]["center_hz"] = 7074000;
  receivers[0]["slices"] =
      Json::array({{{"mode", "FT8"}, {"dial_hz", 7074000}}});
  ExpectEqual<int64_t>(WinningCenter(receivers, Minute(12), kMonday), 7074000,
                       "a manual allocation takes precedence over schedules");

  const Json overrides = {
      {"hf",
       {{"schedule", receivers[0]["schedules"][0]}, {"expires", 1234567}}}};
  auto plans = Plans(receivers, Minute(12), kMonday, overrides);
  ExpectEqual<int64_t>(plans.at("hf").at("center_hz"), 14074000,
                       "Run Now overrides the manual allocation");
  ExpectEqual<int64_t>(plans.at("hf").at("override_expires"), 1234567,
                       "the plan carries the override expiry");

  const Json stale = {
      {"hf",
       {{"schedule", Schedule("gone", 0, 0, 1, 1)}, {"expires", 1234567}}}};
  ExpectEqual<int64_t>(WinningCenter(receivers, Minute(12), kMonday, stale),
                       7074000,
                       "an override for a removed schedule is ignored");

  receivers[0]["enabled"] = false;
  Expect(!Plans(receivers, Minute(12), kMonday).contains("hf"),
         "a disabled receiver gets no plan");
}

void TestSliceCoverage() {
  // 48 kHz capture at 14.075 MHz covers 14.051-14.099 MHz.
  Json receiver = {{"receiver", "hf"},
                   {"device", "rtltcp:127.0.0.1:1234"},
                   {"rate", 48000},
                   {"center_hz", 14075000}};
  receiver["slices"] = Json::array({
      {{"mode", "FT8"}, {"dial_hz", 14074000}},  // 12 kHz slice at +500 Hz.
      {{"mode", "FT8"}, {"dial_hz", 14090000}},  // Reaches 14.0975 MHz.
      {{"mode", "FT8"}, {"dial_hz", 14092000}},  // Reaches 14.0995 MHz.
      {{"mode", "CW"}},                          // Receiver center.
  });
  receiver["attached_slices"] =
      Json::array({{{"mode", "RTTY"}, {"center_hz", 14080000}},
                   {{"mode", "RTTY"}, {"center_hz", 14095000}}});
  auto plans = Plans(Json::array({receiver}), Minute(12), kMonday);
  std::vector<std::string> active;
  for (const auto& slice : plans.at("hf").at("slices"))
    active.push_back(slice.at("mode").get<std::string>() + "@" +
                     std::to_string(slice.value(
                         "dial_hz", slice.value("center_hz", int64_t{0}))));
  ExpectEqual(active,
              std::vector<std::string>{"FT8@14074000", "FT8@14090000", "CW@0",
                                       "RTTY@14080000"},
              "only slices whose passband fits are activated");

  ExpectEqual<int64_t>(skim::SliceReference({{"dial_hz", 7030000}}, "CW", 1),
                       7030000, "legacy CW dial_hz is the slice center");
  ExpectEqual<int64_t>(skim::SliceReference(Json::object(), "RTTY", 14080000),
                       14080000, "a skimmer defaults to the receiver center");
}

void TestValidation() {
  auto config = [](Json schedule) {
    return Json{{"receivers", Json::array({Receiver({std::move(schedule)})})}};
  };
  skim::ValidateStation(config(Schedule("ok", 0, 1439, 1, 14074000)));
  ExpectThrows(
      [&] {
        skim::ValidateStation(config(Schedule("x", 0, 1440, 1, 14074000)));
      },
      "end minute 1440 is out of range");
  ExpectThrows(
      [&] {
        skim::ValidateStation(config(Schedule("x", 0, 60, 1, 14074000, 0)));
      },
      "a schedule needs at least one day");
  ExpectThrows(
      [&] {
        skim::ValidateStation(config(Schedule("x", 0, 60, 1, 14074000, 128)));
      },
      "day masks above Sunday are invalid");
  auto bad_mode = Schedule("x", 0, 60, 1, 14074000);
  bad_mode["slices"][0]["mode"] = "AM";
  ExpectThrows([&] { skim::ValidateStation(config(bad_mode)); },
               "unknown decoder modes are rejected");
  auto duplicate = Schedule("x", 0, 60, 1, 14074000);
  duplicate["slices"] = Json::array(
      {{{"mode", "FT8"}, {"dial_hz", 14074000}, {"slice_id", "same"}},
       {{"mode", "FT4"}, {"dial_hz", 14080000}, {"slice_id", "same"}}});
  ExpectThrows([&] { skim::ValidateStation(config(duplicate)); },
               "duplicate slice IDs are rejected");
  auto gain = config(Schedule("ok", 0, 60, 1, 14074000));
  gain["receivers"][0]["gain_db"] = 500.0;
  ExpectThrows([&] { skim::ValidateStation(gain); },
               "out-of-range gain is rejected");
}

void TestWebHelpers() {
  using namespace skim::web;
  ExpectEqual(Escape("<a href=\"x\">&</a>"),
              std::string("&lt;a href=&quot;x&quot;&gt;&amp;&lt;/a&gt;"),
              "HTML escaping");
  ExpectEqual(CsvDocument(Json::array({"a", "b"}),
                          Json::array({Json::array({"=cmd()", "say \"hi\""})})),
              std::string("\"a\",\"b\"\r\n\"'=cmd()\",\"say \"\"hi\"\"\"\r\n"),
              "CSV quoting neutralizes formulas");
  ExpectEqual<int64_t>(*GalleryDateStart("1970-01-02", "start"),
                       86400LL * 1000000LL, "gallery dates are UTC midnight");
  ExpectEqual<int64_t>(DaysFromCivil(2024, 2, 29) + 1,
                       DaysFromCivil(2024, 3, 1), "leap day ordering");
  GalleryDateStart("2024-02-29", "start");
  ExpectThrows([] { GalleryDateStart("2023-02-29", "start"); },
               "February 29 only exists in leap years");
  ExpectThrows([] { GalleryDateStart("2024-13-01", "start"); },
               "month 13 is invalid");
  Expect(!GalleryDateStart("", "start"), "an empty date is no filter");
  ExpectEqual(UtcMinute(Minute(9, 5)), std::string("09:05"), "UTC minute");

  httplib::Request request;
  request.params.emplace("start", "23:59");
  request.params.emplace("bad", "24:00");
  request.params.emplace("rate", "48000");
  request.params.emplace("trailing", "12x");
  request.params.emplace("gain", "nan");
  ExpectEqual(FormTime(request, "start"), Minute(23, 59), "form time");
  ExpectThrows([&] { FormTime(request, "bad"); }, "24:00 is not a UTC time");
  ExpectEqual<int64_t>(*FormInteger(request, "rate", 12000, 6144000), 48000,
                       "form integer");
  ExpectThrows([&] { FormInteger(request, "rate", 96000, 192000); },
               "form integers are range checked");
  ExpectThrows([&] { FormInteger(request, "trailing", 0, 100); },
               "form integers reject trailing text");
  Expect(!FormInteger(request, "missing", 0, 1, true),
         "an optional empty integer is absent");
  ExpectThrows([&] { FormDecimal(request, "gain"); },
               "form decimals must be finite");
}
struct SmMemoryDb {
  SmMemoryDb() {
    if (sqlite3_open(":memory:", &handle) != SQLITE_OK)
      throw std::runtime_error("cannot open memory database");
  }
  ~SmMemoryDb() { sqlite3_close(handle); }
  void Exec(const char* sql) {
    if (sqlite3_exec(handle, sql, nullptr, nullptr, nullptr) != SQLITE_OK)
      throw std::runtime_error(sqlite3_errmsg(handle));
  }
  std::string One(const char* sql) {
    sqlite3_stmt* query = nullptr;
    if (sqlite3_prepare_v2(handle, sql, -1, &query, nullptr) != SQLITE_OK)
      throw std::runtime_error(sqlite3_errmsg(handle));
    std::string value;
    if (sqlite3_step(query) == SQLITE_ROW && sqlite3_column_text(query, 0))
      value = reinterpret_cast<const char*>(sqlite3_column_text(query, 0));
    sqlite3_finalize(query);
    return value;
  }
  sqlite3* handle = nullptr;
};

void TestSchema() {
  SmMemoryDb fresh;
  skim::MigrateStationSchema(fresh.handle);
  ExpectEqual(skim::StationSchemaVersion(fresh.handle),
              skim::kStationSchemaVersion, "a new database is current");
  skim::MigrateStationSchema(fresh.handle);
  skim::VerifyStationSchema(fresh.handle);

  // A database from before versioning, with a running service worker.
  SmMemoryDb legacy;
  legacy.Exec(
      "CREATE TABLE workers(id TEXT PRIMARY KEY,plan TEXT NOT NULL,"
      "status TEXT NOT NULL);"
      "INSERT INTO workers VALUES('@psk','{}','Worker launched');"
      "INSERT INTO workers VALUES('hf','{}','Worker launched');"
      "CREATE TABLE station_runtime(id INTEGER PRIMARY KEY CHECK(id=1),"
      "generation INTEGER NOT NULL);"
      "INSERT INTO station_runtime VALUES(1,7);");
  skim::MigrateStationSchema(legacy.handle);
  skim::VerifyStationSchema(legacy.handle);
  ExpectEqual(legacy.One("SELECT kind FROM workers WHERE id='@psk'"),
              std::string("service"), "legacy service rows become services");
  ExpectEqual(legacy.One("SELECT kind FROM workers WHERE id='hf'"),
              std::string("receiver"), "legacy rows default to receivers");
  ExpectEqual(legacy.One("SELECT generation||'/'||exec_generation FROM "
                         "station_runtime"),
              std::string("7/0"), "runtime counters survive the migration");

  SmMemoryDb newer;
  newer.Exec("PRAGMA user_version=999");
  ExpectThrows([&] { skim::MigrateStationSchema(newer.handle); },
               "a newer schema is refused");

  SmMemoryDb loose;
  loose.Exec(
      "CREATE TABLE spots(receiver,utc_ms,mode,frequency_hz,message,callsign,"
      "grid)");
  skim::MigrateStationSchema(loose.handle);
  ExpectThrows([&] { skim::VerifyStationSchema(loose.handle); },
               "a spots table without its constraints is rejected");
}

void TestBandsAndUsage() {
  ExpectEqual(std::string(skim::BandName(14074000)), std::string("20m"),
              "20 m");
  ExpectEqual(std::string(skim::BandName(1840000)), std::string("160m"),
              "160 m");
  ExpectEqual(std::string(skim::BandName(144174000)), std::string("2m"), "2 m");
  Expect(skim::BandName(15000000).empty(), "WWV is outside amateur bands");

  skim::SmCpuMeter meter;
  Expect(!meter.Update(1.0, 10.0), "one sample has no rate");
  auto percent = meter.Update(1.5, 12.0);
  Expect(percent && std::abs(*percent - 25.0) < 1e-9,
         "half a CPU second over two seconds is 25%");
  Expect(!meter.Update(1.0, 13.0), "a reset CPU counter is not a rate");

  auto self = skim::SampleProcess(skim::CurrentProcessId());
  Expect(self && self->memory_bytes > 0 && self->cpu_seconds >= 0,
         "the test process can sample itself");
  Expect(!skim::SampleProcess(0x7ffffff0), "a missing process has no usage");
}
}  // namespace

int main() {
  const std::pair<const char*, void (*)()> tests[] = {
      {"schedule windows", TestScheduleWindows},
      {"schedule priority", TestSchedulePriority},
      {"manual and override", TestManualAndOverride},
      {"slice coverage", TestSliceCoverage},
      {"validation", TestValidation},
      {"web helpers", TestWebHelpers},
      {"schema", TestSchema},
      {"bands and usage", TestBandsAndUsage},
  };
  for (const auto& [name, test] : tests) {
    try {
      test();
    } catch (const std::exception& error) {
      ++failures;
      std::cerr << "FAIL: " << name << " threw: " << error.what() << '\n';
    }
  }
  if (failures) {
    std::cerr << failures << " check(s) failed\n";
    return 1;
  }
  std::cout << "all unit checks passed\n";
}
