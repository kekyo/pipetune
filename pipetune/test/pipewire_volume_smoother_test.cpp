#include "pipewire_volume_smoother.h"

#include <spa/param/param.h>
#include <spa/param/props.h>
#include <spa/pod/builder.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <span>
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

static const spa_pod *buildChannelVolumes(
    spa_pod_builder &builder, std::span<float> volumes) {
  return static_cast<const spa_pod *>(spa_pod_builder_add_object(
      &builder, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props,
      SPA_PROP_channelVolumes,
      SPA_POD_Array(sizeof(float), SPA_TYPE_Float,
                    static_cast<int>(volumes.size()), volumes.data())));
}

static bool testStereoVolumeRampsWithoutAStep() {
  auto smoother = pipetune::PipeWireVolumeSmoother(2);
  auto volumes = std::array{0.25F, 0.5F};
  auto parameterStorage = std::array<std::uint8_t, 256>{};
  auto builder = spa_pod_builder{};
  spa_pod_builder_init(
      &builder, parameterStorage.data(), parameterStorage.size());
  const auto *parameter = buildChannelVolumes(builder, volumes);

  auto samples = std::array<float, 10>{};
  samples.fill(1.0F);
  if (!check(smoother.update(parameter, 4),
             "channel-volume changes must update the daemon gain")) {
    return false;
  }
  smoother.process(samples, 5);

  const auto expected = std::array{
      0.8125F, 0.625F, 0.4375F, 0.25F, 0.25F,
      0.875F,  0.75F,  0.625F,  0.5F,  0.5F};
  for (auto index = std::size_t{0}; index < samples.size(); ++index) {
    if (!check(near(samples[index], expected[index]),
               "stereo volume ramp differs")) {
      return false;
    }
  }
  return true;
}

static bool testNewTargetContinuesFromCurrentGain() {
  auto smoother = pipetune::PipeWireVolumeSmoother(1);
  auto firstVolume = std::array{0.0F};
  auto firstStorage = std::array<std::uint8_t, 256>{};
  auto firstBuilder = spa_pod_builder{};
  spa_pod_builder_init(
      &firstBuilder, firstStorage.data(), firstStorage.size());
  const auto *first = buildChannelVolumes(firstBuilder, firstVolume);
  if (!check(smoother.update(first, 4),
             "zero volume must start a fade-out")) {
    return false;
  }

  auto firstSamples = std::array{1.0F, 1.0F};
  smoother.process(firstSamples, firstSamples.size());
  if (!check(near(firstSamples[0], 0.75F) &&
                 near(firstSamples[1], 0.5F),
             "fade-out does not start from unity")) {
    return false;
  }

  auto secondVolume = std::array{1.0F};
  auto secondStorage = std::array<std::uint8_t, 256>{};
  auto secondBuilder = spa_pod_builder{};
  spa_pod_builder_init(
      &secondBuilder, secondStorage.data(), secondStorage.size());
  const auto *second = buildChannelVolumes(secondBuilder, secondVolume);
  if (!check(smoother.update(second, 4),
             "a replacement target must update the active ramp")) {
    return false;
  }

  auto secondSamples = std::array{1.0F, 1.0F, 1.0F, 1.0F};
  smoother.process(secondSamples, secondSamples.size());
  const auto expected =
      std::array{0.625F, 0.75F, 0.875F, 1.0F};
  for (auto index = std::size_t{0}; index < secondSamples.size();
       ++index) {
    if (!check(near(secondSamples[index], expected[index]),
               "replacement ramp must continue from the current gain")) {
      return false;
    }
  }
  return true;
}

static bool testMasterVolumeAndMuteRemainFunctional() {
  auto smoother = pipetune::PipeWireVolumeSmoother(2);
  auto volumes = std::array{0.5F, 0.25F};
  auto parameterStorage = std::array<std::uint8_t, 256>{};
  auto builder = spa_pod_builder{};
  spa_pod_builder_init(
      &builder, parameterStorage.data(), parameterStorage.size());
  const auto *parameter = static_cast<const spa_pod *>(
      spa_pod_builder_add_object(
          &builder, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props,
          SPA_PROP_volume, SPA_POD_Float(0.5F),
          SPA_PROP_channelVolumes,
          SPA_POD_Array(sizeof(float), SPA_TYPE_Float,
                        static_cast<int>(volumes.size()), volumes.data()),
          SPA_PROP_mute, SPA_POD_Bool(false)));
  if (!check(smoother.update(parameter, 1),
             "master and channel volume must be combined")) {
    return false;
  }
  auto samples = std::array{1.0F, 1.0F};
  smoother.process(samples, 1);
  if (!check(near(samples[0], 0.25F) &&
                 near(samples[1], 0.125F),
             "combined master and channel gains differ")) {
    return false;
  }

  auto muteStorage = std::array<std::uint8_t, 128>{};
  auto muteBuilder = spa_pod_builder{};
  spa_pod_builder_init(
      &muteBuilder, muteStorage.data(), muteStorage.size());
  const auto *mute = static_cast<const spa_pod *>(
      spa_pod_builder_add_object(
          &muteBuilder, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props,
          SPA_PROP_mute, SPA_POD_Bool(true)));
  auto mutedSamples = std::array{1.0F, 1.0F, 1.0F, 1.0F};
  if (!check(smoother.update(mute, 2),
             "mute must update daemon gain")) {
    return false;
  }
  smoother.process(mutedSamples, 2);
  return check(near(mutedSamples[0], 0.125F) &&
                   near(mutedSamples[1], 0.0F) &&
                   near(mutedSamples[2], 0.0625F) &&
                   near(mutedSamples[3], 0.0F),
               "mute must ramp each channel to silence");
}

static bool testUnrelatedPropertiesDoNotChangeGain() {
  auto smoother = pipetune::PipeWireVolumeSmoother(1);
  auto storage = std::array<std::uint8_t, 128>{};
  auto builder = spa_pod_builder{};
  spa_pod_builder_init(&builder, storage.data(), storage.size());
  const auto *parameter = static_cast<const spa_pod *>(
      spa_pod_builder_add_object(
          &builder, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props,
          SPA_PROP_quality, SPA_POD_Int(9)));
  auto samples = std::array{0.25F, -0.5F};
  if (!check(!smoother.update(parameter, 4),
             "unrelated properties must not update volume")) {
    return false;
  }
  smoother.process(samples, samples.size());
  return check(samples == std::array{0.25F, -0.5F},
               "unrelated properties must leave PCM unchanged");
}

int main() {
  return testStereoVolumeRampsWithoutAStep() &&
                 testNewTargetContinuesFromCurrentGain() &&
                 testMasterVolumeAndMuteRemainFunctional() &&
                 testUnrelatedPropertiesDoNotChangeGain()
             ? 0
             : 1;
}
