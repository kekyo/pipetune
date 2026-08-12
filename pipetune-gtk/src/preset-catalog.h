#ifndef PIPETUNE_GTK_PRESET_CATALOG_H
#define PIPETUNE_GTK_PRESET_CATALOG_H

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace pipetune_gtk {

/**
 * Identifies where an EffeTune preset choice originated.
 */
enum class PresetSource {
  /** Preset distributed with EffeTune. */
  standard,
  /** Named preset saved by the EffeTune desktop application. */
  saved,
};

/**
 * Describes one preset available through EffeTune.
 */
struct PresetChoice {
  /** Origin used to distinguish standard and saved choices. */
  PresetSource source;
  /** User-facing preset name. */
  std::string name;
  /** EffeTune category for a standard preset, otherwise empty. */
  std::string category;
  /** Direct path for a standard preset, otherwise empty. */
  std::filesystem::path path;
  /** Standalone JSON object for a saved preset, otherwise empty. */
  std::string serializedPreset;
};

/**
 * Reports EffeTune's desktop user-preset storage path.
 */
struct EffeTuneUserPresetPathResult {
  /** Resolved `effetune_presets.json` path. */
  std::filesystem::path path;
  /** Resolution diagnostic, or empty on success. */
  std::string error;
};

/**
 * Holds all usable EffeTune preset choices and non-fatal source diagnostics.
 */
struct EffeTunePresetCatalogResult {
  /** Standard choices followed by saved choices. */
  std::vector<PresetChoice> choices;
  /** Missing, malformed, or incomplete source diagnostics. */
  std::vector<std::string> diagnostics;
};

/**
 * Holds one attempted read of EffeTune's saved-preset JSON file.
 */
struct EffeTuneSavedPresetLoadResult {
  /** Saved choices parsed from the complete file. */
  std::vector<PresetChoice> choices;
  /** Whether the file was parsed as a preset object. */
  bool parsed;
  /** Read, parse, or invalid-entry diagnostics. */
  std::vector<std::string> diagnostics;
};

/**
 * Reports the standalone preset path resolved for a catalog choice.
 */
struct PresetChoicePathResult {
  /** Standard path or materialized saved-preset path. */
  std::filesystem::path path;
  /** Materialization diagnostic, or empty on success. */
  std::string error;
};

/**
 * Reports an attempted refresh of the active saved-preset snapshot.
 */
struct ActiveSavedPresetRefreshResult {
  /** Whether the active path belongs to one of the supplied saved presets. */
  bool matched;
  /** Whether the snapshot contents were replaced. */
  bool changed;
  /** Refresh diagnostic, or empty on success or no match. */
  std::string error;
};

/**
 * Resolves the EffeTune desktop application's user-preset JSON file.
 *
 * Electron stores EffeTune data below `effetune` in the XDG configuration
 * directory on Linux.
 *
 * @param xdgConfigHome Value of XDG_CONFIG_HOME, or empty for HOME fallback.
 * @param homeDirectory Value of HOME.
 * @return Resolved file path or a missing-directory diagnostic.
 */
EffeTuneUserPresetPathResult resolveEffeTuneUserPresetPath(
    std::string_view xdgConfigHome,
    const std::filesystem::path &homeDirectory);

/**
 * Loads EffeTune standard presets and named desktop presets.
 *
 * Missing user storage is treated as an empty saved-preset list. Invalid
 * entries are omitted and reported without hiding valid choices.
 *
 * @param standardPresetDirectory Directory containing `presets.txt` and
 * EffeTune's standard `.effetune_preset` files.
 * @param userPresetFile EffeTune desktop `effetune_presets.json` path.
 * @return Available choices and non-fatal diagnostics.
 */
EffeTunePresetCatalogResult loadEffeTunePresetCatalog(
    const std::filesystem::path &standardPresetDirectory,
    const std::filesystem::path &userPresetFile);

/**
 * Loads only the named presets saved by the EffeTune desktop application.
 *
 * A missing file or malformed root is reported with `parsed` set to false.
 * A valid empty object is successful and returns an empty choice list.
 *
 * @param userPresetFile EffeTune desktop `effetune_presets.json` path.
 * @return Saved choices, parse state, and non-fatal diagnostics.
 */
EffeTuneSavedPresetLoadResult loadEffeTuneSavedPresets(
    const std::filesystem::path &userPresetFile);

/**
 * Applies one saved-preset file refresh to the current catalog choices.
 *
 * Standard choices are always retained. Saved choices are completely
 * replaced only when the refreshed file parsed successfully; otherwise the
 * previous saved choices are retained.
 *
 * @param currentChoices Current standard and saved choices.
 * @param refresh Newly loaded saved-preset file state.
 * @return Updated standard and saved choices.
 */
std::vector<PresetChoice> applyEffeTuneSavedPresetRefresh(
    const std::vector<PresetChoice> &currentChoices,
    const EffeTuneSavedPresetLoadResult &refresh);

/**
 * Resolves a catalog choice to a standalone `.effetune_preset` file.
 *
 * Standard presets retain their installed path. A saved preset is atomically
 * copied into a private snapshot below `savedPresetDirectory` so the PipeTune
 * daemon can load it independently of EffeTune's multi-preset JSON file.
 *
 * @param choice Choice to resolve.
 * @param savedPresetDirectory Private directory for saved-preset snapshots.
 * @return Loadable path or a materialization diagnostic.
 */
PresetChoicePathResult resolvePresetChoicePath(
    const PresetChoice &choice,
    const std::filesystem::path &savedPresetDirectory);

/**
 * Refreshes the saved-preset snapshot currently used by the daemon.
 *
 * The active path is matched against deterministic snapshot paths for the
 * supplied saved choices. A matching snapshot is atomically replaced only
 * when its serialized contents changed. Standard and non-active choices are
 * never materialized by this operation.
 *
 * @param choices Current standard and saved preset choices.
 * @param activePresetPath Preset path currently reported by the daemon.
 * @param savedPresetDirectory Private saved-preset snapshot directory.
 * @return Match, change, and diagnostic state.
 */
ActiveSavedPresetRefreshResult refreshActiveSavedPresetSnapshot(
    const std::vector<PresetChoice> &choices,
    const std::filesystem::path &activePresetPath,
    const std::filesystem::path &savedPresetDirectory);

} // namespace pipetune_gtk

#endif
