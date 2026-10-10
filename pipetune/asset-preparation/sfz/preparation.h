/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#ifndef PIPETUNE_SFZ_PREPARATION_H
#define PIPETUNE_SFZ_PREPARATION_H
#include "bank.h"
#include "parser.h"

namespace pipetune::assets {

/**
 * Completes an import after optional selection and native audio decoding.
 * @param parsed Selected regions and existing parser/selection warnings.
 * @param samples Original-rate mono/stereo PCM for the selected source files.
 * @param maximumBytes Bank and index budget, from one byte through 1 GiB.
 * @return Bank with combined parser and sample-aware omission counts and diagnostics.
 * @throws SfzError No playable region, invalid audio, or capacity exhaustion.
 * @remarks Distinct parser and sample-position omissions are added; repeated categories
 * from reparsing keep their maximum count. No file, decoder, or host state is accessed.
 */
SfzBank prepareSfzBank(const ParsedSfz &parsed, std::span<const SfzSample> samples, std::uint64_t maximumBytes);

} // namespace pipetune::assets
#endif
