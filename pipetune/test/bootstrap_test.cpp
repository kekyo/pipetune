#include "pipetune/version.h"

#include <pffft.h>

#include <cstddef>
#include <iostream>
#include <string_view>

#if defined(__GNUC__) && !defined(_WIN32)
extern "C" void *__real_malloc(std::size_t size);
extern "C" void *__wrap_malloc(std::size_t size) {
  return __real_malloc(size);
}
#endif

int main() {
  if (pipetune::version() != std::string_view("0.1.0")) {
    std::cerr << "PipeTune must report version 0.1.0\n";
    return 1;
  }
  if (pffft_simd_size() != 1) {
    std::cerr << "PipeTune must match EffeTune's scalar PFFFT configuration\n";
    return 1;
  }
  return 0;
}
