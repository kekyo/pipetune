#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string_view>

int main(int argc, char **argv) {
  if (argc != 3 || std::string_view(argv[1]) != "setup" ||
      std::string_view(argv[2]) != "--no-launch-gtk") {
    std::cerr << "user setup arguments differ\n";
    return 64;
  }
  const auto *recordPath =
      std::getenv("PIPETUNE_GTK_SETUP_HELPER_RECORD");
  if (recordPath != nullptr && recordPath[0] != '\0') {
    auto record = std::ofstream(recordPath, std::ios::app);
    if (!record) {
      std::cerr << "cannot open user setup invocation record\n";
      return 74;
    }
    record << "setup --no-launch-gtk\n";
    if (!record) {
      std::cerr << "cannot write user setup invocation record\n";
      return 74;
    }
  }
  if (std::getenv("PIPETUNE_GTK_SETUP_HELPER_FAIL") != nullptr) {
    std::cerr << "simulated user setup failure\n";
    return 7;
  }
  std::cout << "user setup helper succeeded\n";
  return 0;
}
