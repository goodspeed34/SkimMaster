#include <filesystem>
#include <iostream>

#include "app/backup.h"
#include "app/config.h"
#include "app/instance.h"

int main(int argc, char** argv) {
  if (argc > 2 ||
      (argc == 2 && std::filesystem::path(argv[1]).extension() != ".toml")) {
    std::cerr << "Usage: SkimMaster [STATION.toml]\n";
    return 1;
  }
  try {
    const std::filesystem::path config = argc == 2 ? argv[1] : "config.toml";
    if (argc == 1) skim::CreateDefaultSettings(config);
    const auto station = std::filesystem::canonical(config);
    do {
      // A restore or reset staged by the panel runs before the station
      // takes its lock again.
      skim::ApplyPendingMaintenance(station);
    } while (skim::SmInstance::Run(station) == skim::SmRunOutcome::Restart);
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
