/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#ifndef PIPETUNE_GTK_OUTPUT_MAPPING_MODEL_H
#define PIPETUNE_GTK_OUTPUT_MAPPING_MODEL_H

#include "pipetune/output_configuration.h"

namespace pipetune_gtk {

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
