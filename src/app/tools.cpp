#include <charconv>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>

#include "app/backup.h"
#include "app/live.h"
#include "app/replay.h"
#include "app/station.h"
#include "app/wideband.h"
#include "decoder/slot.h"
#include "receiver/receiver.h"
#include "report/cluster.h"
#include "report/reporting.h"
#include "store/store.h"
#include "web/web.h"

namespace {
constexpr char usage[] = R"(SkimMasterTools commands
  --wideband RECEIVER_JSON
  --receive MODE DEVICE RF_HZ RECEIVER DB [FIRST_UTC_MS]
  --sstv DEVICE DIAL_HZ RATE GALLERY
  --decode FT8|FT4|WSPR AUDIO DIAL_HZ UTC_MS RECEIVER DB
  --replay IQ_FILE [RECEIVER_HZ SLICE_HZ]
  --cluster DB CALL GRID PORT
  --backup DB GALLERY NEW_ZIP
  --restore ZIP DB GALLERY
  --report rbn|psk|wsprnet DB REPORTER_JSON
)";
int64_t Number(std::string_view field,
               int64_t maximum = std::numeric_limits<int64_t>::max()) {
  int64_t value = 0;
  auto end = field.data() + field.size();
  auto parsed = std::from_chars(field.data(), end, value);
  if (parsed.ec != std::errc{} || parsed.ptr != end || value < 0 ||
      value > maximum)
    throw std::runtime_error("invalid nonnegative integer: " +
                             std::string(field));
  return value;
}
int Port(const char* arg) {
  auto value = Number(arg, 65535);
  if (!value) throw std::runtime_error("port must be positive");
  return value;
}
int Run(int argc, char** argv) {
  if (argc == 1) {
    std::cout << usage;
    return 0;
  }
  std::string_view cmd = argv[1];
  if (cmd == "--help" && argc == 2)
    std::cout << usage;
  else if (cmd == "--station-worker" && argc == 4)
    skim::RunStationWorker(argv[2], argv[3]);
  else if (cmd == "--test-receiver" && argc == 3) {
    const auto settings = nlohmann::json::parse(argv[2]);
    struct SmReceivedIq {};
    try {
      const auto device = settings.at("device").get<std::string>();
      if (device.empty() || device == "-")
        throw std::runtime_error("invalid receiver device");
      skim::ReadReceiver(
          device.c_str(), settings.at("center_hz"), settings.at("rate"),
          settings.value("gain_db", NAN), settings.value("ppm", NAN),
          [](const auto*, size_t count) {
            if (count) throw SmReceivedIq{};
          },
          [] {});
    } catch (const SmReceivedIq&) {
      return 0;
    }
    throw std::runtime_error("receiver ended without IQ samples");
  } else if (cmd == "--backup" && argc == 5)
    skim::BackupStation(argv[2], argv[3], argv[4]);
  else if (cmd == "--restore" && argc == 5)
    skim::RestoreStation(argv[2], argv[3], argv[4]);
  else if (cmd == "--wideband" && argc == 3)
    skim::Wideband(argv[2]);
  else if (cmd == "--serve-config" && argc == 3)
    skim::Serve(argv[2]);
  else if (cmd == "--station-panel" && argc == 7)
    skim::Serve(argv[2], argv[3], Port(argv[4]), argv[5], argv[6]);
  else if (cmd == "--report" && argc == 5)
    skim::RunReporter(argv[2], argv[3], argv[4]);
  else if (cmd == "--cluster" && argc == 6)
    skim::ServeCluster(argv[2], argv[3], argv[4], Port(argv[5]));
  else if (cmd == "--sstv" && argc == 6)
    skim::ReceiveSstv(argv[2], Number(argv[3]), Number(argv[4], 384000),
                      argv[5]);
  else if (cmd == "--receive" && (argc == 7 || argc == 8)) {
    if (!*argv[5]) throw std::runtime_error("empty receiver identifier");
    skim::ReceiveMode(argv[2], argv[3], Number(argv[4], 1000000000000LL),
                      argv[5], argv[6],
                      argc == 8 ? Number(argv[7], 100000000000000LL) : -1);
  } else if (cmd == "--decode" && argc == 8) {
    auto dial = Number(argv[4], 1000000000000LL);
    auto utc = Number(argv[5]);
    if (utc % (skim::SlotSamples(argv[2]) / 12) || !*argv[6])
      throw std::runtime_error("invalid UTC slot boundary or receiver ID");
    auto spots = skim::DecodeSlot(argv[2], argv[3], dial, utc);
    skim::SaveSlot(argv[7], argv[6], argv[2], utc, spots);
    for (const auto& s : spots)
      std::cout << s.frequency << ' ' << s.message << '\n';
  } else if (cmd == "--replay" && (argc == 3 || argc == 5)) {
    std::ifstream input(argv[2]);
    if (!input) throw std::runtime_error("cannot open replay file");
    if (argc == 5)
      skim::RunReplay(input, std::cout, Number(argv[3]), Number(argv[4]));
    else
      skim::RunReplay(input, std::cout);
  } else {
    std::cerr << usage;
    return 1;
  }
  return 0;
}
}  // namespace
int main(int argc, char** argv) {
  try {
    return Run(argc, argv);
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
