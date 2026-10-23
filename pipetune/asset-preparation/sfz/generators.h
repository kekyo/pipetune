/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#ifndef PIPETUNE_SFZ_GENERATORS_H
#define PIPETUNE_SFZ_GENERATORS_H
#include "selection.h"
#include <string_view>

namespace pipetune::assets {

/**
 * Recognizes a built-in SFZ sample without resolving a file path.
 * @param sample Exact, case-sensitive sample name.
 * @return Whether the sample is one of the supported generators.
 */
bool isSfzGenerator(std::string_view sample) noexcept;

/**
 * Describes a generated sample for the normal bank capacity selection.
 * @param sample Recognized generator name.
 * @return Mono PCM dimensions and zero encoded-file bytes.
 * @throws SfzError The generator is unknown.
 */
SfzSampleMetadata sfzGeneratorMetadata(std::string_view sample);

/**
 * Generates the application's repeatable sample at its original rate.
 * @param sample Recognized generator name.
 * @return One channel of float32 PCM, with the generator's sample rate.
 * @throws SfzError The generator is unknown.
 * @remarks Periodic samples use the middle-C reference rate; noise uses 48 kHz.
 */
Audio createSfzGeneratorSample(std::string_view sample);

} // namespace pipetune::assets
#endif
