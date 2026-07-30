#include "dsp_idle_controller.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <span>
#include <string_view>

static bool check(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
  }
  return condition;
}

static bool processQuietBlock(pipetune::DspIdleController &controller,
                              std::span<const float> input,
                              std::span<const float> output,
                              std::uint32_t channelCount,
                              std::uint32_t frameCount) {
  return check(controller.observeInput(input, channelCount, frameCount),
               "draining input must continue DSP processing") &&
         controller.observeOutput(output, channelCount, frameCount, true);
}

static bool testConservativePolicySleepsAfterFixedDurations() {
  auto controller = pipetune::DspIdleController(
      10, pipetune::DspIdlePolicy::conservative);
  const auto input = std::array<float, 10>{};
  auto quietOutput = std::array<float, 10>{};
  quietOutput.fill(1.0e-9F);

  for (auto block = std::uint32_t{0}; block < 4; ++block) {
    if (!check(!processQuietBlock(controller, input, quietOutput, 1, 10),
               "conservative policy slept before five seconds")) {
      return false;
    }
  }
  if (!check(processQuietBlock(controller, input, quietOutput, 1, 10),
             "conservative policy did not request sleep at five seconds") ||
      !check(controller.state() == pipetune::DspIdleState::draining,
             "eligible processing must remain draining until reset succeeds")) {
    return false;
  }
  controller.enterSleep();
  return check(controller.state() == pipetune::DspIdleState::sleeping,
               "successful reset must enter sleep") &&
         check(controller.sleepTransitions() == 1,
               "sleep transition counter differs") &&
         check(!controller.observeInput(input, 1, 10),
               "zero input must skip DSP while sleeping") &&
         check(controller.skippedFrames() == 10,
               "skipped frame counter differs");
}

static bool testExactPolicyRejectsSubthresholdNonzeroTail() {
  auto controller =
      pipetune::DspIdleController(10, pipetune::DspIdlePolicy::exact);
  const auto input = std::array<float, 10>{};
  auto nonzeroOutput = std::array<float, 10>{};
  nonzeroOutput.fill(1.0e-9F);
  for (auto block = std::uint32_t{0}; block < 5; ++block) {
    if (!check(!processQuietBlock(controller, input, nonzeroOutput, 1, 10),
               "exact policy accepted a nonzero tail")) {
      return false;
    }
  }

  const auto zeroOutput = std::array<float, 10>{};
  return check(processQuietBlock(controller, input, zeroOutput, 1, 10),
               "exact policy did not accept one second of exact zero");
}

static bool testNonzeroInputWakesTheSameBlock() {
  auto controller =
      pipetune::DspIdleController(10, pipetune::DspIdlePolicy::exact);
  const auto zero = std::array<float, 10>{};
  for (auto block = std::uint32_t{0}; block < 5; ++block) {
    static_cast<void>(controller.observeInput(zero, 1, 10));
    if (controller.observeOutput(zero, 1, 10, true)) {
      controller.enterSleep();
    }
  }
  if (!check(controller.state() == pipetune::DspIdleState::sleeping,
             "wake test did not reach sleep")) {
    return false;
  }

  const auto input =
      std::array<float, 8>{0.0F, 0.0F, 0.0F, 0.0F,
                          0.0F, 1.0F, 0.0F, 0.0F};
  return check(controller.observeInput(input, 2, 4),
               "nonzero block must be processed immediately") &&
         check(controller.state() == pipetune::DspIdleState::draining,
               "trailing zero frames must start draining") &&
         check(controller.inputSilentFrames() == 2,
               "trailing exact-zero frame count differs");
}

static bool testNonfiniteValuesAreNeverQuiet() {
  auto controller = pipetune::DspIdleController(
      1, pipetune::DspIdlePolicy::conservative);
  const auto zero = std::array<float, 1>{0.0F};
  for (auto second = std::uint32_t{0}; second < 5; ++second) {
    static_cast<void>(controller.observeInput(zero, 1, 1));
    static_cast<void>(controller.observeOutput(zero, 1, 1, true));
  }
  const auto nan =
      std::array<float, 1>{std::numeric_limits<float>::quiet_NaN()};
  const auto infinity =
      std::array<float, 1>{std::numeric_limits<float>::infinity()};
  return check(!controller.observeOutput(nan, 1, 1, true),
               "NaN must reset output quiet time") &&
         check(controller.outputQuietFrames() == 0,
               "NaN did not reset output quiet frames") &&
         check(controller.observeInput(infinity, 1, 1),
               "infinite input must be processed") &&
         check(controller.state() == pipetune::DspIdleState::active,
               "infinite input must return the controller to active");
}

static bool testResetFailureIsLatchedUntilNewActivity() {
  auto controller =
      pipetune::DspIdleController(1, pipetune::DspIdlePolicy::exact);
  const auto zero = std::array<float, 1>{0.0F};
  for (auto second = std::uint32_t{0}; second < 5; ++second) {
    static_cast<void>(controller.observeInput(zero, 1, 1));
    static_cast<void>(controller.observeOutput(zero, 1, 1, true));
  }
  if (!check(controller.observeOutput(zero, 1, 1, true),
             "eligible controller did not request reset")) {
    return false;
  }
  controller.rejectSleep();
  if (!check(!controller.observeOutput(zero, 1, 1, true),
             "failed reset must not be retried in the same idle interval")) {
    return false;
  }

  const auto nonzero = std::array<float, 1>{1.0F};
  static_cast<void>(controller.observeInput(nonzero, 1, 1));
  for (auto second = std::uint32_t{0}; second < 5; ++second) {
    static_cast<void>(controller.observeInput(zero, 1, 1));
    static_cast<void>(controller.observeOutput(zero, 1, 1, true));
  }
  return check(controller.observeOutput(zero, 1, 1, true),
               "new input activity must allow another reset attempt");
}

int main() {
  return testConservativePolicySleepsAfterFixedDurations() &&
                 testExactPolicyRejectsSubthresholdNonzeroTail() &&
                 testNonzeroInputWakesTheSameBlock() &&
                 testNonfiniteValuesAreNeverQuiet() &&
                 testResetFailureIsLatchedUntilNewActivity()
             ? 0
             : 1;
}
