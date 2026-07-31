#ifndef PIPETUNE_GTK_UI_LANGUAGE_H
#define PIPETUNE_GTK_UI_LANGUAGE_H

#include <filesystem>
#include <string>
#include <string_view>

namespace pipetune_gtk {

/**
 * Selects the language used by the PipeTune GTK presentation.
 */
enum class UiLanguage {
  /** Follow the process environment and operating-system locale. */
  system,
  /** Always present English messages. */
  english,
  /** Always present Japanese messages. */
  japanese
};

/**
 * Contains a loaded UI language preference and any recoverable diagnostic.
 */
struct UiLanguageLoadResult {
  /** Loaded language, or system when the preference cannot be used. */
  UiLanguage language;
  /** English technical diagnostic for a recoverable load problem. */
  std::string warning;
};

/**
 * Contains the result of saving a UI language preference.
 */
struct UiLanguageSaveResult {
  /** English technical diagnostic, empty when the save succeeded. */
  std::string error;
};

/**
 * Resolves the dedicated PipeTune GTK preference path.
 *
 * @param xdgConfigHome XDG_CONFIG_HOME value, or an empty view when unset.
 * @param home HOME value, or an empty view when unset.
 * @return Path ending in pipetune/gtk.conf.
 */
std::filesystem::path resolveUiLanguageConfigPath(
    std::string_view xdgConfigHome, std::string_view home);

/**
 * Returns the stable configuration identifier for a language.
 *
 * @param language Language to identify.
 * @return One of system, en, or ja.
 */
std::string_view uiLanguageId(UiLanguage language) noexcept;

/**
 * Parses a stable configuration language identifier.
 *
 * @param id Identifier to parse.
 * @param language Receives the parsed language when successful.
 * @return True when id is system, en, or ja.
 */
bool parseUiLanguageId(std::string_view id,
                       UiLanguage &language) noexcept;

/**
 * Loads the dedicated PipeTune GTK language preference.
 *
 * A missing file selects the system language without a warning. Unreadable,
 * malformed, or unsupported preferences select the system language and return
 * a diagnostic suitable for the GUI action log.
 *
 * @param path Preference file to load.
 * @return Loaded preference and recoverable diagnostic.
 */
UiLanguageLoadResult
loadUiLanguagePreference(const std::filesystem::path &path);

/**
 * Saves the dedicated PipeTune GTK language preference atomically.
 *
 * Well-formed existing comments, unknown keys, and unknown groups are
 * retained. The containing PipeTune directory and file are restricted to the
 * current user.
 *
 * @param path Preference file to update.
 * @param language Language to save.
 * @return Empty error on success, otherwise an English technical diagnostic.
 */
UiLanguageSaveResult
saveUiLanguagePreference(const std::filesystem::path &path,
                         UiLanguage language);

} // namespace pipetune_gtk

#endif
