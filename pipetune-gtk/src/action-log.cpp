#include "action-log.h"

#include <string>
#include <utility>

namespace pipetune_gtk {

static void trimActionLog(ActionLog &log) {
  while (log.entries.size() > log.capacity) {
    log.entries.pop_front();
  }
}

static bool filterMatches(const ActionLogEntry &entry,
                          ActionLogFilter filter) {
  switch (filter) {
  case ActionLogFilter::all:
    return true;
  case ActionLogFilter::warnings:
    return entry.severity == ActionLogSeverity::warning ||
           entry.severity == ActionLogSeverity::error;
  case ActionLogFilter::errors:
    return entry.severity == ActionLogSeverity::error;
  }
  return false;
}

ActionLog createActionLog(std::size_t capacity) {
  return {.entries = {}, .capacity = capacity, .nextId = 1};
}

std::uint64_t appendPendingAction(
    ActionLog &log, std::uint64_t timestampUnixMilliseconds,
    ActionLogCategory category, std::string_view summary,
    std::string_view detail) {
  return appendAction(
      log, timestampUnixMilliseconds, ActionLogSeverity::info, category,
      ActionLogState::pending, summary, detail);
}

bool completePendingAction(
    ActionLog &log, std::uint64_t id,
    std::uint64_t timestampUnixMilliseconds, bool success,
    ActionLogSeverity severity, std::string_view summary,
    std::string_view detail) {
  for (auto &entry : log.entries) {
    if (entry.id != id) {
      continue;
    }
    entry.timestampUnixMilliseconds = timestampUnixMilliseconds;
    entry.severity = severity;
    entry.state =
        success ? ActionLogState::success : ActionLogState::failure;
    entry.summary = summary;
    entry.detail = detail;
    return true;
  }
  return false;
}

std::uint64_t appendAction(
    ActionLog &log, std::uint64_t timestampUnixMilliseconds,
    ActionLogSeverity severity, ActionLogCategory category,
    ActionLogState state, std::string_view summary,
    std::string_view detail) {
  const auto id = log.nextId++;
  log.entries.push_back({
      .id = id,
      .timestampUnixMilliseconds = timestampUnixMilliseconds,
      .severity = severity,
      .category = category,
      .state = state,
      .summary = std::string(summary),
      .detail = std::string(detail),
  });
  trimActionLog(log);
  return id;
}

std::vector<const ActionLogEntry *> filteredActionLogEntries(
    const ActionLog &log, ActionLogFilter filter) {
  auto entries = std::vector<const ActionLogEntry *>{};
  entries.reserve(log.entries.size());
  for (const auto &entry : log.entries) {
    if (filterMatches(entry, filter)) {
      entries.push_back(&entry);
    }
  }
  return entries;
}

void clearActionLog(ActionLog &log) {
  log.entries.clear();
}

} // namespace pipetune_gtk
