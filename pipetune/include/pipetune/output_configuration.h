/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#ifndef PIPETUNE_OUTPUT_CONFIGURATION_H
#define PIPETUNE_OUTPUT_CONFIGURATION_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace pipetune {

/** Selects who owns physical output selection. */
enum class OutputMode {
  /** Follow the operating system's default output. */
  single,
  /** Route fixed DSP channels to the configured outputs. */
  multiple
};

/** Identifies an output without persisting runtime node or ALSA card IDs. */
struct OutputDeviceIdentity {
  /** Device backend, or "node" for a named virtual output. */
  std::string api;
  /** Stable bus location or backend-provided device identifier. */
  std::string location;
  /** Backend-specific output identifier within that location. */
  std::string port;
  /** Vendor identifier, or empty when unavailable. */
  std::string vendor;
  /** Product identifier, or empty when unavailable. */
  std::string product;
  /** Reported serial information; not assumed to be globally unique. */
  std::string serial;
  /** Compares every persisted identity component. */
  bool operator==(const OutputDeviceIdentity &) const = default;
};

/** Describes every channel exposed by one active output profile. */
struct OutputDeviceDescription {
  /** Stable matching information. */
  OutputDeviceIdentity identity;
  /** User-facing name retained while the device is disconnected. */
  std::string name;
  /** Output profile name; empty for an output without profiles. */
  std::string profile;
  /** Ordered channel positions, including explicit unknown/AUX positions. */
  std::vector<std::string> channelPositions;
  /** Compares the complete description. */
  bool operator==(const OutputDeviceDescription &) const = default;
};

/** Retains a selected or temporarily disabled output and its channel layout. */
struct ConfiguredOutput {
  /** Unique identifier referenced by channel slots in this configuration. */
  std::string id;
  /** False reserves its slots without sending audio to the device. */
  bool enabled = true;
  /** Saved identity, label, profile, and complete channel layout. */
  OutputDeviceDescription device;
  /** Compares all saved output settings. */
  bool operator==(const ConfiguredOutput &) const = default;
};

/** Maps one final DSP channel, identified by its position in the slot array. */
struct OutputChannelSlot {
  /** Configured output identifier, or empty for an unassigned slot. */
  std::string outputId;
  /** Zero-based channel within that output; zero for an unassigned slot. */
  std::uint32_t deviceChannel = 0;
  /** Optional user-facing purpose label, represented by an empty string. */
  std::string label;
  /** Compares the complete mapping and label. */
  bool operator==(const OutputChannelSlot &) const = default;
};

/** Complete persistent routing choices, independent of connection state. */
struct OutputConfiguration {
  /** Defaults to the existing OS-managed behavior. */
  OutputMode mode = OutputMode::single;
  /** Includes disabled and currently disconnected outputs. */
  std::vector<ConfiguredOutput> outputs;
  /** Fixed Ch 1 through Ch N slots, including reserved or unassigned slots. */
  std::vector<OutputChannelSlot> channels;
  /** Compares all persistent choices. */
  bool operator==(const OutputConfiguration &) const = default;
};

/** Reports a complete candidate or the unchanged configuration on failure. */
struct OutputConfigurationResult {
  /** Candidate after success; original configuration after failure. */
  OutputConfiguration configuration;
  /** Human-readable diagnostic, or empty on success. */
  std::string error;
};

/** One currently enumerated output. Runtime identifiers are never persisted. */
struct AvailableOutput {
  /** Current identity, profile, and channel layout. */
  OutputDeviceDescription device;
  /** Current PipeWire target node name. */
  std::string nodeName;
  /** Current PipeWire global ID, valid only for this inventory snapshot. */
  std::uint32_t nodeId = 0;
};

/** Explains whether one saved output can currently receive its assigned audio. */
enum class OutputConnectionState {
  /** All identity and profile constraints match exactly one output. */
  connected,
  /** The user disabled this output while retaining its slots. */
  disabled,
  /** No output matches the saved identity. */
  missing,
  /** Matching hardware exposes a different profile or channel layout. */
  profileMismatch,
  /** Multiple outputs match; no automatic choice is safe. */
  ambiguous
};

/** Resolution of a saved output against one current inventory snapshot. */
struct ResolvedOutput {
  /** Configured identifier, retaining the saved order. */
  std::string outputId;
  /** Connection or mismatch state. */
  OutputConnectionState state;
  /** Index into the supplied inventory, present only when connected. */
  std::optional<std::size_t> inventoryIndex;
};

/**
 * Checks the sixteen-channel limit and complete, unique channel assignments.
 * @param configuration Candidate routing choices.
 * @return Empty on success, otherwise a diagnostic without modifying input.
 */
std::string validateOutputConfiguration(const OutputConfiguration &configuration);

/**
 * Appends every channel of an output without reusing reserved slots.
 * @param configuration Current routing choices.
 * @param output New output, or the same saved ID and layout to enable again.
 * @return Complete valid candidate, or unchanged configuration and diagnostic.
 */
OutputConfigurationResult appendConfiguredOutput(
    const OutputConfiguration &configuration, const ConfiguredOutput &output);

/**
 * Resolves enabled outputs without fallback or changes to persistent slots.
 * @param configuration Valid saved routing choices.
 * @param inventory Current available outputs, in any enumeration order.
 * @return One resolution per configured output, including missing outputs.
 */
std::vector<ResolvedOutput> resolveConfiguredOutputs(
    const OutputConfiguration &configuration, std::span<const AvailableOutput> inventory);

/**
 * Determines the DSP width while preserving the stereo desktop input.
 * @param configuration Valid saved routing choices.
 * @return Two in single mode; at least two and at most sixteen in multiple mode.
 */
std::uint32_t outputDspChannelCount(const OutputConfiguration &configuration) noexcept;

} // namespace pipetune

#endif
