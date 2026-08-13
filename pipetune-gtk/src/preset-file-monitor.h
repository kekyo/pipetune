/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#ifndef PIPETUNE_GTK_PRESET_FILE_MONITOR_H
#define PIPETUNE_GTK_PRESET_FILE_MONITOR_H

#include <filesystem>
#include <string>

namespace pipetune_gtk {

/**
 * Receives a coalesced notification after the EffeTune preset file changes.
 *
 * @param userData Opaque data supplied when the monitor was created.
 */
using EffeTunePresetFileChangeCallback = void (*)(void *userData);

/**
 * Opaque EffeTune preset file monitor state.
 */
struct EffeTunePresetFileMonitor;

/**
 * Reports creation of an EffeTune preset file monitor.
 */
struct EffeTunePresetFileMonitorCreateResult {
  /** Created monitor, or null on failure. */
  EffeTunePresetFileMonitor *monitor;
  /** Monitor creation diagnostic, or empty on success. */
  std::string error;
};

/**
 * Starts monitoring an EffeTune `effetune_presets.json` path.
 *
 * The nearest existing parent is monitored when EffeTune has not created its
 * settings directory yet. Notifications are delivered on the default GLib
 * main context used to create the monitor.
 *
 * @param path EffeTune saved-preset JSON path.
 * @param callback Function invoked after a relevant filesystem change.
 * @param userData Opaque callback data.
 * @return Created monitor or a diagnostic.
 */
EffeTunePresetFileMonitorCreateResult createEffeTunePresetFileMonitor(
    const std::filesystem::path &path,
    EffeTunePresetFileChangeCallback callback, void *userData);

/**
 * Stops and destroys an EffeTune preset file monitor.
 *
 * @param monitor Monitor returned by createEffeTunePresetFileMonitor, or null.
 */
void destroyEffeTunePresetFileMonitor(
    EffeTunePresetFileMonitor *monitor);

} // namespace pipetune_gtk

#endif
