/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */

/*
 * Derived from prepareSfzSamples in EffeTune v2.13.0 js/sfz/service.js.
 * Copyright (c) 2025-2026 Yoshiyuki Kobayashi. See LICENSE.effetune.
 */
#include "preparation.h"
#include "selection.h"
#include <algorithm>

namespace pipetune::assets {

SfzBank prepareSfzBank(const ParsedSfz &parsed, std::span<const SfzSample> samples, std::uint64_t maximumBytes) {
  auto bank = packSfzBank(parsed.regions, samples, maximumBytes);
  for (auto &warning : bank.warnings) {
    if (warning.code != "invalid-regions") continue;
    const auto previous = std::ranges::find(parsed.warnings, warning.code, &SfzWarning::code);
    if (previous != parsed.warnings.end()) {
      if (previous->count > UINT32_MAX - warning.count) throw SfzError("too-large", "Too many SFZ region omissions.");
      warning.count += previous->count;
    }
  }
  bank.warnings = mergeSfzWarnings(parsed.warnings, bank.warnings);
  bank.invalidRegions.insert(bank.invalidRegions.begin(), parsed.diagnostics.invalidRegions.begin(), parsed.diagnostics.invalidRegions.end());
  return bank;
}

} // namespace pipetune::assets
