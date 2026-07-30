#include "pipewire_volume_state.h"

#include <spa/param/props.h>
#include <spa/pod/iter.h>

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace pipetune {

static bool readFloatArray(const spa_pod_prop *property,
                           std::array<float, SPA_AUDIO_MAX_CHANNELS> &values,
                           std::uint32_t &count) noexcept {
  if (property == nullptr || !spa_pod_is_array(&property->value) ||
      SPA_POD_ARRAY_VALUE_TYPE(&property->value) != SPA_TYPE_Float ||
      SPA_POD_ARRAY_VALUE_SIZE(&property->value) != sizeof(float)) {
    return false;
  }
  auto actualCount = std::uint32_t{0};
  if (spa_pod_get_array(&property->value, &actualCount) == nullptr ||
      actualCount == 0 || actualCount > SPA_AUDIO_MAX_CHANNELS) {
    return false;
  }
  count = spa_pod_copy_array(&property->value, SPA_TYPE_Float,
                             values.data(), values.size());
  return count == actualCount &&
         std::all_of(values.begin(), values.begin() + count,
                     [](float value) {
                       return std::isfinite(value) && value >= 0.0F;
                     });
}

static bool readChannelMap(
    const spa_pod_prop *property,
    std::array<std::uint32_t, SPA_AUDIO_MAX_CHANNELS> &positions,
    std::uint32_t &count) noexcept {
  if (property == nullptr || !spa_pod_is_array(&property->value) ||
      SPA_POD_ARRAY_VALUE_TYPE(&property->value) != SPA_TYPE_Id ||
      SPA_POD_ARRAY_VALUE_SIZE(&property->value) !=
          sizeof(std::uint32_t)) {
    return false;
  }
  auto actualCount = std::uint32_t{0};
  const auto *data =
      spa_pod_get_array(&property->value, &actualCount);
  if (actualCount > SPA_AUDIO_MAX_CHANNELS ||
      (actualCount != 0 && data == nullptr)) {
    return false;
  }
  if (actualCount == 0) {
    count = 0;
    return true;
  }
  count = spa_pod_copy_array(&property->value, SPA_TYPE_Id,
                             positions.data(), positions.size());
  return count == actualCount;
}

static bool readMute(const spa_pod_prop *property,
                     bool &muted) noexcept {
  return property != nullptr &&
         spa_pod_get_bool(&property->value, &muted) >= 0;
}

static bool arraysEqual(
    const std::array<float, SPA_AUDIO_MAX_CHANNELS> &left,
    const std::array<float, SPA_AUDIO_MAX_CHANNELS> &right,
    std::uint32_t count) noexcept {
  return std::equal(left.begin(), left.begin() + count, right.begin());
}

static bool mapsEqual(
    const std::array<std::uint32_t, SPA_AUDIO_MAX_CHANNELS> &left,
    const std::array<std::uint32_t, SPA_AUDIO_MAX_CHANNELS> &right,
    std::uint32_t count) noexcept {
  return std::equal(left.begin(), left.begin() + count, right.begin());
}

PipeWireVolumeMergeResult mergePipeWireVolumeState(
    const spa_pod *parameter, PipeWireVolumeState &state) noexcept {
  if (parameter == nullptr ||
      !spa_pod_is_object_type(parameter, SPA_TYPE_OBJECT_Props)) {
    return {.valid = false, .changed = false, .volumePresent = false};
  }

  auto next = state;
  auto volumePresent = false;
  const auto *volumeProperty =
      spa_pod_find_prop(parameter, nullptr, SPA_PROP_channelVolumes);
  if (volumeProperty != nullptr) {
    volumePresent = true;
    if (!readFloatArray(volumeProperty, next.channelVolumes,
                        next.channelCount)) {
      return {.valid = false, .changed = false, .volumePresent = true};
    }
  }

  const auto *mapProperty =
      spa_pod_find_prop(parameter, nullptr, SPA_PROP_channelMap);
  if (mapProperty != nullptr &&
      !readChannelMap(mapProperty, next.channelMap,
                      next.channelMapCount)) {
    return {.valid = false, .changed = false,
            .volumePresent = volumePresent};
  }

  auto *muteProperty =
      spa_pod_find_prop(parameter, nullptr, SPA_PROP_mute);
  if (muteProperty == nullptr) {
    muteProperty =
        spa_pod_find_prop(parameter, nullptr, SPA_PROP_softMute);
  }
  if (muteProperty != nullptr) {
    if (!readMute(muteProperty, next.muted)) {
      return {.valid = false, .changed = false,
              .volumePresent = volumePresent};
    }
    next.muteKnown = true;
  }

  const auto changed =
      next.channelCount != state.channelCount ||
      !arraysEqual(next.channelVolumes, state.channelVolumes,
                   next.channelCount) ||
      next.channelMapCount != state.channelMapCount ||
      !mapsEqual(next.channelMap, state.channelMap,
                 next.channelMapCount) ||
      next.muted != state.muted ||
      next.muteKnown != state.muteKnown;
  if (changed) {
    state = next;
  }
  return {.valid = true, .changed = changed,
          .volumePresent = volumePresent};
}

spa_pod *buildPipeWireVolumeParameter(
    spa_pod_builder &builder,
    const PipeWireVolumeState &state) noexcept {
  if (state.channelCount == 0 ||
      state.channelCount > SPA_AUDIO_MAX_CHANNELS) {
    return nullptr;
  }

  auto frame = spa_pod_frame{};
  spa_pod_builder_push_object(&builder, &frame, SPA_TYPE_OBJECT_Props,
                              SPA_PARAM_Props);
  spa_pod_builder_add(
      &builder, SPA_PROP_volume, SPA_POD_Float(1.0F),
      SPA_PROP_channelVolumes,
      SPA_POD_Array(sizeof(float), SPA_TYPE_Float,
                    static_cast<int>(state.channelCount),
                    const_cast<float *>(state.channelVolumes.data())),
      0);
  if (state.muteKnown) {
    spa_pod_builder_add(&builder, SPA_PROP_mute,
                        SPA_POD_Bool(state.muted), 0);
  }
  return static_cast<spa_pod *>(
      spa_pod_builder_pop(&builder, &frame));
}

static bool hasUsableMap(const PipeWireVolumeState &state) noexcept {
  return state.channelCount != 0 &&
         state.channelMapCount == state.channelCount;
}

static bool positionIsUsable(std::uint32_t position) noexcept {
  return position != SPA_AUDIO_CHANNEL_UNKNOWN &&
         position != SPA_AUDIO_CHANNEL_NA;
}

static std::uint32_t findChannel(
    const PipeWireVolumeState &state, std::uint32_t position) noexcept {
  if (!hasUsableMap(state) || !positionIsUsable(position)) {
    return SPA_AUDIO_MAX_CHANNELS;
  }
  for (auto index = std::uint32_t{0}; index < state.channelCount;
       ++index) {
    if (state.channelMap[index] == position) {
      return index;
    }
  }
  return SPA_AUDIO_MAX_CHANNELS;
}

static float logicalMaster(
    const PipeWireVolumeState &state) noexcept {
  if (state.channelCount == 0) {
    return 0.0F;
  }
  return *std::max_element(
      state.channelVolumes.begin(),
      state.channelVolumes.begin() + state.channelCount);
}

PipeWireVolumeState mapPhysicalVolumeToVirtual(
    const PipeWireVolumeState &physical,
    std::span<const std::uint32_t> virtualChannelMap) noexcept {
  auto result = PipeWireVolumeState{};
  result.channelCount = std::min(
      static_cast<std::uint32_t>(virtualChannelMap.size()),
      SPA_AUDIO_MAX_CHANNELS);
  result.channelMapCount = result.channelCount;
  result.muted = physical.muteKnown && physical.muted;
  result.muteKnown = true;
  const auto physicalHasMap = hasUsableMap(physical);
  const auto master = logicalMaster(physical);

  for (auto channel = std::uint32_t{0}; channel < result.channelCount;
       ++channel) {
    result.channelMap[channel] = virtualChannelMap[channel];
    auto source = SPA_AUDIO_MAX_CHANNELS;
    if (physicalHasMap) {
      source = findChannel(physical, virtualChannelMap[channel]);
    } else if (physical.channelCount != 0) {
      source = physical.channelCount == 1
                   ? 0
                   : std::min(channel, physical.channelCount - 1);
    }
    result.channelVolumes[channel] =
        source < physical.channelCount
            ? physical.channelVolumes[source]
            : master;
  }
  return result;
}

PipeWireVolumeState mapVirtualVolumeToPhysical(
    const PipeWireVolumeState &requestedVirtual,
    const PipeWireVolumeState &previousVirtual,
    const PipeWireVolumeState &physical) noexcept {
  auto result = physical;
  if (requestedVirtual.channelCount == 0 ||
      physical.channelCount == 0) {
    return result;
  }

  const auto oldMaster = logicalMaster(previousVirtual);
  const auto newMaster = logicalMaster(requestedVirtual);
  const auto ratio =
      oldMaster > 0.0F ? newMaster / oldMaster : 0.0F;
  const auto mapsAvailable =
      hasUsableMap(requestedVirtual) && hasUsableMap(physical);
  for (auto channel = std::uint32_t{0}; channel < physical.channelCount;
       ++channel) {
    auto source = SPA_AUDIO_MAX_CHANNELS;
    if (mapsAvailable) {
      source = findChannel(requestedVirtual,
                           physical.channelMap[channel]);
    } else if (channel < requestedVirtual.channelCount) {
      source = channel;
    } else if (requestedVirtual.channelCount == 1) {
      source = 0;
    }

    if (source < requestedVirtual.channelCount) {
      result.channelVolumes[channel] =
          requestedVirtual.channelVolumes[source];
    } else if (oldMaster > 0.0F) {
      result.channelVolumes[channel] =
          physical.channelVolumes[channel] * ratio;
    } else {
      result.channelVolumes[channel] = newMaster;
    }
  }
  if (requestedVirtual.muteKnown) {
    result.muted = requestedVirtual.muted;
    result.muteKnown = true;
  }
  return result;
}

static bool near(float left, float right) noexcept {
  const auto difference = std::abs(left - right);
  const auto scale =
      std::max({1.0F, std::abs(left), std::abs(right)});
  return difference <= 0.000001F * scale;
}

bool pipeWireVolumeStatesEquivalent(
    const PipeWireVolumeState &left,
    const PipeWireVolumeState &right) noexcept {
  if (left.channelCount != right.channelCount ||
      left.channelMapCount != right.channelMapCount ||
      left.muteKnown != right.muteKnown ||
      (left.muteKnown && left.muted != right.muted)) {
    return false;
  }
  for (auto channel = std::uint32_t{0}; channel < left.channelCount;
       ++channel) {
    if (!near(left.channelVolumes[channel],
              right.channelVolumes[channel])) {
      return false;
    }
  }
  return mapsEqual(left.channelMap, right.channelMap,
                   left.channelMapCount);
}

} // namespace pipetune
