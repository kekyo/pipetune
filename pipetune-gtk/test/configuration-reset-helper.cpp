/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include <cstdlib>
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
  std::cout << "configuration reset helper succeeded\n";
  return 0;
}
