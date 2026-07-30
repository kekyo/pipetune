#include <pffft.h>

#include <iostream>

int main() {
  if (pffft_simd_size() != 1) {
    std::cerr << "PipeTune must match EffeTune's scalar PFFFT configuration\n";
    return 1;
  }
  return 0;
}
