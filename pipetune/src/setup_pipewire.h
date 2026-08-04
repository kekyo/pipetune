#ifndef PIPETUNE_SETUP_PIPEWIRE_H
#define PIPETUNE_SETUP_PIPEWIRE_H

#include <string>

namespace pipetune {

/**
 * Reports setup-time PipeWire and WirePlumber integration preparation.
 */
struct SetupPipeWireIntegrationResult {
  /** Active compatible WirePlumber policy backend, or empty on failure. */
  std::string policyBackend;
  /** True when an obsolete PipeTune virtual-sink selection was removed. */
  bool legacyDefaultCleared;
  /** Preparation diagnostic, or empty on success. */
  std::string error;
};

/**
 * Waits for the PipeTune WirePlumber policy and removes obsolete defaults.
 *
 * Only default metadata values that resolve exactly to the retired
 * `pipetune_sink` virtual device are removed. Other user selections are
 * preserved.
 *
 * @return Compatible policy backend, migration state, and any diagnostic.
 */
SetupPipeWireIntegrationResult prepareSetupPipeWireIntegration();

} // namespace pipetune

#endif
