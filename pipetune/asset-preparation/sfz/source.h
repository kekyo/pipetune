/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#ifndef PIPETUNE_SFZ_SOURCE_H
#define PIPETUNE_SFZ_SOURCE_H
#include "parser.h"
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pipetune::assets {

/** Selected ARIA bank definitions and all bank files examined to find them. */
struct SfzBankDefines {
  std::vector<std::pair<std::string, std::string>> defines; /**< Ordered final macro values. */
  std::optional<std::string> bankPath; /**< Matching bank, if any. */
  std::vector<std::string> dependencies; /**< Consulted candidate paths, including empty files. */
};

/**
 * Decodes source bytes with fatal UTF-8 first and Windows-1252 as the fallback.
 * @param bytes Complete source bytes.
 * @return UTF-8 text; a leading UTF-8 BOM is removed when UTF-8 decoding succeeds.
 */
std::string decodeSfzText(std::span<const std::uint8_t> bytes);

/**
 * Finds the applicable ARIA bank and its initial SFZ macro definitions.
 * @param selectedPath Selected root-relative SFZ path.
 * @param bankPaths Root-relative bank definition paths in inventory order.
 * @param documents Already decoded bank texts; an empty text represents a missing or empty file.
 * @return Definitions, selected bank path, and consulted candidate paths.
 * @throws SfzError With requiredPath set when a candidate has not been supplied.
 */
SfzBankDefines readSfzBankDefines(std::string_view selectedPath,
    std::span<const std::string> bankPaths, std::span<const SfzDocument> documents);

} // namespace pipetune::assets
#endif
