/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#ifndef PIPETUNE_GTK_ACTION_LOG_H
#define PIPETUNE_GTK_ACTION_LOG_H

#include "ui-message.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <string_view>
#include <vector>

namespace pipetune_gtk {

/**
 * Identifies the attention level of one action log entry.
 */
enum class ActionLogSeverity {
  /** Ordinary successful or informational activity. */
  info,
  /** Recoverable fallback or partial degradation. */
  warning,
  /** Failed action that requires attention. */
  error
};

/**
 * Identifies the subsystem that produced an action log entry.
 */
enum class ActionLogCategory {
  /** A dialog settings change. */
  settings,
  /** Control-socket connection or request activity. */
  control,
  /** Startup configuration persistence. */
  persistence,
  /** General application lifecycle activity. */
  application
};

/**
 * Identifies the lifecycle of one action.
 */
enum class ActionLogState {
  /** The action is awaiting completion. */
  pending,
  /** The action completed successfully. */
  success,
  /** The action failed or completed only partially. */
  failure
};

/**
 * Selects entries visible in the log drawer.
 */
enum class ActionLogFilter {
  /** Show every retained entry. */
  all,
  /** Show warning and error entries. */
  warnings,
  /** Show only error entries. */
  errors
};

/**
 * Describes one retained action and its completion state.
 */
struct ActionLogEntry {
  /** Monotonically increasing identifier within this process. */
  std::uint64_t id;
  /** Unix wall-clock timestamp in milliseconds. */
  std::uint64_t timestampUnixMilliseconds;
  /** Attention level used by filters and presentation. */
  ActionLogSeverity severity;
  /** Subsystem that produced the entry. */
  ActionLogCategory category;
  /** Pending, successful, or failed lifecycle state. */
  ActionLogState state;
  /** Concise one-line semantic action summary. */
  UiMessage summary;
  /** Optional localized context or untranslated technical detail. */
  UiMessage detail;
};

/**
 * Stores a bounded in-memory action history.
 */
struct ActionLog {
  /** Entries in chronological order. */
  std::deque<ActionLogEntry> entries;
  /** Maximum retained entry count. */
  std::size_t capacity;
  /** Identifier assigned to the next entry. */
  std::uint64_t nextId;
};

/**
 * Creates an empty bounded action history.
 *
 * @param capacity Maximum number of entries; zero retains no entries.
 * @return Initialized action log.
 */
ActionLog createActionLog(std::size_t capacity);

/**
 * Appends an action that is awaiting asynchronous completion.
 *
 * @param log History to update.
 * @param timestampUnixMilliseconds Start timestamp.
 * @param category Originating subsystem.
 * @param summary Deferred concise action summary.
 * @param detail Deferred contextual or technical detail.
 * @return Identifier used to complete the action in place.
 */
std::uint64_t appendPendingAction(
    ActionLog &log, std::uint64_t timestampUnixMilliseconds,
    ActionLogCategory category, UiMessage summary,
    UiMessage detail);

/**
 * Completes a retained pending action in place.
 *
 * @param log History to update.
 * @param id Identifier returned by appendPendingAction().
 * @param timestampUnixMilliseconds Completion timestamp.
 * @param success True for success, false for failure.
 * @param severity Final attention level.
 * @param summary Final deferred concise summary.
 * @param detail Final deferred diagnostic or contextual detail.
 * @return True when the pending entry was still retained.
 */
bool completePendingAction(
    ActionLog &log, std::uint64_t id,
    std::uint64_t timestampUnixMilliseconds, bool success,
    ActionLogSeverity severity, UiMessage summary,
    UiMessage detail);

/**
 * Appends an already completed action.
 *
 * @param log History to update.
 * @param timestampUnixMilliseconds Completion timestamp.
 * @param severity Attention level.
 * @param category Originating subsystem.
 * @param state Successful or failed state.
 * @param summary Deferred concise action summary.
 * @param detail Deferred contextual or technical detail.
 * @return Identifier assigned to the entry.
 */
std::uint64_t appendAction(
    ActionLog &log, std::uint64_t timestampUnixMilliseconds,
    ActionLogSeverity severity, ActionLogCategory category,
    ActionLogState state, UiMessage summary, UiMessage detail);

/**
 * Selects retained entries for one drawer filter.
 *
 * Returned pointers remain valid until the log is modified.
 *
 * @param log History to inspect.
 * @param filter All, warnings and errors, or errors only.
 * @return Chronological pointers to matching retained entries.
 */
std::vector<const ActionLogEntry *> filteredActionLogEntries(
    const ActionLog &log, ActionLogFilter filter);

/**
 * Removes all retained entries without reusing identifiers.
 *
 * @param log History to clear.
 */
void clearActionLog(ActionLog &log);

} // namespace pipetune_gtk

#endif
