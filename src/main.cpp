#include "pipetune/version.h"

#include <iostream>
#include <string_view>

int main(int argc, char **argv) {
  if (argc == 2 && std::string_view(argv[1]) == "--version") {
    std::cout << "pipetune " << pipetune::version() << '\n';
    return 0;
  }
  std::cout << "Usage: pipetune [--version]\n";
  return 0;
}
