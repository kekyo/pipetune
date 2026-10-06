/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include "wireplumber_04_compat.h"
#include "wireplumber_visibility.h"

#include <iostream>

int main(int argc, char **argv) {
  auto policy = pipetune::wirePlumber04EndpointClientPolicy();
  if (argc == 2) {
    const auto part = std::string_view{argv[1]};
    if (part == "configuration") policy = pipetune::wirePlumber04CompatibilityPolicy();
    else if (part == "stream-configuration") policy = pipetune::wirePlumber04StreamConfiguration();
    else if (part == "endpoint-client") policy = pipetune::wirePlumber04EndpointClientPolicy();
    else if (part == "endpoint-device") policy = pipetune::wirePlumber04EndpointDevicePolicy();
    else if (part == "visibility") policy = pipetune::wirePlumberNodeVisibilityPolicy();
    else if (part == "visibility-configuration") policy = pipetune::wirePlumber05NodeVisibilityConfiguration();
    else return 2;
  } else if (argc != 1) return 2;
  std::cout.write(policy.data(), static_cast<std::streamsize>(policy.size()));
  return std::cout.good() ? 0 : 1;
}
