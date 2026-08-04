#ifndef PIPETUNE_GTK_STATUS_MODEL_H
#define PIPETUNE_GTK_STATUS_MODEL_H

#include "application-state.h"

#include "pipetune/startup_config.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace pipetune_gtk {

/**
 * Identifies the semantic state of one status value.
 */
enum class StatusSeverity {
  /** Ordinary healthy or unavailable information. */
  normal,
  /** A degraded or transitional value. */
  warning,
  /** An error requiring attention. */
  error
};

/**
 * Identifies how a status value may be visualized.
 */
enum class StatusDisplayKind {
  /** Render the formatted value as text. */
  text,
  /** Render bounded numeric data as a horizontal level bar. */
  levelBar
};

/**
 * Describes one independent row in the persistent status tree.
 */
struct StatusItem {
  /** Stable identifier used by GTK and end-to-end tests. */
  std::string id;
  /** Human-readable row label. */
  std::string label;
  /** Current display-ready value. */
  std::string value;
  /** Optional raw value retained for future graph presentation. */
  std::optional<double> numericValue;
  /** Unit associated with numericValue. */
  std::string unit;
  /** Semantic severity for styling. */
  StatusSeverity severity;
  /** Current or future visualization type. */
  StatusDisplayKind displayKind;
  /** Optional graph lower bound. */
  std::optional<double> minimum;
  /** Optional graph upper bound. */
  std::optional<double> maximum;
  /** Full value or contextual detail shown on hover. */
  std::string tooltip;
};

/**
 * Describes one always-expanded status tree section.
 */
struct StatusSection {
  /** Stable section identifier. */
  std::string id;
  /** Human-readable section heading. */
  std::string label;
  /** Independent status rows in stable order. */
  std::vector<StatusItem> items;
};

/**
 * Holds normalized drawing data for one bounded status level.
 */
struct StatusLevelPresentation {
  /** Numeric value clamped to the item's inclusive bounds. */
  double clampedValue;
  /** Normalized position from zero through one. */
  double fraction;
  /** Low-saturation HUE step from zero through ten. */
  std::uint8_t hueStep;
};

/**
 * Normalizes graph-ready metadata for level-bar rendering.
 *
 * @param item Status item to inspect.
 * @return Drawing data when all bounds and values are finite and valid.
 */
std::optional<StatusLevelPresentation>
statusLevelPresentation(const StatusItem &item);

/**
 * Builds the complete persistent status tree presentation.
 *
 * Values remain separated into independent rows. DSP Load requests a bounded
 * level-bar presentation, while other numeric metadata remains available for
 * future visualizations without changing the state boundary.
 *
 * @param state Current application and daemon state.
 * @param saved Configuration loaded from persistent storage.
 * @param currentUnixMilliseconds Current wall time reserved for status data.
 * @return Seven stable, always-present status sections.
 */
std::vector<StatusSection> buildStatusSections(
    const ApplicationState &state,
    const pipetune::StartupConfig &saved,
    std::uint64_t currentUnixMilliseconds);

} // namespace pipetune_gtk

#endif
