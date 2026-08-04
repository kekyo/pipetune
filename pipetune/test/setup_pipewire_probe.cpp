#include "setup_pipewire.h"

#include <iostream>

int main() {
  const auto result = pipetune::prepareSetupPipeWireIntegration();
  if (!result.error.empty()) {
    std::cerr << result.error << '\n';
    return 1;
  }
  std::cout << "policyBackend=" << result.policyBackend << '\n'
            << "legacyDefaultCleared="
            << (result.legacyDefaultCleared ? "true" : "false") << '\n';
  return 0;
}
