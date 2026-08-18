/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include "pipewire_latency.h"

#include <spa/param/latency-utils.h>
#include <spa/pod/builder.h>

#include <array>
#include <cstdint>
#include <iostream>
#include <string_view>

static bool check(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
  }
  return condition;
}

static bool testProcessLatencyUsesNegotiatedStreamFrames() {
  const auto bypass = pipetune::calculatePipeWireProcessLatency(
      0, 48000, 0, 48000);
  const auto converted = pipetune::calculatePipeWireProcessLatency(
      191, 192000, 17, 48000);
  const auto fractional = pipetune::calculatePipeWireProcessLatency(
      1, 192000, 0, 48000);
  const auto missingRate = pipetune::calculatePipeWireProcessLatency(
      64, 48000, 0, 0);
  const auto overflow = pipetune::calculatePipeWireProcessLatency(
      UINT32_MAX, 1, UINT32_MAX, UINT32_MAX);
  return check(bypass.valid && bypass.frames == 0,
               "bypass must publish zero processing latency") &&
         check(converted.valid && converted.frames == 65,
               "DSP and bridge delay must share the stream frame domain") &&
         check(fractional.valid && fractional.frames == 1,
               "fractional DSP delay must round up") &&
         check(!missingRate.valid,
               "an unnegotiated stream rate must not publish latency") &&
         check(!overflow.valid,
               "unrepresentable processing latency must be rejected");
}

static bool testPortLatencyPropagatesThroughProcessingDelay() {
  auto state = pipetune::PipeWireLatencyState{};
  state.setProcessLatencyFrames(64);

  auto outputInfo = spa_latency_info{};
  outputInfo.direction = SPA_DIRECTION_OUTPUT;
  outputInfo.min_quantum = 1.0F;
  outputInfo.max_quantum = 2.0F;
  outputInfo.min_rate = 100;
  outputInfo.max_rate = 120;
  outputInfo.min_ns = 300;
  outputInfo.max_ns = 400;
  auto outputStorage = std::array<std::uint8_t, 512>{};
  auto outputBuilder = spa_pod_builder{};
  spa_pod_builder_init(&outputBuilder, outputStorage.data(),
                       outputStorage.size());
  const auto *outputParameter = spa_latency_build(
      &outputBuilder, SPA_PARAM_Latency, &outputInfo);
  const auto outputUpdate = state.updatePortLatency(outputParameter);
  const auto output = state.propagatedLatency(SPA_DIRECTION_OUTPUT);
  if (!check(outputUpdate.valid &&
                 outputUpdate.direction == SPA_DIRECTION_OUTPUT,
             "output Latency parameter must be accepted") ||
      !check(output.direction == SPA_DIRECTION_OUTPUT &&
                 output.min_quantum == 1.0F &&
                 output.max_quantum == 2.0F && output.min_rate == 164 &&
                 output.max_rate == 184 && output.min_ns == 300 &&
                 output.max_ns == 400,
             "output latency must retain every component and add frames")) {
    return false;
  }

  auto inputInfo = spa_latency_info{};
  inputInfo.direction = SPA_DIRECTION_INPUT;
  inputInfo.min_rate = 200;
  inputInfo.max_rate = 240;
  auto inputStorage = std::array<std::uint8_t, 512>{};
  auto inputBuilder = spa_pod_builder{};
  spa_pod_builder_init(&inputBuilder, inputStorage.data(),
                       inputStorage.size());
  const auto *inputParameter = spa_latency_build(
      &inputBuilder, SPA_PARAM_Latency, &inputInfo);
  const auto inputUpdate = state.updatePortLatency(inputParameter);
  const auto input = state.propagatedLatency(SPA_DIRECTION_INPUT);
  return check(inputUpdate.valid &&
                   inputUpdate.direction == SPA_DIRECTION_INPUT,
               "input Latency parameter must be accepted") &&
         check(input.direction == SPA_DIRECTION_INPUT &&
                   input.min_rate == 264 && input.max_rate == 304,
               "input latency must propagate upstream with processing delay") &&
         check(state.propagatedLatency(
                   static_cast<spa_direction>(99)).direction ==
                   SPA_DIRECTION_OUTPUT,
               "invalid latency directions must return a safe value");
}

static bool testProcessLatencyOwnershipTracksDynamicChanges() {
  auto state = pipetune::PipeWireLatencyState{};
  state.setProcessLatencyFrames(80);
  const auto first = state.processLatency();

  auto storage = std::array<std::uint8_t, 256>{};
  auto builder = spa_pod_builder{};
  spa_pod_builder_init(&builder, storage.data(), storage.size());
  const auto *matching = spa_process_latency_build(
      &builder, SPA_PARAM_ProcessLatency, &first);
  if (!check(first.quantum == 0.0F && first.rate == 80 && first.ns == 0,
             "ProcessLatency must use only stream-rate frames") ||
      !check(state.processLatencyMatches(matching),
             "published ProcessLatency must be recognized")) {
    return false;
  }

  state.setProcessLatencyFrames(0);
  const auto bypass = state.processLatency();
  return check(bypass.rate == 0,
               "dynamic bypass must clear ProcessLatency") &&
         check(!state.processLatencyMatches(matching),
               "a stale ProcessLatency value must be rejected") &&
         check(!state.processLatencyMatches(nullptr),
               "a null ProcessLatency parameter must not match");
}

int main() {
  return testProcessLatencyUsesNegotiatedStreamFrames() &&
                 testPortLatencyPropagatesThroughProcessingDelay() &&
                 testProcessLatencyOwnershipTracksDynamicChanges()
             ? 0
             : 1;
}
