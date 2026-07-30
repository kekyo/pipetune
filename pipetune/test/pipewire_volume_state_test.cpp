#include "pipewire_volume_state.h"

#include <spa/param/props.h>
#include <spa/pod/builder.h>
#include <spa/pod/iter.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <string_view>

static bool check(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
  }
  return condition;
}

static bool near(float actual, float expected) {
  return std::abs(actual - expected) < 0.00001F;
}

static bool testEffectiveVolumesAreNotMultipliedByMasterOrSoftVolume() {
  auto state = pipetune::PipeWireVolumeState{};
  auto effective = std::array{0.25F, 0.5F};
  auto software = std::array{0.5F, 0.5F};
  auto unusedPosition = std::uint32_t{SPA_AUDIO_CHANNEL_UNKNOWN};
  auto storage = std::array<std::uint8_t, 512>{};
  auto builder = spa_pod_builder{};
  spa_pod_builder_init(&builder, storage.data(), storage.size());
  const auto *parameter = static_cast<const spa_pod *>(
      spa_pod_builder_add_object(
          &builder, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props,
          SPA_PROP_volume, SPA_POD_Float(0.5F),
          SPA_PROP_channelVolumes,
          SPA_POD_Array(sizeof(float), SPA_TYPE_Float,
                        static_cast<int>(effective.size()),
                        effective.data()),
          SPA_PROP_softVolumes,
          SPA_POD_Array(sizeof(float), SPA_TYPE_Float,
                        static_cast<int>(software.size()),
                        software.data()),
          SPA_PROP_channelMap,
          SPA_POD_Array(sizeof(std::uint32_t), SPA_TYPE_Id, 0,
                        &unusedPosition)));

  const auto merged =
      pipetune::mergePipeWireVolumeState(parameter, state);
  return check(merged.valid && merged.changed &&
                   merged.volumePresent,
               "effective channel volumes must be accepted") &&
         check(state.channelCount == 2 &&
                   state.channelMapCount == 0 &&
                   near(state.channelVolumes[0], 0.25F) &&
                   near(state.channelVolumes[1], 0.5F),
               "channelVolumes must remain the unmultiplied effective gain");
}

static bool testPartialMutePreservesVolume() {
  auto state = pipetune::PipeWireVolumeState{};
  state.channelCount = 2;
  state.channelVolumes[0] = 0.2F;
  state.channelVolumes[1] = 0.4F;
  auto storage = std::array<std::uint8_t, 128>{};
  auto builder = spa_pod_builder{};
  spa_pod_builder_init(&builder, storage.data(), storage.size());
  const auto *parameter = static_cast<const spa_pod *>(
      spa_pod_builder_add_object(
          &builder, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props,
          SPA_PROP_mute, SPA_POD_Bool(true)));

  const auto merged =
      pipetune::mergePipeWireVolumeState(parameter, state);
  return check(merged.valid && merged.changed &&
                   !merged.volumePresent,
               "partial mute update must be accepted") &&
         check(state.channelCount == 2 &&
                   near(state.channelVolumes[0], 0.2F) &&
                   near(state.channelVolumes[1], 0.4F) &&
                   state.muteKnown && state.muted,
               "partial mute update must preserve effective volumes");
}

static bool testInvalidVolumeDoesNotModifyState() {
  auto state = pipetune::PipeWireVolumeState{};
  state.channelCount = 1;
  state.channelVolumes[0] = 0.75F;
  auto volumes = std::array{-1.0F};
  auto storage = std::array<std::uint8_t, 256>{};
  auto builder = spa_pod_builder{};
  spa_pod_builder_init(&builder, storage.data(), storage.size());
  const auto *parameter = static_cast<const spa_pod *>(
      spa_pod_builder_add_object(
          &builder, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props,
          SPA_PROP_channelVolumes,
          SPA_POD_Array(sizeof(float), SPA_TYPE_Float,
                        static_cast<int>(volumes.size()),
                        volumes.data())));

  const auto merged =
      pipetune::mergePipeWireVolumeState(parameter, state);
  return check(!merged.valid && !merged.changed,
               "negative effective volume must be rejected") &&
         check(state.channelCount == 1 &&
                   near(state.channelVolumes[0], 0.75F),
               "rejected volume must leave state unchanged");
}

static pipetune::PipeWireVolumeState makeSurroundState() {
  auto state = pipetune::PipeWireVolumeState{};
  state.channelCount = 6;
  state.channelMapCount = 6;
  state.channelVolumes = {0.2F, 0.4F, 0.3F, 0.1F, 0.16F, 0.32F};
  state.channelMap = {
      SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR,
      SPA_AUDIO_CHANNEL_FC, SPA_AUDIO_CHANNEL_LFE,
      SPA_AUDIO_CHANNEL_RL, SPA_AUDIO_CHANNEL_RR};
  state.muteKnown = true;
  return state;
}

static bool testStereoRequestPreservesSurroundBalance() {
  const auto physical = makeSurroundState();
  const auto stereoMap =
      std::array<std::uint32_t, 2>{SPA_AUDIO_CHANNEL_FL,
                                  SPA_AUDIO_CHANNEL_FR};
  const auto previous = pipetune::mapPhysicalVolumeToVirtual(
      physical, stereoMap);
  auto requested = previous;
  requested.channelVolumes[0] = 0.4F;
  requested.channelVolumes[1] = 0.8F;
  const auto mapped = pipetune::mapVirtualVolumeToPhysical(
      requested, previous, physical);

  return check(near(mapped.channelVolumes[0], 0.4F) &&
                   near(mapped.channelVolumes[1], 0.8F),
               "matching stereo channels must use requested values") &&
         check(near(mapped.channelVolumes[2], 0.6F) &&
                   near(mapped.channelVolumes[3], 0.2F) &&
                   near(mapped.channelVolumes[4], 0.32F) &&
                   near(mapped.channelVolumes[5], 0.64F),
               "unmatched surround channels must preserve their balance");
}

static bool testZeroMasterInitializesUnmatchedChannels() {
  auto physical = makeSurroundState();
  physical.channelVolumes.fill(0.0F);
  const auto stereoMap =
      std::array<std::uint32_t, 2>{SPA_AUDIO_CHANNEL_FL,
                                  SPA_AUDIO_CHANNEL_FR};
  const auto previous = pipetune::mapPhysicalVolumeToVirtual(
      physical, stereoMap);
  auto requested = previous;
  requested.channelVolumes[0] = 0.3F;
  requested.channelVolumes[1] = 0.3F;
  const auto mapped = pipetune::mapVirtualVolumeToPhysical(
      requested, previous, physical);

  for (auto channel = std::uint32_t{0}; channel < mapped.channelCount;
       ++channel) {
    if (!check(near(mapped.channelVolumes[channel], 0.3F),
               "zero-master expansion must initialize every channel")) {
      return false;
    }
  }
  return true;
}

static bool testBuiltParameterRoundTripsEffectiveState() {
  const auto original = makeSurroundState();
  auto storage = std::array<std::uint8_t, 2048>{};
  auto builder = spa_pod_builder{};
  spa_pod_builder_init(&builder, storage.data(), storage.size());
  const auto *parameter =
      pipetune::buildPipeWireVolumeParameter(builder, original);
  const auto *masterProperty =
      parameter == nullptr
          ? nullptr
          : spa_pod_find_prop(parameter, nullptr, SPA_PROP_volume);
  auto master = float{0.0F};
  auto parsed = pipetune::PipeWireVolumeState{};
  const auto merged =
      pipetune::mergePipeWireVolumeState(parameter, parsed);
  return check(masterProperty != nullptr &&
                   spa_pod_get_float(&masterProperty->value, &master) >= 0 &&
                   near(master, 1.0F),
               "built controls must normalize the physical master") &&
         check(merged.valid && merged.volumePresent,
               "built volume parameter must parse") &&
         check(parsed.channelCount == original.channelCount &&
                   parsed.muted == original.muted &&
                   parsed.muteKnown == original.muteKnown,
               "built volume controls must round trip") &&
         check(near(parsed.channelVolumes[0],
                    original.channelVolumes[0]) &&
                   near(parsed.channelVolumes[5],
                        original.channelVolumes[5]),
               "built effective channel volumes differ");
}

int main() {
  return testEffectiveVolumesAreNotMultipliedByMasterOrSoftVolume() &&
                 testPartialMutePreservesVolume() &&
                 testInvalidVolumeDoesNotModifyState() &&
                 testStereoRequestPreservesSurroundBalance() &&
                 testZeroMasterInitializesUnmatchedChannels() &&
                 testBuiltParameterRoundTripsEffectiveState()
             ? 0
             : 1;
}
