#include "pipetune/dsp_idle.h"

#include <iostream>
#include <string_view>

static bool check(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
  }
  return condition;
}

int main() {
  const auto conservative =
      pipetune::parseDspIdlePolicyName("conservative");
  const auto exact = pipetune::parseDspIdlePolicyName("exact");
  return check(pipetune::defaultDspIdlePolicy() ==
                   pipetune::DspIdlePolicy::conservative,
               "DSP idle policy default must be conservative") &&
         check(conservative == pipetune::DspIdlePolicy::conservative &&
                   exact == pipetune::DspIdlePolicy::exact,
               "DSP idle policy names must round-trip") &&
         check(!pipetune::parseDspIdlePolicyName("threshold").has_value(),
               "unknown DSP idle policies must be rejected") &&
         check(pipetune::dspIdlePolicyName(
                   static_cast<pipetune::DspIdlePolicy>(99))
                   .empty(),
               "invalid DSP idle policies must not have names") &&
         check(pipetune::dspIdleStateName(
                   pipetune::DspIdleState::active) == "active" &&
                   pipetune::dspIdleStateName(
                       pipetune::DspIdleState::draining) == "draining" &&
                   pipetune::dspIdleStateName(
                       pipetune::DspIdleState::sleeping) == "sleeping",
               "DSP idle state names differ") &&
         check(pipetune::dspIdleStateName(
                   static_cast<pipetune::DspIdleState>(99))
                   .empty(),
               "invalid DSP idle states must not have names")
             ? 0
             : 1;
}
