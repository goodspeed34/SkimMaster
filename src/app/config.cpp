#include "app/config.h"

#include <openssl/sha.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <random>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <toml++/toml.hpp>

#include "app/slice_tuning.h"
#include "app/station.h"
#include "receiver/receiver.h"

namespace skim {
namespace {
using Json = nlohmann::json;
namespace fs = std::filesystem;

constexpr char kDefaultSettings[] = R"toml(# Skim Master station configuration
listen_address = "127.0.0.1"
port = 8080
# Run Setup Wizard in your local browser.
first_run_wizard = true
database = "station.sqlite"
gallery = "gallery"

# Uncomment and adjust this example for your receiver.
# [[receivers]]
# receiver = "hf"
# device = "driver=airspyhf"
# rate = 192000
# center_hz = 14075500
#
# [[receivers.slices]]
# mode = "FT8"
# dial_hz = 14074000
)toml";

Json Value(const toml::node& node) {
  if (auto t = node.as_table()) {
    auto result = Json::object();
    for (const auto& [key, child] : *t)
      result[std::string(key.str())] = Value(child);
    return result;
  }
  if (auto a = node.as_array()) {
    auto result = Json::array();
    for (const auto& child : *a) result.push_back(Value(child));
    return result;
  }
  if (auto v = node.as_string()) return v->get();
  if (auto v = node.as_integer()) return v->get();
  if (auto v = node.as_floating_point(); v && std::isfinite(v->get()))
    return v->get();
  if (auto v = node.as_boolean()) return v->get();
  throw std::runtime_error("unsupported TOML value");
}

toml::table SliceTable(const SmSliceSettings& settings) {
  toml::table slice{{"mode", settings.mode}};
  const bool skimmer = IsSkimmer(settings.mode);
  if (settings.frequency)
    slice.insert(skimmer ? "center_hz" : "dial_hz", *settings.frequency);
  else if (!skimmer)
    throw std::runtime_error("enter the decoder dial frequency");
  return slice;
}

toml::table& ReceiverTable(toml::table& station, const std::string& receiver) {
  if (const auto entries = station["receivers"].as_array())
    for (auto& entry : *entries)
      if ((*entry.as_table())["receiver"].value_or(std::string{}) == receiver)
        return *entry.as_table();
  throw std::runtime_error("receiver no longer exists");
}

toml::table& IndexedTable(toml::array* entries, size_t index) {
  if (!entries || index >= entries->size())
    throw std::runtime_error("configuration entry no longer exists");
  return *(*entries)[index].as_table();
}

// Insert IDs at each slice definition so existing TOML comments and layout
// survive.
std::string WithDecoderIds(const std::string& source) {
  auto station = toml::parse(source);
  auto* receivers = station["receivers"].as_array();
  if (!receivers) return source;
  std::map<size_t, std::string, std::greater<size_t>> insertions;
  std::vector<size_t> lines{0};
  for (size_t i = 0; i < source.size(); ++i)
    if (source[i] == '\n') lines.push_back(i + 1);
  auto add_ids = [&](toml::table& owner, const char* group) {
    auto* slices = owner[group].as_array();
    if (!slices) return;
    for (auto& node : *slices) {
      auto& slice = *node.as_table();
      auto position = node.source().begin;
      auto offset = lines.at(position.line - 1);
      auto line_end = source.find('\n', offset);
      if (line_end == std::string::npos) line_end = source.size();
      const bool crlf = line_end && source[line_end - 1] == '\r';
      if (slice.is_inline()) {
        // TOML source columns count UTF-8 code points.
        if (!offset && source.compare(0, 3, "\xEF\xBB\xBF") == 0) offset = 3;
        for (size_t column = 1; column < position.column; ++column) {
          ++offset;
          while (offset < line_end &&
                 (static_cast<unsigned char>(source[offset]) & 0xc0) == 0x80)
            ++offset;
        }
        if (offset >= line_end || source[offset] != '{')
          throw std::runtime_error("cannot locate inline slice definition");
        ++offset;
      } else {
        offset = line_end - crlf;
      }
      for (auto key : {"slice_id", "decoder_id"}) {
        if (slice.contains(key)) continue;
        std::random_device random;
        std::ostringstream id;
        if (!slice.is_inline()) id << (crlf ? "\r\n" : "\n");
        id << key << " = \"" << std::hex << random() << '-' << random() << '-'
           << random() << '"';
        if (slice.is_inline()) id << ", ";
        insertions[offset] += id.str();
      }
    }
  };
  for (auto& node : *receivers) {
    auto& receiver = *node.as_table();
    add_ids(receiver, "slices");
    add_ids(receiver, "attached_slices");
    if (auto* schedules = receiver["schedules"].as_array())
      for (auto& schedule : *schedules) add_ids(*schedule.as_table(), "slices");
  }
  auto result = source;
  for (const auto& [offset, fields] : insertions) result.insert(offset, fields);
  return result;
}

void SaveTable(const fs::path& file, const toml::table& table,
               const std::string& expected) {
  std::ostringstream output;
  output << toml::toml_formatter{table} << '\n';
  SaveSettings(file, output.str(), expected);
}

std::string Revision(const std::string& source) {
  unsigned char digest[SHA256_DIGEST_LENGTH];
  SHA256(reinterpret_cast<const unsigned char*>(source.data()), source.size(),
         digest);
  std::string hex;
  for (auto byte : digest) {
    hex += "0123456789abcdef"[byte >> 4];
    hex += "0123456789abcdef"[byte & 15];
  }
  return hex;
}

SmInstanceSettings Parse(const fs::path& file, std::string source) {
  auto table = toml::parse(source, file.string());
  auto revision = Revision(source);
  // Ignore legacy login settings and omit them from the configuration shown
  // in the panel. Preserve the digest of the on-disk file for stale edits.
  if (table.contains("user") || table.contains("password")) {
    table.erase("user");
    table.erase("password");
    std::ostringstream cleaned;
    auto flags = toml::toml_formatter::default_flags &
                 ~toml::format_flags::allow_literal_strings;
    cleaned << toml::toml_formatter{table, flags} << '\n';
    source = cleaned.str();
  }
  auto data = Value(table);
  for (auto it = data.begin(); it != data.end(); ++it) {
    const auto& key = it.key();
    if (key != "listen_address" && key != "port" && key != "first_run_wizard" &&
        key != "database" && key != "gallery" && key != "station" &&
        key != "receivers" && key != "services")
      throw std::runtime_error("unknown TOML setting: " + key);
  }
  if (data.contains("port") && !data["port"].is_number_integer())
    throw std::runtime_error("port must be an integer");
  auto port = data.value("port", int64_t{8080});
  auto address = data.value("listen_address", "127.0.0.1");
  if (port <= 0 || port > 65535 || address.empty())
    throw std::runtime_error("invalid listener settings");
  auto resolve = [&](const char* key, const char* fallback) {
    auto value = data.value(key, fallback);
    if (value.empty())
      throw std::runtime_error(std::string(key) + " cannot be empty");
    return (file.parent_path() / value).lexically_normal().string();
  };
  auto topology = Json{{"receivers", data.value("receivers", Json::array())},
                       {"services", data.value("services", Json::object())}};
  auto service_options = data.value("services", Json::object());
  std::optional<SmStationIdentity> identity;
  if (data.contains("station")) {
    const auto& station = data.at("station");
    identity = SmStationIdentity{
        station.value("callsign", ""), station.value("email", ""),
        station.value("name", ""), station.value("grid", "")};
    if (!std::regex_match(identity->callsign, std::regex("[A-Z0-9/-]{2,16}")))
      throw std::runtime_error("invalid station callsign");
    if (!std::regex_match(identity->grid,
                          std::regex("[A-Ra-r]{2}[0-9]{2}([A-Xa-x]{2})?")))
      throw std::runtime_error("invalid station Maidenhead grid");
    if (!identity->email.empty() &&
        (identity->email.size() > 254 ||
         !std::regex_match(identity->email, std::regex(R"([^@\s]+@[^@\s]+)"))))
      throw std::runtime_error("invalid station email");
    if (identity->name.size() > 128 ||
        identity->name.find_first_of("\r\n") != std::string::npos)
      throw std::runtime_error("invalid station name");
    if (topology["services"].is_object()) {
      for (auto& [feed, options] : topology["services"].items()) {
        if (!options.is_object() || !options.value("enabled", false)) continue;
        if (!options.contains("callsign"))
          options["callsign"] = identity->callsign;
        if (!options.contains("grid")) options["grid"] = identity->grid;
        if (feed == "rbn") {
          if (!options.contains("sign_in") && !identity->email.empty())
            options["sign_in"] = identity->email;
          if (!options.contains("name") && !identity->name.empty())
            options["name"] = identity->name;
        }
      }
    }
  }
  ValidateStation(topology);
  for (const auto& receiver : topology["receivers"]) {
    auto device = receiver.at("device").get<std::string>();
    if (IsFileReceiver(device))
      ResolveReceiverDevice(device, file.parent_path());
  }
  return {address,
          static_cast<int>(port),
          data.value("first_run_wizard", true),
          resolve("database", "station.sqlite"),
          resolve("gallery", "gallery"),
          std::move(identity),
          std::move(topology),
          std::move(service_options),
          std::move(source),
          revision};
}
bool IsPrivateSetting(std::string_view key) {
  return key == "email" || key == "sign_in" || key == "user" ||
         key == "password";
}

void RedactTable(toml::table& table) {
  for (auto&& [key, value] : table) {
    if (IsPrivateSetting(key.str()) && value.is_string())
      value.as_string()->get() = "<redacted>";
    else if (auto* child = value.as_table())
      RedactTable(*child);
    else if (auto* array = value.as_array())
      for (auto& item : *array)
        if (auto* entry = item.as_table()) RedactTable(*entry);
  }
}
}  // namespace

std::string RedactedSettings(const std::string& source) {
  auto table = toml::parse(source);
  RedactTable(table);
  // Schedule names stay; the operator's name does not.
  if (auto* name = table["station"]["name"].as_string())
    name->get() = "<redacted>";
  std::ostringstream out;
  out << toml::toml_formatter{table} << '\n';
  return out.str();
}

nlohmann::json RedactedJson(nlohmann::json value) {
  if (value.is_object()) {
    for (auto it = value.begin(); it != value.end(); ++it)
      if (IsPrivateSetting(it.key()) && it->is_string())
        *it = "<redacted>";
      else
        *it = RedactedJson(std::move(*it));
  } else if (value.is_array()) {
    for (auto& item : value) item = RedactedJson(std::move(item));
  }
  return value;
}

void CreateDefaultSettings(const fs::path& file) {
  // Exclusive creation preserves an existing file even if two servers start
  // together. Open in binary mode to write the same sample on both platforms.
  auto output = std::fopen(file.string().c_str(), "wbx");
  if (!output) {
    if (errno == EEXIST) return;
    throw std::system_error(errno, std::generic_category(),
                            "cannot create " + file.string());
  }
  constexpr auto size = sizeof(kDefaultSettings) - 1;
  auto written = std::fwrite(kDefaultSettings, 1, size, output);
  auto closed = std::fclose(output);
  if (written != size || closed != 0) {
    std::error_code ignored;
    fs::remove(file, ignored);
    throw std::runtime_error("cannot write " + file.string());
  }
}
SmInstanceSettings LoadSettings(const fs::path& file) {
  if (!fs::is_regular_file(file) || fs::file_size(file) > 1024 * 1024)
    throw std::runtime_error("expected a station TOML file of at most 1 MiB");
  std::ifstream input(file, std::ios::binary);
  if (!input) throw std::runtime_error("cannot read station TOML");
  return Parse(file, {std::istreambuf_iterator<char>(input), {}});
}

void SaveSettings(const fs::path& file, const std::string& source,
                  const std::string& expected) {
  auto current = LoadSettings(file);
  if (current.revision != expected)
    throw std::runtime_error("configuration changed; reload and retry");
  auto validated = Parse(file, source);
  auto with_ids = WithDecoderIds(validated.source);
  if (with_ids != validated.source)
    validated = Parse(file, std::move(with_ids));
  // The staging directory is next to the destination so rename is atomic.
  auto staging = file.parent_path() / (".skimmaster-config-" +
                                       std::to_string(std::random_device{}()));
  if (!fs::create_directory(staging))
    throw std::runtime_error("could not stage station TOML");
  try {
    fs::permissions(staging, fs::perms::owner_all, fs::perm_options::replace);
    auto temp = staging / "station.toml";
    std::ofstream output(temp, std::ios::binary);
    output.exceptions(std::ios::badbit | std::ios::failbit);
    output << validated.source;
    output.close();
    fs::permissions(temp, fs::status(file).permissions());
    if (LoadSettings(file).revision != expected)
      throw std::runtime_error("configuration changed; reload and retry");
    fs::rename(temp, file);
  } catch (...) {
    std::error_code ignored;
    fs::remove_all(staging, ignored);
    throw;
  }
  std::error_code ignored;
  fs::remove(staging, ignored);
}

SmInstanceSettings PersistDecoderIds(const fs::path& file,
                                     SmInstanceSettings current) {
  auto with_ids = WithDecoderIds(current.source);
  if (with_ids == current.source) return current;
  SaveSettings(file, with_ids, current.revision);
  return LoadSettings(file);
}

void UpdateReceiver(const fs::path& file, const std::string& expected,
                    const std::string& original,
                    const std::optional<SmReceiverSettings>& receiver) {
  const auto current = LoadSettings(file);
  if (current.revision != expected)
    throw std::runtime_error("configuration changed; reload and retry");
  auto table = toml::parse(current.source);
  if (!table.contains("receivers")) table.insert("receivers", toml::array{});
  auto& receivers = *table["receivers"].as_array();
  auto existing =
      std::find_if(receivers.begin(), receivers.end(), [&](const auto& entry) {
        return original ==
               (*entry.as_table())["receiver"].value_or(std::string{});
      });
  if (!original.empty() && existing == receivers.end())
    throw std::runtime_error("receiver no longer exists");
  if (!receiver) {
    if (existing == receivers.end())
      throw std::runtime_error("select a receiver to delete");
    receivers.erase(existing);
  } else {
    const auto& r = *receiver;
    toml::table added;
    auto& entry = existing == receivers.end() ? added : *existing->as_table();
    const auto& driver = DriverFor(r.device);
    if (r.device.size() == driver.prefix.size())
      throw std::runtime_error("enter a device parameter");
    if ((r.gain || r.ppm) && !driver.adjustable_gain)
      throw std::runtime_error(
          "this source does not support gain or PPM correction");
    if (r.rate < 12000 || r.rate > 6144000)
      throw std::runtime_error(
          "enter a sample rate between 12000 and 6144000 Hz");
    entry.insert_or_assign("receiver", r.name);
    entry.insert_or_assign("device", r.device);
    entry.insert_or_assign("rate", r.rate);
    entry.insert_or_assign("enabled", r.enabled);
    auto set = [&](auto key, const auto& value) {
      if (value)
        entry.insert_or_assign(key, *value);
      else
        entry.erase(key);
    };
    set("center_hz", r.center);
    set("gain_db", r.gain);
    set("ppm", r.ppm);
    if (IsFileReceiver(r.device)) {
      set("utc_ms", r.utc);
      entry.insert_or_assign("repeat", r.repeat);
    } else {
      entry.erase("utc_ms");
      entry.erase("repeat");
    }
    if (!r.mode.empty()) {
      const bool scheduled =
          entry.contains("schedules") && !entry.contains("slices");
      if (!scheduled && !r.center)
        throw std::runtime_error("enter the receiver RF center");
      auto key = scheduled ? "attached_slices" : "slices";
      if (!entry.contains(key)) entry.insert(key, toml::array{});
      entry[key].as_array()->push_back(
          SliceTable({r.mode, IsSkimmer(r.mode) ? r.slice_center : r.dial}));
    }
    if (existing == receivers.end()) {
      receivers.push_back(std::move(added));
    } else if (entry.contains("slices") && !r.center) {
      throw std::runtime_error(
          "enter the receiver RF center for its assigned slices");
    }
  }
  SaveTable(file, table, expected);
}

void UpdateSlice(const fs::path& file, const std::string& expected,
                 const std::string& receiver, const std::string& group,
                 std::optional<size_t> schedule, size_t index,
                 const std::optional<SmSliceSettings>& slice) {
  const auto current = LoadSettings(file);
  if (current.revision != expected)
    throw std::runtime_error("configuration changed; reload and retry");
  if (group != "slices" && group != "attached_slices")
    throw std::runtime_error("invalid slice group");
  auto table = toml::parse(current.source);
  auto& entry = ReceiverTable(table, receiver);
  auto& owner =
      schedule ? IndexedTable(entry["schedules"].as_array(), *schedule) : entry;
  auto* slices = owner[group].as_array();
  auto& existing = IndexedTable(slices, index);
  if (slice) {
    existing.erase("center_hz");
    existing.erase("dial_hz");
    for (const auto& [key, value] : SliceTable(*slice))
      existing.insert_or_assign(key, value);
  } else {
    slices->erase(slices->cbegin() + index);
    if (slices->empty()) owner.erase(group);
  }
  SaveTable(file, table, expected);
}

void UpdateSchedule(const fs::path& file, const std::string& expected,
                    const std::string& receiver, std::optional<size_t> index,
                    bool duplicate,
                    const std::optional<SmScheduleSettings>& schedule) {
  const auto current = LoadSettings(file);
  if (current.revision != expected)
    throw std::runtime_error("configuration changed; reload and retry");
  auto table = toml::parse(current.source);
  auto& entry = ReceiverTable(table, receiver);
  if (!entry.contains("schedules")) entry.insert("schedules", toml::array{});
  auto* schedules = entry["schedules"].as_array();
  toml::table added;
  auto& target = index ? IndexedTable(schedules, *index) : added;
  if (duplicate) {
    if (!index) throw std::runtime_error("select a schedule to duplicate");
    added = target;
    if (auto* slices = added["slices"].as_array()) {
      for (auto& slice : *slices) {
        slice.as_table()->erase("slice_id");
        slice.as_table()->erase("decoder_id");
      }
    }
    const auto name = added["name"].value_or(std::string("Schedule"));
    added.insert_or_assign("name", name + " (copy)");
    schedules->push_back(std::move(added));
  } else if (schedule) {
    const auto& s = *schedule;
    if (s.name.empty())
      target.erase("name");
    else
      target.insert_or_assign("name", s.name);
    target.insert_or_assign("start_minute", s.start_minute);
    target.insert_or_assign("end_minute", s.end_minute);
    target.insert_or_assign("priority", s.priority);
    target.insert_or_assign("center_hz", s.center_hz);
    target.insert_or_assign("days", s.days);
    target.insert_or_assign("enabled", s.enabled);
    if (s.slice) {
      if (!target.contains("slices")) target.insert("slices", toml::array{});
      target["slices"].as_array()->push_back(SliceTable(*s.slice));
    }
    if (!index) schedules->push_back(std::move(added));
  } else {
    if (!index) throw std::runtime_error("select a schedule to delete");
    schedules->erase(schedules->cbegin() + *index);
    if (schedules->empty()) entry.erase("schedules");
  }
  SaveTable(file, table, expected);
}

void UpdateReportingService(const fs::path& file, const std::string& expected,
                            const SmReportingServiceSettings& service) {
  static const std::set<std::string> names{"cluster", "psk", "rbn", "wsprnet"};
  if (!names.count(service.service))
    throw std::runtime_error("unknown reporting service");
  const auto current = LoadSettings(file);
  if (current.revision != expected)
    throw std::runtime_error("configuration changed; reload and retry");
  auto table = toml::parse(current.source);
  if (!table.contains("services")) table.insert("services", toml::table{});
  auto* services = table["services"].as_table();
  if (!services) throw std::runtime_error("services must be a table");
  if (!services->contains(service.service))
    services->insert(service.service, toml::table{});
  auto* options = (*services)[service.service].as_table();
  if (!options)
    throw std::runtime_error("reporting service options must be a table");
  options->insert_or_assign("enabled", service.enabled);
  auto set_string = [&](const char* key,
                        const std::optional<std::string>& value) {
    if (value && !value->empty())
      options->insert_or_assign(key, *value);
    else
      options->erase(key);
  };
  auto set_integer = [&](const char* key, const std::optional<int>& value) {
    if (value)
      options->insert_or_assign(key, *value);
    else
      options->erase(key);
  };
  set_string("callsign", service.callsign);
  set_string("grid", service.grid);
  set_string("host", service.host);
  set_string("endpoint", service.endpoint);
  set_string("sign_in", service.sign_in);
  set_string("band_limits", service.band_limits);
  set_integer("port", service.port);
  set_integer("backup_port", service.backup_port);
  SaveTable(file, table, expected);
}

void SaveWizard(const fs::path& file, const std::string& expected,
                const SmStationIdentity& identity, const std::string& receiver,
                const std::string& device, int rate,
                std::optional<int64_t> dial, const std::string& mode,
                std::optional<int64_t> center, std::optional<int64_t> utc,
                bool repeat) {
  auto current = LoadSettings(file);
  auto table = toml::parse(current.source);
  table.insert_or_assign("station", toml::table{{"callsign", identity.callsign},
                                                {"email", identity.email},
                                                {"name", identity.name},
                                                {"grid", identity.grid}});
  if (!table.contains("receivers")) table.insert("receivers", toml::array{});
  toml::table slice{{"mode", mode}};
  if (IsSkimmer(mode)) {
    if (!center)
      throw std::runtime_error(
          "Enter the receiver RF center in IQ Hardware Source.");
    slice.insert("center_hz", *center);
  } else {
    if (!dial) throw std::runtime_error("Enter the decoder dial frequency.");
    slice.insert("dial_hz", *dial);
  }
  const auto receiver_center =
      center.value_or(dial.value_or(0) + (IsTimedDecoder(mode) ? 1500 : 0));
  toml::table definition{{"receiver", receiver},
                         {"device", device},
                         {"rate", rate},
                         {"center_hz", receiver_center},
                         {"slices", toml::array{std::move(slice)}}};
  if (utc) definition.insert("utc_ms", *utc);
  if (IsFileReceiver(device)) definition.insert("repeat", repeat);
  table["receivers"].as_array()->push_back(std::move(definition));
  table.insert_or_assign("first_run_wizard", false);
  std::ostringstream output;
  output << toml::toml_formatter{table} << '\n';
  SaveSettings(file, output.str(), expected);
}
}  // namespace skim
