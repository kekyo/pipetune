/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include "filter_graph_properties.h"

#include <pipewire/pipewire.h>
#include <pipewire/impl-module.h>
#include <pipewire/extensions/metadata.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/latency-utils.h>
#include <spa/param/props.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <map>
#include <optional>
#include <string>

struct Probe;

constexpr auto kTagPeriod = std::uint32_t{524287};
constexpr auto kChannelOffset = std::uint32_t{524288};
constexpr auto kSampleScale = 268435456.0F;
constexpr auto kRestartFirstTag = std::uint32_t{262144};

enum class Phase {
  initial, disconnected, reconnected, attenuated, muted, restored,
  awaitingPolicyStop, restartingOutputs, policyRestarted, restoringOutputs, awaitingPulseCheck
};

struct Endpoint {
  Probe *probe = nullptr;
  pw_stream *stream = nullptr;
  spa_hook listener = {};
  pw_stream_events events = {};
  std::uint32_t firstChannel = 0;
  std::uint64_t receivedFrames = 0;
  std::uint64_t attenuatedFrames = 0;
  std::uint64_t mutedFrames = 0;
  std::uint64_t restoredFrames = 0;
  std::uint64_t framesAfterRestart = 0;
  std::uint64_t mutedFramesAfterRestart = 0;
  std::uint64_t restartWarmupFrames = 0;
  std::int64_t lastGraphNanoseconds = 0;
  std::uint32_t lastBlockFrames = 0;
  int previousTag = -1;
  bool selected = true;
  bool awaitingSignal = true;
};

struct OutputLatency {
  pw_port *port = nullptr;
  std::uint32_t id = PW_ID_ANY;
  spa_hook listener = {};
  pw_port_events events = {};
  std::int64_t reportedFrames = -1;
};

struct Selector {
  Probe *probe = nullptr;
  pw_core *core = nullptr;
  pw_registry *registry = nullptr;
  spa_hook listener = {};
  pw_registry_events events = {};
  std::map<std::uint32_t, std::string> nodes;
};

struct OutputLink {
  pw_link *proxy = nullptr;
  spa_hook listener = {};
  pw_link_events events = {};
  std::uint32_t targetId = PW_ID_ANY;
  bool active = false;
};

struct Probe {
  pw_main_loop *loop = nullptr;
  pw_context *context = nullptr;
  pw_core *core = nullptr;
  pw_core *applicationCore = nullptr;
  pw_registry *registry = nullptr;
  pw_registry_events registryEvents = {};
  spa_hook registryListener = {};
  pw_node *combinedNode = nullptr;
  pw_node *controlNode = nullptr;
  std::uint32_t controlNodeId = PW_ID_ANY;
  spa_hook controlNodeListener = {};
  pw_node_events controlNodeEvents = {};
  float visibleVolume = -1.0F;
  bool visibleMute = false;
  bool controlsRestoredAfterRestart = false;
  pw_metadata *metadata = nullptr;
  std::uint32_t metadataId = PW_ID_ANY;
  std::uint32_t metadataInstances = 0;
  pw_metadata_events metadataEvents = {};
  spa_hook metadataListener = {};
  pw_impl_module *combine = nullptr;
  pw_stream *source = nullptr;
  pw_stream_events sourceEvents = {};
  spa_hook sourceListener = {};
  pw_stream *processorInput = nullptr;
  pw_stream *processorOutput = nullptr;
  pw_stream_events processorInputEvents = {};
  pw_stream_events processorOutputEvents = {};
  spa_hook processorInputListener = {};
  spa_hook processorOutputListener = {};
  spa_source *watchdog = nullptr;
  spa_source *pulseSignal = nullptr;
  std::array<Endpoint, 3> endpoints = {};
  std::array<OutputLatency, 2> outputLatencies = {};
  std::array<Selector, 2> selectors = {};
  std::uint64_t producedFrames = 0;
  std::uint64_t sourceTagOffset = 0;
  std::uint64_t processedFrames = 0;
  std::uint64_t unmutedInputFramesDuringMute = 0;
  std::array<float, 2> channelVolumes = {1.0F, 1.0F};
  bool masterMute = false;
  bool reconnectMuted = false;
  std::uint64_t reconnectMutedFrames = 0;
  std::uint32_t reconnectingOutputId = PW_ID_ANY;
  std::optional<std::uint64_t> reconnectStartTick;
  std::uint64_t reconnectWaitFrames = 0;
  std::uint64_t reconnectGraphWaitFrames = 0;
  std::uint64_t channelErrors = 0;
  std::uint64_t disconnectSurvivorFrame = 0;
  std::uint64_t survivorFramesWhileDisconnected = 0;
  Phase phase = Phase::initial;
  bool reconnect = false;
  bool volume = false;
  bool latency = false;
  bool compensate = true;
  bool policy = false;
  bool visibility = false;
  bool recovery = false;
  bool restart = false;
  bool restartControls = false;
  bool restartWithMute = false;
  bool pulse = false;
  bool pulseReady = false;
  bool outputsHiddenDuringPlayback = false;
  bool outputsRestored = false;
  bool defaultsReady = false;
  bool defaultRequested = false;
  std::uint32_t applicationClientId = PW_ID_ANY;
  std::uint32_t processorClientId = PW_ID_ANY;
  bool captureFeedsPlayback = false;
  bool success = false;
  std::string error;
  std::map<std::string, std::string> nodeNames;
  std::map<std::string, std::pair<std::string, std::string>> links;
  std::map<std::uint32_t, OutputLink> outputLinks;
  std::map<std::string, std::string> metadataValues;
};

static bool createEndpoint(Probe &probe, std::uint32_t index);
static bool createSource(Probe &probe);
static bool createCombinedOutput(Probe &probe);
static void selectDefaultOutput(Probe &probe);
static void finishVisibilityCheck(Probe &probe);
static bool observeOutputs(Probe &probe, Selector &selector, pw_core *core);
static void restoreVisibleOutputs(Probe &probe);
static void closeProcessingOutputs(Probe &probe);

static void fail(Probe &probe, const std::string &message) {
  if (probe.error.empty()) probe.error = message;
  pw_main_loop_quit(probe.loop);
}

// A channel-specific offset identifies the channel, and a modulo-524287 tag
// identifies consecutive frames without depending on startup silence length.
// Values are exactly representable in float32 and remain below 0.034 FS.
// The period exceeds the producer's watchdog frame limit, so a long delay
// cannot alias a short delay in the latency comparison.
static float sampleValue(std::uint32_t channel, std::uint64_t frame) {
  return static_cast<float>((channel + 1u) * kChannelOffset + frame % kTagPeriod) /
         kSampleScale;
}

static bool setVolume(Probe &probe, float gain, bool mute) {
  if (probe.pulse) {
    std::cerr << "pipetune-probe:volume-" << (mute ? "mute" : gain == 1.0F ? "restore" : "attenuate")
              << '\n' << std::flush;
    return true;
  }
  if (probe.controlNode == nullptr) {
    fail(probe, "combined output was not available for volume control");
    return false;
  }
  auto storage = std::array<std::uint8_t, 1024>{};
  auto builder = SPA_POD_BUILDER_INIT(storage.data(), storage.size());
  const auto volumes = std::array<float, 4>{gain, gain, gain, gain};
  const auto *properties = static_cast<const spa_pod *>(spa_pod_builder_add_object(
      &builder, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props,
      SPA_PROP_volume, SPA_POD_Float(1.0F),
      SPA_PROP_mute, SPA_POD_Bool(mute),
      SPA_PROP_channelVolumes, SPA_POD_Array(sizeof(float), SPA_TYPE_Float,
                                              probe.policy ? 2u : volumes.size(), volumes.data())));
  if (pw_node_set_param(probe.controlNode, SPA_PARAM_Props, 0, properties) < 0) {
    fail(probe, "cannot change combined output volume");
    return false;
  }
  return true;
}

static bool validateFrame(Endpoint &endpoint, const std::array<float, 2> &samples) {
  if (!endpoint.selected) return false;
  const auto phase = endpoint.probe->phase;
  // PulseAudio's 50% software volume corresponds to a linear gain of 1/8.
  const auto attenuatedGain = endpoint.probe->pulse ? 0.125F : 0.25F;
  const auto silence = samples[0] == 0.0F && samples[1] == 0.0F;
  if (phase == Phase::restartingOutputs && endpoint.probe->restartWithMute) {
    if (!silence) return false;
    ++endpoint.mutedFramesAfterRestart;
    endpoint.previousTag = -1;
    return true;
  }
  if (phase == Phase::muted && silence) {
    ++endpoint.mutedFrames;
    endpoint.previousTag = -1;
    return true;
  }
  if (phase == Phase::restored && silence && endpoint.restoredFrames == 0)
    return true;
  auto gain = 1.0F;
  if (phase == Phase::attenuated) {
    // Accept already queued full-volume frames until the requested gain is
    // first observed in PCM. Afterwards, every sample must use the new gain.
    const auto boundary = sampleValue(endpoint.firstChannel, 0) * 0.5F;
    if (samples[0] < boundary) gain = attenuatedGain;
    else if (endpoint.attenuatedFrames != 0) return false;
  } else if (phase == Phase::muted) {
    if (endpoint.mutedFrames != 0) return false;
    gain = attenuatedGain;
  } else if (endpoint.probe->restartControls &&
      (phase == Phase::restartingOutputs || phase == Phase::policyRestarted)) {
    gain = attenuatedGain;
  }
  const auto tag = static_cast<int>(std::lround(samples[0] / gain * kSampleScale)) -
                   static_cast<int>((endpoint.firstChannel + 1u) * kChannelOffset);
  if (tag < 0 || tag >= static_cast<int>(kTagPeriod) ||
      samples[0] != sampleValue(endpoint.firstChannel, tag) * gain ||
      samples[1] != sampleValue(endpoint.firstChannel + 1, tag) * gain ||
      (phase != Phase::restartingOutputs && endpoint.previousTag != -1 &&
       tag != (endpoint.previousTag + 1) % static_cast<int>(kTagPeriod)))
    return false;
  endpoint.previousTag = tag;
  if (phase == Phase::attenuated && gain == attenuatedGain) ++endpoint.attenuatedFrames;
  if (phase == Phase::restored) ++endpoint.restoredFrames;
  return true;
}

static void capture(void *data) {
  auto &endpoint = *static_cast<Endpoint *>(data);
  auto &probe = *endpoint.probe;
  auto *queued = pw_stream_dequeue_buffer(endpoint.stream);
  if (queued == nullptr) return;
  if (probe.phase == Phase::restoringOutputs || probe.phase == Phase::awaitingPolicyStop ||
      probe.phase == Phase::awaitingPulseCheck) {
    pw_stream_queue_buffer(endpoint.stream, queued);
    return;
  }
  auto *buffer = queued->buffer;
  if (buffer == nullptr || buffer->n_datas < 2) {
    pw_stream_queue_buffer(endpoint.stream, queued);
    fail(probe, "capture did not negotiate two planar channels");
    return;
  }
  auto frameCount = UINT32_MAX;
  for (auto channel = 0u; channel < 2; ++channel) {
    const auto &plane = buffer->datas[channel];
    if (plane.data == nullptr || plane.chunk == nullptr ||
        plane.chunk->stride != static_cast<int>(sizeof(float)) ||
        plane.chunk->offset > plane.maxsize ||
        plane.chunk->size > plane.maxsize - plane.chunk->offset) {
      pw_stream_queue_buffer(endpoint.stream, queued);
      fail(probe, "capture received an invalid planar buffer");
      return;
    }
    frameCount = std::min(frameCount, plane.chunk->size /
                                        static_cast<std::uint32_t>(sizeof(float)));
  }
  if (probe.latency) {
    auto time = pw_time{};
    if (pw_stream_get_time_n(endpoint.stream, &time, sizeof(time)) < 0 ||
        time.rate.num != 1 || time.rate.denom != 48000) {
      pw_stream_queue_buffer(endpoint.stream, queued);
      fail(probe, "capture did not provide the expected graph timeline");
      return;
    }
    endpoint.lastGraphNanoseconds = time.now;
    endpoint.lastBlockFrames = frameCount;
  }
  auto silent = true;
  for (auto frame = 0u; frame < frameCount; ++frame) {
    auto samples = std::array<float, 2>{};
    for (auto channel = 0u; channel < 2; ++channel) {
      const auto &plane = buffer->datas[channel];
      std::memcpy(&samples[channel],
                  static_cast<const char *>(plane.data) + plane.chunk->offset +
                      frame * sizeof(float), sizeof(float));
    }
    silent = silent && samples[0] == 0.0F && samples[1] == 0.0F;
    if (probe.reconnectMuted && probe.phase == Phase::reconnected &&
        endpoint.firstChannel == 0 && !silent) {
      ++probe.channelErrors;
      pw_stream_queue_buffer(endpoint.stream, queued);
      fail(probe, "a reconnecting output received audio before all channels were ready");
      return;
    }
    if (samples[0] == 0.0F && samples[1] == 0.0F &&
        (endpoint.awaitingSignal || probe.phase == Phase::restartingOutputs) &&
        !(probe.phase == Phase::restartingOutputs && probe.restartWithMute && endpoint.selected))
      continue;
    // A restart discards queued audio. Only begin the new continuous PCM
    // interval when the producer's post-restart tag reaches each output.
    if (probe.phase == Phase::policyRestarted && endpoint.selected && endpoint.awaitingSignal &&
        samples[0] < sampleValue(endpoint.firstChannel, kRestartFirstTag) *
            (probe.restartControls ? 0.25F : 1.0F)) continue;
    if (!validateFrame(endpoint, samples)) {
      ++probe.channelErrors;
      pw_stream_queue_buffer(endpoint.stream, queued);
      fail(probe, "received PCM does not preserve channel identity and sample order: channel " +
                      std::to_string(endpoint.firstChannel + 1) + " after " +
                      std::to_string(endpoint.receivedFrames) + " frames, values " +
                      std::to_string(samples[0]) + ", " + std::to_string(samples[1]) +
                      ", scaled " + std::to_string(samples[0] * kSampleScale) + ", " +
                      std::to_string(samples[1] * kSampleScale) +
                      ", previous tag " + std::to_string(endpoint.previousTag));
      return;
    }
    endpoint.awaitingSignal = false;
    ++endpoint.receivedFrames;
    if (probe.phase == Phase::policyRestarted) ++endpoint.framesAfterRestart;
    if (probe.phase == Phase::restartingOutputs) ++endpoint.restartWarmupFrames;
  }
  pw_stream_queue_buffer(endpoint.stream, queued);
  if (probe.phase == Phase::reconnected && endpoint.firstChannel == 0 && endpoint.awaitingSignal &&
      silent && frameCount > 0) {
    const auto target = pw_stream_get_node_id(endpoint.stream);
    const auto activeLinks = std::count_if(probe.outputLinks.begin(), probe.outputLinks.end(),
        [target](const auto &entry) { return entry.second.targetId == target && entry.second.active; });
    if (activeLinks == 2) {
      // The receiver only verifies silence. Playback readiness must be
      // decided by the controller without a callback from the destination.
      probe.reconnectMutedFrames += frameCount;
    }
  }
  if (probe.phase == Phase::restartingOutputs) {
    const auto warmupFrames = probe.restartWithMute ? 8192u : 256u;
    if (probe.endpoints[0].restartWarmupFrames >= warmupFrames &&
        probe.endpoints[1].restartWarmupFrames >= warmupFrames) {
      if (probe.restartControls) {
        probe.controlsRestoredAfterRestart = probe.visibleVolume == 0.25F &&
            probe.visibleMute == probe.restartWithMute;
        if (!probe.controlsRestoredAfterRestart) {
          fail(probe, "desktop master controls were not restored after the policy restart");
          return;
        }
        if (probe.restartWithMute && !setVolume(probe, 0.25F, false)) return;
      }
      if (probe.producedFrames >= kRestartFirstTag) {
        fail(probe, "policy restart exceeded the first PCM tag interval");
        return;
      }
      // Begin the continuous interval only after both destinations have
      // received audio through their rebuilt links. Initialization may
      // discard queued frames while those separate links are established.
      probe.sourceTagOffset = kRestartFirstTag - probe.producedFrames;
      for (auto &output : probe.endpoints) {
        output.previousTag = -1;
        output.awaitingSignal = true;
      }
      probe.phase = Phase::policyRestarted;
    }
  } else if (probe.phase == Phase::policyRestarted) {
    if (probe.endpoints[0].framesAfterRestart >= 65536 &&
        probe.endpoints[1].framesAfterRestart >= 65536) restoreVisibleOutputs(probe);
  } else if (probe.volume) {
    if (probe.phase == Phase::initial && probe.endpoints[0].receivedFrames >= 65536 &&
        probe.endpoints[1].receivedFrames >= 65536) {
      if (setVolume(probe, 0.25F, false)) probe.phase = Phase::attenuated;
    } else if (probe.phase == Phase::attenuated && probe.endpoints[0].attenuatedFrames >= 8192 &&
               probe.endpoints[1].attenuatedFrames >= 8192) {
      if (probe.restartControls && !probe.restartWithMute) restoreVisibleOutputs(probe);
      else if (setVolume(probe, 0.25F, true)) probe.phase = Phase::muted;
    } else if (probe.phase == Phase::muted && probe.endpoints[0].mutedFrames >= 8192 &&
               probe.endpoints[1].mutedFrames >= 8192) {
      if (probe.restartWithMute) restoreVisibleOutputs(probe);
      else if (setVolume(probe, 1.0F, false)) probe.phase = Phase::restored;
    } else if (probe.phase == Phase::restored && probe.endpoints[0].restoredFrames >= 8192 &&
               probe.endpoints[1].restoredFrames >= 8192) {
      if (probe.pulse) restoreVisibleOutputs(probe);
      else {
        probe.success = true;
        pw_main_loop_quit(probe.loop);
      }
    }
  } else if (probe.reconnect && probe.phase == Phase::initial &&
      probe.endpoints[0].receivedFrames >= 65536 &&
      probe.endpoints[1].receivedFrames >= 65536) {
    // The surviving device's received frames are the barrier for recreating
    // the disconnected sink, rather than a fixed wall-clock sleep.
    probe.reconnectMuted = probe.policy;
    auto &removed = probe.endpoints[0];
    pw_stream_destroy(removed.stream);
    removed.stream = nullptr;
    probe.disconnectSurvivorFrame = probe.endpoints[1].receivedFrames;
    probe.phase = Phase::disconnected;
  } else if (probe.phase == Phase::disconnected &&
      probe.endpoints[1].receivedFrames >= probe.disconnectSurvivorFrame + 8192) {
    probe.survivorFramesWhileDisconnected =
        probe.endpoints[1].receivedFrames - probe.disconnectSurvivorFrame;
    if (!createEndpoint(probe, 0)) {
      fail(probe, "cannot reconnect the first output");
      return;
    }
    probe.phase = Phase::reconnected;
  } else if ((!probe.reconnect && probe.endpoints[0].receivedFrames >= 65536 &&
              probe.endpoints[1].receivedFrames >= 65536) ||
             (probe.phase == Phase::reconnected &&
              probe.endpoints[0].receivedFrames >= 131072 &&
              probe.endpoints[1].receivedFrames >= 131072)) {
    if (probe.visibility) restoreVisibleOutputs(probe);
    else {
      probe.success = true;
      pw_main_loop_quit(probe.loop);
    }
  }
}

static std::size_t visiblePhysicalOutputs(const Selector &selector) {
  return std::count_if(selector.nodes.begin(), selector.nodes.end(),
      [](const auto &entry) { return entry.second.starts_with("pipetune_probe_device_"); });
}

static bool visibleCombinedOutput(const Selector &selector) {
  return std::any_of(selector.nodes.begin(), selector.nodes.end(),
      [](const auto &entry) { return entry.second == "pipetune_probe_combined"; });
}

static void desktopControlsChanged(void *data, int, std::uint32_t id,
                                    std::uint32_t, std::uint32_t, const spa_pod *parameter) {
  auto &probe = *static_cast<Probe *>(data);
  if (id != SPA_PARAM_Props || parameter == nullptr) return;
  auto volume = 1.0F;
  if (const auto *property = spa_pod_find_prop(parameter, nullptr, SPA_PROP_volume))
    spa_pod_get_float(&property->value, &volume);
  if (const auto *property = spa_pod_find_prop(parameter, nullptr, SPA_PROP_channelVolumes)) {
    auto channels = std::array<float, 2>{};
    const auto count = spa_pod_copy_array(&property->value, SPA_TYPE_Float,
        channels.data(), channels.size());
    if (count == 2 && channels[0] == channels[1]) probe.visibleVolume = channels[0] * volume;
  }
  if (const auto *property = spa_pod_find_prop(parameter, nullptr, SPA_PROP_mute))
    spa_pod_get_bool(&property->value, &probe.visibleMute);
}

static void finishVisibilityCheck(Probe &probe) {
  if (probe.phase != Phase::restoringOutputs || probe.outputsRestored) return;
  const auto selection = probe.metadataValues.find("default.audio.sink");
  if (selection == probe.metadataValues.end() ||
      selection->second.find("pipetune_probe_device_") == std::string::npos) return;
  for (const auto &selector : probe.selectors)
    if (visiblePhysicalOutputs(selector) != 3 || visibleCombinedOutput(selector)) return;
  probe.outputsRestored = true;
  if (probe.pulse) {
    // Keep simulated devices alive until the separate PulseAudio clients have
    // observed their restoration through the server's own registry views.
    std::cerr << "pipetune-probe:outputs-restored\n" << std::flush;
    return;
  }
  probe.success = true;
  pw_main_loop_quit(probe.loop);
}

static bool observeOutputs(Probe &probe, Selector &selector, pw_core *core) {
  selector.probe = &probe;
  selector.core = core;
  selector.registry = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);
  if (selector.registry == nullptr) return false;
  selector.events.version = PW_VERSION_REGISTRY_EVENTS;
  selector.events.global = [](void *data, std::uint32_t id, std::uint32_t,
                              const char *type, std::uint32_t version, const spa_dict *properties) {
    auto &view = *static_cast<Selector *>(data);
    if (std::strcmp(type, PW_TYPE_INTERFACE_Node) != 0 || properties == nullptr) return;
    const auto *mediaClass = spa_dict_lookup(properties, PW_KEY_MEDIA_CLASS);
    if (mediaClass == nullptr || std::strcmp(mediaClass, "Audio/Sink") != 0) return;
    const auto *name = spa_dict_lookup(properties, PW_KEY_NODE_NAME);
    if (name != nullptr) view.nodes[id] = name;
    // Model desktop controls with a separate client, including its normal
    // permission boundary, instead of the node owner's loopback connection.
    if (view.probe->volume && !view.probe->pulse &&
        &view == &view.probe->selectors[0] && name != nullptr &&
        std::strcmp(name, "pipetune_probe_combined") == 0) {
      view.probe->controlNode = static_cast<pw_node *>(pw_registry_bind(view.registry, id,
          PW_TYPE_INTERFACE_Node, std::min(version, std::uint32_t{PW_VERSION_NODE}), 0));
      if (view.probe->controlNode == nullptr) fail(*view.probe, "cannot bind desktop volume controls");
      else {
        view.probe->controlNodeId = id;
        view.probe->visibleVolume = -1.0F;
        view.probe->controlNodeEvents.version = PW_VERSION_NODE_EVENTS;
        view.probe->controlNodeEvents.param = desktopControlsChanged;
        pw_node_add_listener(view.probe->controlNode, &view.probe->controlNodeListener,
            &view.probe->controlNodeEvents, view.probe);
        auto parameter = std::uint32_t{SPA_PARAM_Props};
        if (pw_node_subscribe_params(view.probe->controlNode, &parameter, 1) < 0)
          fail(*view.probe, "cannot observe desktop volume controls");
      }
    }
    finishVisibilityCheck(*view.probe);
  };
  selector.events.global_remove = [](void *data, std::uint32_t id) {
    auto &view = *static_cast<Selector *>(data);
    if (&view == &view.probe->selectors[0] && id == view.probe->controlNodeId) {
      spa_hook_remove(&view.probe->controlNodeListener);
      pw_proxy_destroy(reinterpret_cast<pw_proxy *>(view.probe->controlNode));
      view.probe->controlNode = nullptr;
      view.probe->controlNodeId = PW_ID_ANY;
      view.probe->controlNodeListener = {};
    }
    view.nodes.erase(id);
    finishVisibilityCheck(*view.probe);
  };
  pw_registry_add_listener(selector.registry, &selector.listener, &selector.events, &selector);
  return true;
}

static void restoreVisibleOutputs(Probe &probe) {
  for (const auto &selector : probe.selectors) {
    if (selector.nodes.size() != 1 || !visibleCombinedOutput(selector)) {
      fail(probe, "a selector or playback client can select outputs other than the public logical sink");
      return;
    }
  }
  probe.outputsHiddenDuringPlayback = true;
  if (probe.pulse) {
    probe.phase = Phase::awaitingPulseCheck;
    std::cerr << "pipetune-probe:outputs-hidden\n" << std::flush;
    return;
  }
  if (probe.recovery || (probe.restart && probe.metadataInstances == 1)) {
    probe.phase = Phase::awaitingPolicyStop;
    std::cerr << "pipetune-probe:outputs-hidden\n" << std::flush;
    return;
  }
  closeProcessingOutputs(probe);
}

static void closeProcessingOutputs(Probe &probe) {
  probe.phase = Phase::restoringOutputs;
  if (probe.source != nullptr) pw_stream_destroy(probe.source);
  probe.source = nullptr;
  pw_stream_destroy(probe.processorInput);
  probe.processorInput = nullptr;
  pw_stream_destroy(probe.processorOutput);
  probe.processorOutput = nullptr;
  pw_impl_module_destroy(probe.combine);
  probe.combine = nullptr;
  finishVisibilityCheck(probe);
}

static void produce(void *data) {
  auto &probe = *static_cast<Probe *>(data);
  const auto channels = probe.policy ? 2u : 4u;
  auto *queued = pw_stream_dequeue_buffer(probe.source);
  if (queued == nullptr) return;
  auto *buffer = queued->buffer;
  if (buffer == nullptr || buffer->n_datas < channels) {
    pw_stream_queue_buffer(probe.source, queued);
    fail(probe, "producer did not negotiate the requested planar channels");
    return;
  }
  auto frameCount = static_cast<std::uint32_t>(
      queued->requested == 0 ? 256 : queued->requested);
  for (auto channel = 0u; channel < channels; ++channel) {
    if (buffer->datas[channel].data == nullptr ||
        buffer->datas[channel].chunk == nullptr) {
      pw_stream_queue_buffer(probe.source, queued);
      fail(probe, "producer received an unmapped buffer");
      return;
    }
    frameCount = std::min(frameCount, buffer->datas[channel].maxsize /
                                        static_cast<std::uint32_t>(sizeof(float)));
  }
  for (auto channel = 0u; channel < channels; ++channel) {
    auto &plane = buffer->datas[channel];
    auto *samples = static_cast<float *>(plane.data);
    for (auto frame = 0u; frame < frameCount; ++frame)
      samples[frame] = sampleValue(channel, probe.producedFrames + probe.sourceTagOffset + frame) *
          (probe.policy ? 0.5F : 1.0F);
    plane.chunk->offset = 0;
    plane.chunk->stride = sizeof(float);
    plane.chunk->size = frameCount * sizeof(float);
  }
  queued->size = frameCount;
  probe.producedFrames += frameCount;
  pw_stream_queue_buffer(probe.source, queued);
  if (probe.producedFrames >= 480000 && !probe.success)
    fail(probe, "both selected outputs did not receive the required PCM frames");
}

static void sourceStateChanged(void *data, pw_stream_state,
                               pw_stream_state state, const char *error) {
  if (state == PW_STREAM_STATE_ERROR)
    fail(*static_cast<Probe *>(data), error == nullptr ? "source failed" : error);
}

static void sinkStateChanged(void *data, pw_stream_state,
                             pw_stream_state state, const char *error) {
  if (state == PW_STREAM_STATE_ERROR)
    fail(*static_cast<Endpoint *>(data)->probe,
         error == nullptr ? "sink failed" : error);
}

static bool connectStream(pw_stream *stream, bool input, std::uint32_t channels,
                           std::uint32_t extraFlags) {
  auto raw = spa_audio_info_raw{};
  raw.format = SPA_AUDIO_FORMAT_F32P;
  raw.rate = 48000;
  raw.channels = channels;
  for (auto channel = 0u; channel < raw.channels; ++channel) {
    if (channels == 2)
      raw.position[channel] = channel == 0 ? SPA_AUDIO_CHANNEL_FL : SPA_AUDIO_CHANNEL_FR;
    else
      raw.position[channel] = SPA_AUDIO_CHANNEL_AUX0 + channel;
  }
  auto storage = std::array<std::uint8_t, 1024>{};
  auto builder = SPA_POD_BUILDER_INIT(storage.data(), storage.size());
  const auto *format = spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &raw);
  // Main-loop process callbacks keep failure reporting and termination out of
  // the real-time thread. This executable is a verification driver, not DSP.
  const auto flags = static_cast<pw_stream_flags>(PW_STREAM_FLAG_MAP_BUFFERS |
      extraFlags);
  return pw_stream_connect(stream, input ? PW_DIRECTION_INPUT : PW_DIRECTION_OUTPUT,
                           PW_ID_ANY, flags, &format, 1) >= 0;
}

static void updateReconnectingOutput(Probe &probe, std::uint32_t frames) {
  if (!probe.reconnectMuted || frames == 0) return;
  const auto activeLinks = std::count_if(probe.outputLinks.begin(), probe.outputLinks.end(),
      [&probe](const auto &entry) {
        return entry.second.targetId == probe.reconnectingOutputId && entry.second.active;
      });
  if (probe.reconnectingOutputId == PW_ID_ANY || activeLinks != 2 ||
      pw_stream_get_state(probe.processorOutput, nullptr) != PW_STREAM_STATE_STREAMING) {
    probe.reconnectStartTick.reset();
    probe.reconnectWaitFrames = 0;
    return;
  }
  auto time = pw_time{};
  if (pw_stream_get_time_n(probe.processorOutput, &time, sizeof(time)) < 0) return;
  // This driver fixes both stream and graph rates at 48 kHz. Queued and
  // buffered frames therefore have the same units as graph ticks and delay.
  if (time.rate.num != 1 || time.rate.denom != 48000) {
    fail(probe, "processor did not provide the expected reconnect timeline");
    return;
  }
  if (!probe.reconnectStartTick || time.ticks < *probe.reconnectStartTick) {
    probe.reconnectStartTick = time.ticks;
    probe.reconnectWaitFrames = 0;
  }
  // Keep the destination silent until a complete block can have traversed
  // the now-linked graph, including its queues and reported downstream delay.
  // Only the owned output stream's public clock is used for this decision;
  // a real playback device cannot acknowledge PCM through capture callbacks.
  probe.reconnectWaitFrames = std::max(probe.reconnectWaitFrames,
      static_cast<std::uint64_t>(std::max(time.delay, std::int64_t{0})) +
          time.queued + time.buffered + frames);
  const auto elapsed = time.ticks - *probe.reconnectStartTick;
  if (elapsed < probe.reconnectWaitFrames) return;
  probe.reconnectGraphWaitFrames = elapsed;
  probe.reconnectMuted = false;
}

static void processorOutputProcess(void *data) {
  auto &probe = *static_cast<Probe *>(data);
  auto *input = pw_stream_dequeue_buffer(probe.processorInput);
  if (input == nullptr) return;
  auto *output = pw_stream_dequeue_buffer(probe.processorOutput);
  if (output == nullptr) {
    pw_stream_queue_buffer(probe.processorInput, input);
    fail(probe, "processor has no output buffer");
    return;
  }
  auto valid = input->buffer != nullptr && output->buffer != nullptr &&
      input->buffer->n_datas == 2 && output->buffer->n_datas == 4;
  auto frames = std::uint32_t{0};
  for (auto channel = 0u; valid && channel < 4; ++channel) {
    const auto &from = input->buffer->datas[channel % 2];
    const auto &to = output->buffer->datas[channel];
    valid = from.data != nullptr && from.chunk != nullptr && to.data != nullptr &&
        to.chunk != nullptr && from.chunk->stride == static_cast<int>(sizeof(float)) &&
        from.chunk->offset <= from.maxsize &&
        from.chunk->size <= from.maxsize - from.chunk->offset &&
        from.chunk->size <= to.maxsize;
    if (!valid) break;
    const auto channelFrames = from.chunk->size / static_cast<std::uint32_t>(sizeof(float));
    if (channel == 0) frames = channelFrames;
    else valid = frames == channelFrames;
  }
  if (valid) {
    updateReconnectingOutput(probe, frames);
    for (auto channel = 0u; channel < 4; ++channel) {
      const auto &from = input->buffer->datas[channel % 2];
      auto &to = output->buffer->datas[channel];
      auto *samples = static_cast<float *>(to.data);
      for (auto frame = 0u; frame < frames; ++frame) {
        auto sample = 0.0F;
        std::memcpy(&sample, static_cast<const char *>(from.data) +
            from.chunk->offset + frame * sizeof(float), sizeof(float));
        if (channel == 0 && probe.masterMute && sample != 0.0F)
          ++probe.unmutedInputFramesDuringMute;
        const auto processed = sample == 0.0F ? 0.0F : sample * 2.0F +
            (channel >= 2 ? 2.0F * kChannelOffset / kSampleScale : 0.0F);
        // This probe exercises uniform master volume. The additive DSP term
        // must be attenuated too, so applying gain before DSP cannot pass.
        samples[frame] = probe.masterMute || (probe.reconnectMuted && channel < 2) ? 0.0F :
            processed * probe.channelVolumes[0];
      }
      to.chunk->offset = 0;
      to.chunk->stride = sizeof(float);
      to.chunk->size = frames * sizeof(float);
    }
    output->size = frames;
    probe.processedFrames += frames;
  }
  pw_stream_queue_buffer(probe.processorOutput, output);
  pw_stream_queue_buffer(probe.processorInput, input);
  if (!valid) fail(probe, "processor did not receive compatible stereo input and four-channel output buffers");
}

static void processorInputProcess(void *data) {
  auto &probe = *static_cast<Probe *>(data);
  if (pw_stream_trigger_process(probe.processorOutput) >= 0) return;
  // Like the product runtime, retire input while the output is still being
  // connected. The always-process input may run before its output is ready.
  while (auto *input = pw_stream_dequeue_buffer(probe.processorInput)) {
    input->size = 0;
    pw_stream_queue_buffer(probe.processorInput, input);
  }
}

static bool publishMasterVolume(Probe &probe) {
  auto storage = std::array<std::uint8_t, 256>{};
  auto builder = SPA_POD_BUILDER_INIT(storage.data(), storage.size());
  const auto unity = std::array<float, 2>{1.0F, 1.0F};
  const auto *properties = static_cast<const spa_pod *>(spa_pod_builder_add_object(
      &builder, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props,
      SPA_PROP_volume, SPA_POD_Float(1.0F),
      SPA_PROP_mute, SPA_POD_Bool(probe.masterMute),
      SPA_PROP_channelVolumes, SPA_POD_Array(sizeof(float), SPA_TYPE_Float,
          probe.channelVolumes.size(), probe.channelVolumes.data()),
      SPA_PROP_softMute, SPA_POD_Bool(false),
      SPA_PROP_softVolumes, SPA_POD_Array(sizeof(float), SPA_TYPE_Float,
          unity.size(), unity.data())));
  return pw_stream_set_param(probe.processorInput, SPA_PARAM_Props, properties) >= 0;
}

static void processorInputParameterChanged(void *data, std::uint32_t id, const spa_pod *parameter) {
  auto &probe = *static_cast<Probe *>(data);
  if (!probe.policy || id != SPA_PARAM_Props || parameter == nullptr ||
      probe.phase == Phase::restoringOutputs) return;
  // Adapter construction emits its initial properties before the stream has
  // a node to control. Initialize our controls after connectStream returns.
  if (pw_stream_get_node_id(probe.processorInput) == PW_ID_ANY) return;
  auto masterVolume = 1.0F;
  if (const auto *property = spa_pod_find_prop(parameter, nullptr, SPA_PROP_volume))
    spa_pod_get_float(&property->value, &masterVolume);
  if (const auto *property = spa_pod_find_prop(parameter, nullptr, SPA_PROP_mute))
    spa_pod_get_bool(&property->value, &probe.masterMute);
  if (const auto *property = spa_pod_find_prop(parameter, nullptr, SPA_PROP_channelVolumes)) {
    const auto count = spa_pod_copy_array(&property->value, SPA_TYPE_Float,
        probe.channelVolumes.data(), probe.channelVolumes.size());
    if (count != 0 && (count != 2 || probe.channelVolumes[0] != probe.channelVolumes[1])) {
      fail(probe, "master volume probe requires uniform stereo controls");
      return;
    }
  }
  for (auto &volume : probe.channelVolumes) volume *= masterVolume;
  // Intercept the adapter parameters: retain the visible channel controls,
  // but force its software gain to unity. The DSP output applies the gain.
  if (!publishMasterVolume(probe)) fail(probe, "cannot publish master volume");
}

static bool createProcessor(Probe &probe) {
  auto options = pipetune::FilterGraphPropertyOptions{
      .nodeName = "pipetune_probe_processor",
      .nodeDescription = "PipeTune probe processor",
      .fixedSampleRate = 48000,
      .channelCount = 2,
      .forceRate = false};
  const auto input = pipetune::makeFilterGraphProperties(options);
  options.channelCount = 4;
  const auto output = pipetune::makeFilterGraphProperties(options);
  auto *inputProperties = pw_properties_new(nullptr, nullptr);
  auto *outputProperties = pw_properties_new(nullptr, nullptr);
  for (const auto &[key, value] : input.input)
    pw_properties_set(inputProperties, key.c_str(), value.c_str());
  for (const auto &[key, value] : output.output)
    pw_properties_set(outputProperties, key.c_str(), value.c_str());
  if (probe.policy) {
    // The selectable logical output is the application's actual input sink.
    // The distribution sink remains private behind the DSP.
    pw_properties_set(inputProperties, PW_KEY_NODE_NAME, "pipetune_probe_combined");
    pw_properties_set(inputProperties, "node.pipetune.internal", nullptr);
    pw_properties_set(inputProperties, "node.pipetune.aggregate", "true");
    pw_properties_set(inputProperties, "node.pipetune.public-input", "true");
    pw_properties_set(inputProperties, "filter.smart", nullptr);
    pw_properties_set(inputProperties, "filter.smart.name", nullptr);
    pw_properties_set(inputProperties, "target.endpoint", nullptr);
    pw_properties_set(inputProperties, "node.pipetune.target-endpoint", nullptr);
  }
  pw_properties_set(outputProperties, PW_KEY_TARGET_OBJECT,
      probe.policy ? "pipetune_probe_distribution" : "pipetune_probe_combined");
  pw_properties_set(outputProperties, "node.pipetune.managed-output", "true");
  pw_properties_set(outputProperties, "node.dont-fallback", "true");
  pw_properties_set(outputProperties, "node.dont-move", "true");
  probe.processorInput = pw_stream_new(probe.core, "PipeTune probe processor input", inputProperties);
  probe.processorOutput = pw_stream_new(probe.core, "PipeTune probe processor output", outputProperties);
  if (probe.processorInput == nullptr || probe.processorOutput == nullptr) return false;
  probe.processorInputEvents.version = PW_VERSION_STREAM_EVENTS;
  probe.processorInputEvents.state_changed = sourceStateChanged;
  probe.processorInputEvents.process = processorInputProcess;
  probe.processorInputEvents.param_changed = processorInputParameterChanged;
  probe.processorOutputEvents.version = PW_VERSION_STREAM_EVENTS;
  probe.processorOutputEvents.state_changed = sourceStateChanged;
  probe.processorOutputEvents.process = processorOutputProcess;
  pw_stream_add_listener(probe.processorInput, &probe.processorInputListener,
      &probe.processorInputEvents, &probe);
  pw_stream_add_listener(probe.processorOutput, &probe.processorOutputListener,
      &probe.processorOutputEvents, &probe);
  return connectStream(probe.processorInput, true, 2, PW_STREAM_FLAG_ASYNC) &&
      connectStream(probe.processorOutput, false, 4,
          PW_STREAM_FLAG_TRIGGER | PW_STREAM_FLAG_AUTOCONNECT) &&
      (!probe.policy || publishMasterVolume(probe));
}

static void sinkParameterChanged(void *data, std::uint32_t id, const spa_pod *parameter) {
  auto &endpoint = *static_cast<Endpoint *>(data);
  if (!endpoint.probe->latency || (id != SPA_PARAM_Latency && id != SPA_PARAM_Format)) return;
  auto storage = std::array<std::uint8_t, 1024>{};
  auto builder = SPA_POD_BUILDER_INIT(storage.data(), storage.size());
  auto latency = spa_latency_info{};
  latency.direction = SPA_DIRECTION_INPUT;
  latency.min_rate = latency.max_rate = endpoint.firstChannel == 2 ? 63 : 0;
  auto parameters = std::array<const spa_pod *, 2>{};
  parameters[0] = spa_latency_build(&builder, SPA_PARAM_Latency, &latency);
  auto upstream = spa_latency_info{};
  // Latency negotiation can replace the port's parameter list. Publish both
  // the sink's fixed downstream delay and the newly received upstream delay.
  if (parameter != nullptr && id == SPA_PARAM_Latency &&
      spa_latency_parse(parameter, &upstream) >= 0 && upstream.direction == SPA_DIRECTION_OUTPUT)
    parameters[1] = parameter;
  else {
    upstream = {};
    upstream.direction = SPA_DIRECTION_OUTPUT;
    parameters[1] = spa_latency_build(&builder, SPA_PARAM_Latency, &upstream);
  }
  if (pw_stream_update_params(endpoint.stream, parameters.data(), parameters.size()) < 0)
    fail(*endpoint.probe, "cannot publish simulated output latency");
}

static bool createEndpoint(Probe &probe, std::uint32_t index) {
  auto &endpoint = probe.endpoints[index];
  endpoint.probe = &probe;
  endpoint.firstChannel = index * 2u;
  endpoint.selected = index < 2;
  endpoint.previousTag = -1;
  endpoint.awaitingSignal = true;
  endpoint.listener = {};
  const auto name = "pipetune_probe_device_" + std::to_string(index);
  // These virtual PCM checks use the test server's single dummy clock.
  // Keep it as the driver even when a policy endpoint appears or disappears;
  // physical-device clock changes require their own resynchronization test.
  endpoint.stream = pw_stream_new(probe.core, name.c_str(), pw_properties_new(
      PW_KEY_NODE_NAME, name.c_str(), PW_KEY_NODE_DESCRIPTION, name.c_str(),
      PW_KEY_NODE_GROUP, "pipewire.dummy",
      PW_KEY_PRIORITY_SESSION, "1000",
      PW_KEY_MEDIA_CLASS, "Audio/Sink", PW_KEY_MEDIA_CATEGORY, "Playback",
      PW_KEY_NODE_VIRTUAL, probe.visibility ? "false" : "true", "node.want-driver", "true",
      "node.pause-on-idle", "false", "audio.channels", "2",
      "audio.position", "[ FL FR ]", nullptr));
  if (endpoint.stream == nullptr) return false;
  endpoint.events.version = PW_VERSION_STREAM_EVENTS;
  endpoint.events.state_changed = sinkStateChanged;
  endpoint.events.process = capture;
  endpoint.events.param_changed = sinkParameterChanged;
  pw_stream_add_listener(endpoint.stream, &endpoint.listener, &endpoint.events, &endpoint);
  return connectStream(endpoint.stream, true, 2, 0);
}

static int metadataChanged(void *data, std::uint32_t subject, const char *key,
                            const char *, const char *value) {
  auto &probe = *static_cast<Probe *>(data);
  if (subject == 0 && key != nullptr)
    probe.metadataValues[key] = value == nullptr ? "(removed)" : value;
  finishVisibilityCheck(probe);
  if (subject == 0 && key != nullptr && value != nullptr &&
      std::strcmp(key, "default.audio.sink") == 0) {
    // Wait for the default-node policy to publish its first selection. On
    // WirePlumber 0.4 the metadata global appears before that policy listens
    // for changes to default.configured.audio.sink.
    probe.defaultsReady = true;
    selectDefaultOutput(probe);
    if (probe.restart && probe.metadataInstances == 2 &&
        probe.phase == Phase::awaitingPolicyStop &&
        std::string_view{value}.find("pipetune_probe_combined") != std::string_view::npos) {
      // Real ALSA nodes belong to WirePlumber and reappear after it restarts.
      // Recreate the simulated devices only after the new policy is running.
      for (auto index = 0u; index < probe.endpoints.size(); ++index) {
        if (!createEndpoint(probe, index)) {
          fail(probe, "cannot recreate outputs after a policy restart");
          return 0;
        }
      }
      probe.phase = Phase::restartingOutputs;
    }
  }
  if (subject == 0 && key != nullptr && value != nullptr &&
      std::strcmp(key, "default.audio.sink") == 0 &&
      std::string{value}.find("pipetune_probe_combined") != std::string::npos &&
      probe.source == nullptr && !probe.pulseReady && !createSource(probe))
    fail(probe, "cannot start playback on the default output");
  return 0;
}

static void selectDefaultOutput(Probe &probe) {
  if (!probe.policy || !probe.defaultsReady || probe.defaultRequested || probe.metadata == nullptr ||
      probe.combinedNode == nullptr) return;
  probe.defaultRequested = true;
  if (pw_metadata_set_property(probe.metadata, 0, "default.configured.audio.sink",
      "Spa:String:JSON", "{\"name\":\"pipetune_probe_combined\"}") < 0)
    fail(probe, "cannot select the default combined output");
}

static void outputLatencyChanged(void *data, int, std::uint32_t id,
                                  std::uint32_t, std::uint32_t, const spa_pod *parameter) {
  auto &output = *static_cast<OutputLatency *>(data);
  auto latency = spa_latency_info{};
  if (id != SPA_PARAM_Latency || parameter == nullptr ||
      spa_latency_parse(parameter, &latency) < 0 ||
      latency.direction != SPA_DIRECTION_INPUT) return;
  // These tests negotiate exactly 48 kHz with a 256-frame graph quantum.
  // This is the downstream delay published by the port, not direct access
  // to combine-stream's private compensation buffer.
  output.reportedFrames = static_cast<std::int64_t>(
      (latency.min_quantum + latency.max_quantum) * 0.5 * 256) +
      (static_cast<std::int64_t>(latency.min_rate) + latency.max_rate) / 2 +
      static_cast<std::int64_t>((latency.min_ns + latency.max_ns) * 0.5 * 48000 / 1000000000);
}

static void globalAdded(void *data, std::uint32_t id, std::uint32_t,
                        const char *type, std::uint32_t version,
                        const spa_dict *properties) {
  auto &probe = *static_cast<Probe *>(data);
  if (properties == nullptr) return;
  if ((probe.policy || probe.latency) && std::strcmp(type, PW_TYPE_INTERFACE_Node) == 0) {
    const auto *name = spa_dict_lookup(properties, PW_KEY_NODE_NAME);
    if (name != nullptr) probe.nodeNames[std::to_string(id)] = name;
    if (probe.reconnect && name != nullptr && std::strcmp(name, "pipetune_probe_device_0") == 0)
      probe.reconnectingOutputId = id;
    const auto *client = spa_dict_lookup(properties, PW_KEY_CLIENT_ID);
    if (name != nullptr && client != nullptr) {
      if (std::strcmp(name, "pipetune_probe_signal") == 0)
        probe.applicationClientId = static_cast<std::uint32_t>(std::stoul(client));
      if (std::strcmp(name, "pipetune_probe_processor") == 0)
        probe.processorClientId = static_cast<std::uint32_t>(std::stoul(client));
      if (probe.policy && std::strcmp(name, "pipetune_probe_combined") == 0)
        probe.processorClientId = static_cast<std::uint32_t>(std::stoul(client));
    }
  }
  if (probe.latency && std::strcmp(type, PW_TYPE_INTERFACE_Port) == 0) {
    const auto *node = spa_dict_lookup(properties, PW_KEY_NODE_ID);
    const auto *direction = spa_dict_lookup(properties, PW_KEY_PORT_DIRECTION);
    if (node == nullptr || direction == nullptr || std::strcmp(direction, "out") != 0) return;
    for (auto index = 0u; index < probe.outputLatencies.size(); ++index) {
      auto &output = probe.outputLatencies[index];
      const auto name = "output.pipetune_probe_combined_pipetune_probe_device_" + std::to_string(index);
      if (output.port != nullptr || probe.nodeNames[node] != name) continue;
      output.id = id;
      output.port = static_cast<pw_port *>(pw_registry_bind(probe.registry, id,
          PW_TYPE_INTERFACE_Port, std::min(version, std::uint32_t{PW_VERSION_PORT}), 0));
      if (output.port == nullptr) {
        fail(probe, "cannot bind the output port latency");
        return;
      }
      output.events.version = PW_VERSION_PORT_EVENTS;
      output.events.param = outputLatencyChanged;
      pw_port_add_listener(output.port, &output.listener, &output.events, &output);
      auto parameter = std::uint32_t{SPA_PARAM_Latency};
      if (pw_port_subscribe_params(output.port, &parameter, 1) < 0)
        fail(probe, "cannot subscribe to output port latency");
    }
    return;
  }
  if (probe.policy && std::strcmp(type, PW_TYPE_INTERFACE_Link) == 0) {
    const auto *output = spa_dict_lookup(properties, PW_KEY_LINK_OUTPUT_NODE);
    const auto *input = spa_dict_lookup(properties, PW_KEY_LINK_INPUT_NODE);
    if (output != nullptr && input != nullptr) {
      probe.links[std::to_string(id)] = {output, input};
      if (probe.nodeNames[output] == "control.endpoint.pipetune.capture" &&
          probe.nodeNames[input].starts_with("pipetune_probe_device_"))
        probe.captureFeedsPlayback = true;
      if (probe.reconnect && probe.nodeNames[input] == "pipetune_probe_device_0" &&
          probe.nodeNames[output] == "output.pipetune_probe_distribution_pipetune_probe_device_0") {
        auto &observed = probe.outputLinks[id];
        observed.targetId = static_cast<std::uint32_t>(std::stoul(input));
        observed.proxy = static_cast<pw_link *>(pw_registry_bind(probe.registry, id,
            PW_TYPE_INTERFACE_Link, std::min(version, std::uint32_t{PW_VERSION_LINK}), 0));
        if (observed.proxy == nullptr) {
          fail(probe, "cannot observe the reconnecting output's channel links");
          return;
        }
        observed.events.version = PW_VERSION_LINK_EVENTS;
        observed.events.info = [](void *value, const pw_link_info *info) {
          auto &link = *static_cast<OutputLink *>(value);
          link.active = info != nullptr && info->state == PW_LINK_STATE_ACTIVE;
        };
        pw_link_add_listener(observed.proxy, &observed.listener, &observed.events, &observed);
      }
    }
    return;
  }
  if (probe.policy && std::strcmp(type, PW_TYPE_INTERFACE_Metadata) == 0) {
    const auto *name = spa_dict_lookup(properties, PW_KEY_METADATA_NAME);
    if (name == nullptr || std::strcmp(name, "default") != 0) return;
    probe.metadata = static_cast<pw_metadata *>(pw_registry_bind(probe.registry, id,
        PW_TYPE_INTERFACE_Metadata, std::min(version, std::uint32_t{PW_VERSION_METADATA}), 0));
    if (probe.metadata == nullptr) {
      fail(probe, "cannot bind default output metadata");
      return;
    }
    probe.metadataId = id;
    ++probe.metadataInstances;
    probe.metadataEvents.version = PW_VERSION_METADATA_EVENTS;
    probe.metadataEvents.property = metadataChanged;
    pw_metadata_add_listener(probe.metadata, &probe.metadataListener, &probe.metadataEvents, &probe);
    selectDefaultOutput(probe);
    return;
  }
  if (std::strcmp(type, PW_TYPE_INTERFACE_Node) != 0 || properties == nullptr) return;
  const auto *name = spa_dict_lookup(properties, PW_KEY_NODE_NAME);
  if (name == nullptr || std::strcmp(name, "pipetune_probe_combined") != 0) return;
  probe.combinedNode = static_cast<pw_node *>(pw_registry_bind(probe.registry, id,
      PW_TYPE_INTERFACE_Node, std::min(version, std::uint32_t{PW_VERSION_NODE}), 0));
  if (probe.combinedNode == nullptr) fail(probe, "cannot bind combined output controls");
  else selectDefaultOutput(probe);
}

static void globalRemoved(void *data, std::uint32_t id) {
  auto &probe = *static_cast<Probe *>(data);
  if (id == probe.reconnectingOutputId) {
    probe.reconnectingOutputId = PW_ID_ANY;
    probe.reconnectStartTick.reset();
    probe.reconnectWaitFrames = 0;
  }
  if (id == probe.metadataId && probe.metadata != nullptr) {
    pw_proxy_destroy(reinterpret_cast<pw_proxy *>(probe.metadata));
    probe.metadata = nullptr;
    probe.metadataId = PW_ID_ANY;
    probe.metadataListener = {};
    probe.metadataValues.clear();
    if (probe.phase == Phase::awaitingPolicyStop) {
      if (probe.recovery) {
        closeProcessingOutputs(probe);
        std::cerr << "pipetune-probe:aggregate-closed\n" << std::flush;
      } else {
        pw_stream_destroy(probe.processorInput);
        pw_stream_destroy(probe.processorOutput);
        probe.processorInput = nullptr;
        probe.processorOutput = nullptr;
        probe.processorInputListener = {};
        probe.processorOutputListener = {};
        pw_proxy_destroy(reinterpret_cast<pw_proxy *>(probe.combinedNode));
        probe.combinedNode = nullptr;
        pw_impl_module_destroy(probe.combine);
        probe.combine = nullptr;
        for (auto &endpoint : probe.endpoints) {
          pw_stream_destroy(endpoint.stream);
          endpoint.stream = nullptr;
        }
        probe.defaultsReady = false;
        probe.defaultRequested = false;
        if (!createCombinedOutput(probe) || !createProcessor(probe)) {
          fail(probe, "cannot rebuild processing nodes after a policy restart");
          return;
        }
        std::cerr << "pipetune-probe:policy-stopped\n" << std::flush;
      }
    }
  }
  probe.nodeNames.erase(std::to_string(id));
  probe.links.erase(std::to_string(id));
  if (const auto link = probe.outputLinks.find(id); link != probe.outputLinks.end()) {
    pw_proxy_destroy(reinterpret_cast<pw_proxy *>(link->second.proxy));
    probe.outputLinks.erase(link);
  }
  for (auto &output : probe.outputLatencies) {
    if (output.id != id || output.port == nullptr) continue;
    pw_proxy_destroy(reinterpret_cast<pw_proxy *>(output.port));
    output.port = nullptr;
    output.reportedFrames = -1;
  }
}

static bool createSource(Probe &probe) {
  // Use a separate client so the source needs the same permissions as an
  // ordinary desktop application, rather than owning the processing sink.
  auto *sourceCore = probe.core;
  if (probe.policy) {
    probe.applicationCore = pw_context_connect(probe.context, pw_properties_new(
        PW_KEY_APP_NAME, "PipeTune probe application",
        PW_KEY_APP_PROCESS_BINARY, "pipetune_probe_application", nullptr), 0);
    if (probe.applicationCore == nullptr) return false;
    if (probe.visibility && !observeOutputs(probe, probe.selectors[1], probe.applicationCore)) return false;
    sourceCore = probe.applicationCore;
  }
  if (probe.pulse) {
    probe.pulseReady = true;
    std::cerr << "pipetune-probe:pulse-ready\n" << std::flush;
    return true;
  }
  auto *properties = pw_properties_new(
      PW_KEY_NODE_NAME, "pipetune_probe_signal",
      PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Playback",
      PW_KEY_MEDIA_ROLE, probe.policy ? "Music" : "PipeTune-Probe",
      "node.dont-fallback", "true", "node.dont-move", "true",
      "stream.dont-remix", probe.policy ? "false" : "true", nullptr);
  if (!probe.policy) pw_properties_set(properties, PW_KEY_TARGET_OBJECT, "pipetune_probe_combined");
  probe.source = pw_stream_new(sourceCore, "PipeTune probe signal", properties);
  if (probe.source == nullptr) return false;
  probe.sourceEvents.version = PW_VERSION_STREAM_EVENTS;
  probe.sourceEvents.state_changed = sourceStateChanged;
  probe.sourceEvents.process = produce;
  pw_stream_add_listener(probe.source, &probe.sourceListener, &probe.sourceEvents, &probe);
  return connectStream(probe.source, false, probe.policy ? 2u : 4u, PW_STREAM_FLAG_AUTOCONNECT);
}

static bool createCombinedOutput(Probe &probe) {
  const auto arguments = std::string{"combine.latency-compensate = "} +
      (probe.compensate ? "true\n" : "false\n") + "node.name = " +
      (probe.policy ? "pipetune_probe_distribution\n" : "pipetune_probe_combined\n") + R"conf(
    combine.mode = sink
    node.description = "PipeTune multi-device output probe"
    combine.props = {
      audio.position = [ AUX0 AUX1 AUX2 AUX3 ]
      node.virtual = true
    )conf" + (probe.policy ? "node.pipetune.internal = true\n" : "node.pipetune.aggregate = true\n") + R"conf(
    }
    stream.props = {
      stream.dont-remix = true
      node.dont-fallback = true
      node.dont-move = true
      media.role = PipeTune-Probe-Output
      node.pipetune.managed-output = true
    }
    stream.rules = [
      { matches = [ { node.name = pipetune_probe_device_0 } ]
        actions = { create-stream = {
          combine.audio.position = [ AUX0 AUX1 ]
          audio.position = [ FL FR ]
        } } }
      { matches = [ { node.name = pipetune_probe_device_1 } ]
        actions = { create-stream = {
          combine.audio.position = [ AUX2 AUX3 ]
          audio.position = [ FL FR ]
        } } }
    ]
  )conf";
  probe.combine = pw_context_load_module(probe.context,
      "libpipewire-module-combine-stream", arguments.c_str(), nullptr);
  return probe.combine != nullptr;
}

static bool prepare(Probe &probe) {
  probe.loop = pw_main_loop_new(nullptr);
  if (probe.loop == nullptr) return false;
  if (probe.pulse) {
    probe.pulseSignal = pw_loop_add_signal(pw_main_loop_get_loop(probe.loop), SIGUSR1,
        [](void *data, int) {
          auto &state = *static_cast<Probe *>(data);
          if (state.phase == Phase::awaitingPulseCheck) closeProcessingOutputs(state);
          else if (state.outputsRestored) {
            state.success = true;
            pw_main_loop_quit(state.loop);
          } else fail(state, "unexpected PulseAudio verification acknowledgement");
        }, &probe);
    if (probe.pulseSignal == nullptr) return false;
  }
  // The processor and its local modules have the product daemon's identity.
  // Each observer and application overrides it to exercise normal access.
  probe.context = pw_context_new(pw_main_loop_get_loop(probe.loop),
      pw_properties_new(PW_KEY_APP_PROCESS_BINARY, "pipetune", nullptr), 0);
  if (probe.context == nullptr) return false;
  probe.core = pw_context_connect(probe.context, nullptr, 0);
  if (probe.core == nullptr) return false;
  if (probe.visibility || probe.volume) {
    auto *core = pw_context_connect(probe.context,
        pw_properties_new(PW_KEY_APP_NAME, "PipeTune probe sound settings",
            PW_KEY_APP_PROCESS_BINARY, "pipetune_probe_settings", nullptr), 0);
    probe.selectors[0].core = core;
    if (core == nullptr || !observeOutputs(probe, probe.selectors[0], core)) return false;
  }
  probe.registry = pw_core_get_registry(probe.core, PW_VERSION_REGISTRY, 0);
  if (probe.registry == nullptr) return false;
  probe.registryEvents.version = PW_VERSION_REGISTRY_EVENTS;
  probe.registryEvents.global = globalAdded;
  probe.registryEvents.global_remove = globalRemoved;
  pw_registry_add_listener(probe.registry, &probe.registryListener,
                            &probe.registryEvents, &probe);
  for (auto index = 0u; index < probe.endpoints.size(); ++index) {
    if (!createEndpoint(probe, index)) return false;
  }
  if (!createCombinedOutput(probe)) return false;
  if (probe.policy) {
    if (!createProcessor(probe)) return false;
  } else if (!createSource(probe)) return false;
  probe.watchdog = pw_loop_add_timer(pw_main_loop_get_loop(probe.loop),
      [](void *data, std::uint64_t) {
        fail(*static_cast<Probe *>(data), "PipeWire graph did not complete before the watchdog");
      }, &probe);
  if (probe.watchdog == nullptr) return false;
  auto deadline = timespec{20, 0};
  return pw_loop_update_timer(pw_main_loop_get_loop(probe.loop), probe.watchdog,
                              &deadline, nullptr, false) >= 0;
}

static void destroy(Probe &probe) {
  // Stop observing PCM before disconnecting the producer. Node destruction
  // can dispatch pending callbacks with silence from the dismantled graph.
  for (auto &endpoint : probe.endpoints)
    if (endpoint.stream != nullptr) spa_hook_remove(&endpoint.listener);
  if (probe.source != nullptr) spa_hook_remove(&probe.sourceListener);
  if (probe.processorInput != nullptr) spa_hook_remove(&probe.processorInputListener);
  if (probe.processorOutput != nullptr) spa_hook_remove(&probe.processorOutputListener);
  if (probe.pulseSignal != nullptr)
    pw_loop_destroy_source(pw_main_loop_get_loop(probe.loop), probe.pulseSignal);
  if (probe.watchdog != nullptr)
    pw_loop_destroy_source(pw_main_loop_get_loop(probe.loop), probe.watchdog);
  if (probe.source != nullptr) pw_stream_destroy(probe.source);
  if (probe.processorInput != nullptr) pw_stream_destroy(probe.processorInput);
  if (probe.processorOutput != nullptr) pw_stream_destroy(probe.processorOutput);
  if (probe.metadata != nullptr) pw_proxy_destroy(reinterpret_cast<pw_proxy *>(probe.metadata));
  if (probe.controlNode != nullptr) pw_proxy_destroy(reinterpret_cast<pw_proxy *>(probe.controlNode));
  for (auto &[id, link] : probe.outputLinks) {
    static_cast<void>(id);
    if (link.proxy != nullptr) pw_proxy_destroy(reinterpret_cast<pw_proxy *>(link.proxy));
  }
  for (auto &selector : probe.selectors)
    if (selector.registry != nullptr) pw_proxy_destroy(reinterpret_cast<pw_proxy *>(selector.registry));
  for (auto &output : probe.outputLatencies)
    if (output.port != nullptr) pw_proxy_destroy(reinterpret_cast<pw_proxy *>(output.port));
  if (probe.combinedNode != nullptr)
    pw_proxy_destroy(reinterpret_cast<pw_proxy *>(probe.combinedNode));
  if (probe.registry != nullptr)
    pw_proxy_destroy(reinterpret_cast<pw_proxy *>(probe.registry));
  if (probe.combine != nullptr) pw_impl_module_destroy(probe.combine);
  for (auto &endpoint : probe.endpoints)
    if (endpoint.stream != nullptr) pw_stream_destroy(endpoint.stream);
  if (probe.applicationCore != nullptr) pw_core_disconnect(probe.applicationCore);
  if (probe.selectors[0].core != nullptr) pw_core_disconnect(probe.selectors[0].core);
  if (probe.core != nullptr) pw_core_disconnect(probe.core);
  if (probe.context != nullptr) pw_context_destroy(probe.context);
  if (probe.loop != nullptr) pw_main_loop_destroy(probe.loop);
}

int main(int argc, char **argv) {
  pw_init(&argc, &argv);
  auto probe = Probe{};
  if (argc == 2 && std::string(argv[1]) == "reconnect") probe.reconnect = true;
  else if (argc == 2 && std::string(argv[1]) == "volume") probe.volume = true;
  else if (argc == 2 && std::string(argv[1]) == "policy") probe.policy = true;
  else if (argc == 2 && std::string(argv[1]) == "policy-volume") {
    probe.policy = true;
    probe.volume = true;
  }
  else if (argc == 2 && std::string(argv[1]) == "policy-reconnect") {
    probe.policy = true;
    probe.reconnect = true;
  }
  else if (argc == 2 && std::string(argv[1]) == "policy-visibility") {
    probe.policy = true;
    probe.visibility = true;
  }
  else if (argc == 2 && std::string(argv[1]) == "policy-recovery") {
    probe.policy = true;
    probe.visibility = true;
    probe.recovery = true;
  }
  else if (argc == 2 && (std::string(argv[1]) == "policy-restart" ||
                        std::string(argv[1]) == "policy-restart-volume" ||
                        std::string(argv[1]) == "policy-restart-mute")) {
    probe.policy = true;
    probe.visibility = true;
    probe.restart = true;
    probe.restartControls = std::string(argv[1]) != "policy-restart";
    probe.restartWithMute = std::string(argv[1]) == "policy-restart-mute";
    probe.volume = probe.restartControls;
  }
  else if (argc == 2 && (std::string(argv[1]) == "policy-pulse" ||
                        std::string(argv[1]) == "policy-pulse-volume")) {
    probe.policy = true;
    probe.visibility = true;
    probe.pulse = true;
    probe.volume = std::string(argv[1]) == "policy-pulse-volume";
  }
  else if (argc == 2 && (std::string(argv[1]) == "latency" || std::string(argv[1]) == "latency-off")) {
    probe.latency = true;
    probe.compensate = std::string(argv[1]) == "latency";
  }
  else if (argc > 2 || (argc == 2 && std::string(argv[1]) != "channels")) {
    std::cerr << "Usage: pipetune_multi_device_output_probe "
        "[channels|reconnect|volume|latency|latency-off|policy|policy-volume|policy-reconnect|policy-visibility|policy-recovery|policy-restart|policy-restart-volume|policy-restart-mute|policy-pulse|policy-pulse-volume]\n";
    pw_deinit();
    return 2;
  }
  if (prepare(probe)) pw_main_loop_run(probe.loop);
  else probe.error = "cannot prepare multi-device output probe";
  if (!probe.error.empty() && probe.policy) {
    for (const auto &selector : probe.selectors)
      std::cerr << "Selector: " << visiblePhysicalOutputs(selector) << " physical outputs, combined "
          << visibleCombinedOutput(selector) << '\n';
    for (const auto &[key, value] : probe.metadataValues)
      std::cerr << "Metadata " << key << ": " << value << '\n';
    std::cerr << "Application stream: " << (probe.source == nullptr ? "not created" :
        pw_stream_state_as_string(pw_stream_get_state(probe.source, nullptr))) << '\n';
    for (const auto &[id, edge] : probe.links) {
      const auto output = probe.nodeNames.find(edge.first);
      const auto input = probe.nodeNames.find(edge.second);
      std::cerr << "Link " << id << ": "
          << (output == probe.nodeNames.end() ? edge.first : output->second) << " -> "
          << (input == probe.nodeNames.end() ? edge.second : input->second) << '\n';
    }
  }
  destroy(probe);
  if (!probe.error.empty()) std::cerr << probe.error << '\n';
  // The virtual sinks share one driver clock. The stream-local tick counters
  // can have different origins when activation happens in different cycles.
  // Compare snapshots using their common monotonic timestamp instead.
  auto compensation = std::uint64_t{0};
  if (probe.latency && probe.endpoints[0].previousTag >= 0 && probe.endpoints[1].previousTag >= 0) {
    const auto &first = probe.endpoints[0];
    const auto &second = probe.endpoints[1];
    const auto frameDifference = std::llround(
        static_cast<double>(first.lastGraphNanoseconds - second.lastGraphNanoseconds) *
        48000.0 / 1000000000.0) + static_cast<std::int64_t>(first.lastBlockFrames) -
        static_cast<std::int64_t>(second.lastBlockFrames);
    const auto phaseDifference = frameDifference % kTagPeriod + 2 * kTagPeriod +
        second.previousTag - first.previousTag;
    compensation = static_cast<std::uint64_t>(phaseDifference % kTagPeriod);
  }
  std::cout << "{\"success\":" << (probe.success ? "true" : "false")
            << ",\"channels\":4,\"producedFrames\":" << probe.producedFrames
            << ",\"processedFrames\":" << probe.processedFrames
            << ",\"unmutedInputFramesDuringMute\":" << probe.unmutedInputFramesDuringMute
            << ",\"inputChannels\":" << (probe.policy ? 2 : 4)
            << ",\"applicationClientId\":" << probe.applicationClientId
            << ",\"processorClientId\":" << probe.processorClientId
            << ",\"captureFeedsPlayback\":" << (probe.captureFeedsPlayback ? "true" : "false")
            << ",\"outputsHiddenDuringPlayback\":" << (probe.outputsHiddenDuringPlayback ? "true" : "false")
            << ",\"outputsRestored\":" << (probe.outputsRestored ? "true" : "false")
            << ",\"metadataInstances\":" << probe.metadataInstances
            << ",\"framesAfterRestart\":[" << probe.endpoints[0].framesAfterRestart << ','
            << probe.endpoints[1].framesAfterRestart << ']'
            << ",\"mutedFramesAfterRestart\":[" << probe.endpoints[0].mutedFramesAfterRestart << ','
            << probe.endpoints[1].mutedFramesAfterRestart << ']'
            << ",\"controlsRestoredAfterRestart\":" << (probe.controlsRestoredAfterRestart ? "true" : "false")
            << ",\"receivedFrames\":[" << probe.endpoints[0].receivedFrames << ','
            << probe.endpoints[1].receivedFrames << ',' << probe.endpoints[2].receivedFrames
            << "],\"channelErrors\":" << probe.channelErrors
            << ",\"reconnected\":" << (probe.phase == Phase::reconnected ? "true" : "false")
            << ",\"survivorFramesWhileDisconnected\":" << probe.survivorFramesWhileDisconnected
            << ",\"reconnectMutedFrames\":" << probe.reconnectMutedFrames
            << ",\"reconnectGraphWaitFrames\":" << probe.reconnectGraphWaitFrames
            << ",\"declaredLatencyFrames\":" << (probe.latency ? 63 : 0)
            << ",\"observedCompensationFrames\":" << compensation
            << ",\"reportedDelayFrames\":[" << probe.outputLatencies[0].reportedFrames
            << ',' << probe.outputLatencies[1].reportedFrames << ']'
            << ",\"volumeFrames\":[";
  for (auto index = 0u; index < 2; ++index) {
    if (index != 0) std::cout << ',';
    const auto &endpoint = probe.endpoints[index];
    std::cout << "{\"attenuated\":" << endpoint.attenuatedFrames
              << ",\"muted\":" << endpoint.mutedFrames
              << ",\"restored\":" << endpoint.restoredFrames << '}';
  }
  std::cout << "]}\n";
  pw_deinit();
  return probe.success && probe.error.empty() ? 0 : 1;
}
