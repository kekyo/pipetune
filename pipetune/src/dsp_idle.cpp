#include "pipetune/dsp_idle.h"

namespace pipetune {

DspIdlePolicy defaultDspIdlePolicy() noexcept {
  return DspIdlePolicy::conservative;
}

std::string_view dspIdlePolicyName(DspIdlePolicy policy) noexcept {
  switch (policy) {
  case DspIdlePolicy::conservative:
    return "conservative";
  case DspIdlePolicy::exact:
    return "exact";
  }
  return {};
}

std::optional<DspIdlePolicy>
parseDspIdlePolicyName(std::string_view name) noexcept {
  if (name == "conservative") {
    return DspIdlePolicy::conservative;
  }
  if (name == "exact") {
    return DspIdlePolicy::exact;
  }
  return std::nullopt;
}

std::string_view dspIdleStateName(DspIdleState state) noexcept {
  switch (state) {
  case DspIdleState::active:
    return "active";
  case DspIdleState::draining:
    return "draining";
  case DspIdleState::sleeping:
    return "sleeping";
  }
  return {};
}

} // namespace pipetune
