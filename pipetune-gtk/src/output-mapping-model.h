/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#ifndef PIPETUNE_GTK_OUTPUT_MAPPING_MODEL_H
#define PIPETUNE_GTK_OUTPUT_MAPPING_MODEL_H

#include "pipetune/output_configuration.h"

#include <vector>

namespace pipetune_gtk {

/**
 * Orders configured devices by their lowest EffeTune output number.
 * @param configuration Fixed assignments, including disabled devices.
 * @return Indices into outputs; unassigned devices follow assigned devices.
 * @remarks Equal keys retain the saved device order.
 */
std::vector<std::size_t> configuredOutputOrder(
    const pipetune::OutputConfiguration &configuration);

/**
 * Removes a saved device while retaining the other channel numbers.
 * @param configuration Draft configuration to edit.
 * @param outputId Identifier of the device to remove.
 * @return Edited draft, or the original configuration and a diagnostic.
 * @remarks Removed channels become reserved with their purpose labels. Only
 * trailing slots owned by the removed device are dropped automatically.
 */
pipetune::OutputConfigurationResult removeOutputDevice(
    const pipetune::OutputConfiguration &configuration,
    std::string_view outputId);

/**
 * Explicitly removes every reserved channel and closes the numbering gaps.
 * @param configuration Draft configuration to edit.
 * @return Edited draft, or the original configuration and a diagnostic.
 * @remarks Channels belonging to disabled devices remain assigned.
 */
pipetune::OutputConfigurationResult compactOutputChannels(
    const pipetune::OutputConfiguration &configuration);

/**
 * Proposes an explicit channel move for review before live application.
 * @param configuration Current fixed assignments and labels.
 * @param from Zero-based source slot, including reserved slots.
 * @param to Zero-based destination slot in the resulting configuration.
 * @return Candidate, or the unchanged configuration and a diagnostic.
 * @remarks The purpose label moves with its physical channel assignment.
 */
pipetune::OutputConfigurationResult moveOutputChannel(
    const pipetune::OutputConfiguration &configuration,
    std::size_t from, std::size_t to);

/**
 * Proposes replacing one device while retaining all other channel numbers.
 * @param configuration Current fixed assignments and labels.
 * @param outputId Saved output identifier to retain for the replacement.
 * @param device Explicitly chosen replacement profile and complete layout.
 * @return Candidate, or the unchanged configuration and a diagnostic.
 * @remarks Existing channel indices retain their slots and purpose labels.
 * Removed channels leave reserved slots; new channels append at the end.
 * The caller must verify live identity and obtain the user's mapping review.
 */
pipetune::OutputConfigurationResult replaceOutputDevice(
    const pipetune::OutputConfiguration &configuration,
    std::string_view outputId, const pipetune::OutputDeviceDescription &device);

} // namespace pipetune_gtk

#endif
