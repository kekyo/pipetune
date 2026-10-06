/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#ifndef PIPETUNE_OUTPUT_JSON_H
#define PIPETUNE_OUTPUT_JSON_H

#include "pipetune/output_configuration.h"
#include <yyjson.h>

namespace pipetune {

/**
 * Decodes an embedded routing object using the persisted configuration schema.
 * @param value Borrowed JSON value, or null for a missing field.
 * @return Valid complete settings, or defaults and a diagnostic.
 */
OutputConfigurationResult parseOutputConfigurationJson(yyjson_val *value);

/**
 * Builds an embedded routing object owned by the supplied document.
 * @param document Destination owner; its root is not changed.
 * @param configuration Complete routing choices.
 * @return Owned object, or null for invalid settings or allocation failure.
 */
yyjson_mut_val *makeOutputConfigurationJson(yyjson_mut_doc *document, const OutputConfiguration &configuration);

/**
 * Decodes a device description without imposing DSP selection limits.
 * @param value Borrowed JSON value.
 * @param device Destination, changed only on success.
 * @return Empty on success, otherwise a diagnostic.
 * @remarks Empty or unsupported channel layouts remain visible in inventories.
 */
std::string parseOutputDeviceJson(yyjson_val *value, OutputDeviceDescription &device);

/**
 * Encodes a device description, including unsupported channel layouts.
 * @param document Destination owner; its root is not changed.
 * @param device Description to encode.
 * @return Owned object, or null on allocation failure.
 */
yyjson_mut_val *makeOutputDeviceJson(yyjson_mut_doc *document, const OutputDeviceDescription &device);

} // namespace pipetune

#endif
