/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include "wireplumber_04_compat.h"

#include <iostream>

int main() {
  const auto policy = pipetune::wirePlumber04EndpointClientPolicy();
  std::cout.write(policy.data(), static_cast<std::streamsize>(policy.size()));
  return std::cout.good() ? 0 : 1;
}
