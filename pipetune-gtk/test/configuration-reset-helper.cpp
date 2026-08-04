#include "pipetune/startup_config.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string_view>

int main(int argc, char **argv) {
  if (argc != 4 || std::string_view(argv[1]) != "config" ||
      std::string_view(argv[2]) != "reset" ||
      std::string_view(argv[3]) != "--yes") {
    std::cerr << "configuration reset arguments differ\n";
    return 64;
  }
  if (std::getenv("PIPETUNE_GTK_RESET_HELPER_FAIL") != nullptr) {
    std::cerr << "simulated partial reset failure\n";
    return 7;
  }
  const auto *configPath =
      std::getenv("PIPETUNE_GTK_RESET_HELPER_CONFIG");
  if (configPath != nullptr && configPath[0] != '\0') {
    const auto error = pipetune::saveStartupConfig(
        std::filesystem::path(configPath), pipetune::StartupConfig{});
    if (!error.empty()) {
      std::cerr << error << '\n';
      return 1;
    }
  }
  std::cout << "configuration reset helper succeeded\n";
  return 0;
}
