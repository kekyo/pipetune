/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#ifndef PIPETUNE_SFZ_PARSER_H
#define PIPETUNE_SFZ_PARSER_H
#include "region.h"
#include <span>
#include <string_view>

namespace pipetune::assets {

/** One UTF-8 SFZ document supplied from memory. */
struct SfzDocument {
  std::string path; /**< Root-relative normalized document path. */
  std::string text; /**< Complete decoded source text. */
};

/** Detailed import conditions corresponding to parser.js diagnostics. */
struct SfzDiagnostics {
  std::vector<std::string> ignoredOpcodes; /**< Unused opcode names. */
  std::vector<std::string> excludedOpcodes; /**< Conditions excluding regions. */
  std::vector<std::string> missingSamples; /**< Referenced but unavailable sample paths. */
  std::vector<std::string> invalidRegions; /**< Region validation diagnostics. */
};

/** Host-independent normalized definition and required source identities. */
struct ParsedSfz {
  std::vector<SfzRegion> regions; /**< Playable normalized regions in definition order. */
  std::vector<std::string> dependencies; /**< Source documents used by the parser. */
  std::vector<SfzWarning> warnings; /**< Counted partial-import conditions. */
  SfzDiagnostics diagnostics; /**< Detailed, deterministically ordered import conditions. */
};

/**
 * Resolves an SFZ-relative path without accessing the filesystem.
 * @param value Relative path, accepting either slash convention.
 * @param directory Already normalized containing directory.
 * @return Root-relative path without dot components.
 * @throws SfzError The reference escapes its selected root or is absolute.
 */
std::string normalizeSfzPath(std::string_view value, std::string_view directory);

/**
 * Parses supplied documents without files, JSON, a decoder, or a running DSP engine.
 * @param selectedPath Selected SFZ path relative to its registered root.
 * @param documents Complete in-memory source documents.
 * @param availableSamples Optional known sample inventory; absent assumes paths exist.
 * @param maximumBytes Definition expansion and bank budget, from 1 byte through 1 GiB.
 * @param maximumWorkingBytes Conservative allocation-work allowance; the host reserves this separately.
 * @return Normalized definition and import diagnostics.
 * @throws SfzError A document, syntax, or preparation limit is invalid.
 */
ParsedSfz parseSfz(std::string_view selectedPath, std::span<const SfzDocument> documents,
    std::optional<std::span<const std::string>> availableSamples, std::uint64_t maximumBytes,
    std::uint64_t maximumWorkingBytes = UINT64_MAX);

} // namespace pipetune::assets
#endif
