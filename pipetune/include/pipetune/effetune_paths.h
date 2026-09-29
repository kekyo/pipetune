/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#ifndef PIPETUNE_EFFETUNE_PATHS_H
#define PIPETUNE_EFFETUNE_PATHS_H
#include <filesystem>
#include <string_view>
namespace pipetune {
/**
 * Resolves the installed EffeTune application's configuration directory.
 * @param xdgConfigHome XDG_CONFIG_HOME, or empty when unset.
 * @param homeDirectory HOME, or empty when unset.
 * @return Configuration directory, or empty when neither location is available.
 */
std::filesystem::path resolveEffeTuneDirectory(
    std::string_view xdgConfigHome, const std::filesystem::path &homeDirectory);
}
#endif
