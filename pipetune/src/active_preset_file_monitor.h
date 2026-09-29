/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#ifndef PIPETUNE_ACTIVE_PRESET_FILE_MONITOR_H
#define PIPETUNE_ACTIVE_PRESET_FILE_MONITOR_H

#include <filesystem>
#include <memory>
#include <string>
#include <span>

namespace pipetune {

struct ActivePresetFileMonitorCreateResult;

struct ActivePresetFileMonitorEvent {
  bool changed;
  std::filesystem::path path;
  std::string error;
};

class ActivePresetFileMonitor final {
public:
  struct Impl;

  ~ActivePresetFileMonitor();
  ActivePresetFileMonitor(const ActivePresetFileMonitor &) = delete;
  ActivePresetFileMonitor &operator=(const ActivePresetFileMonitor &) = delete;

  int descriptor() const noexcept;
  std::string setPath(const std::filesystem::path &path);
  /**
   * Monitors a preset and its measurement dependencies, including absent files.
   * @param path Primary preset path reported by consume().
   * @param dependencies Referenced measurement JSON files.
   * @return Empty on success, otherwise a monitoring diagnostic.
   */
  std::string setPaths(const std::filesystem::path &path,
                       std::span<const std::filesystem::path> dependencies);
  void clear() noexcept;
  ActivePresetFileMonitorEvent consume();

private:
  explicit ActivePresetFileMonitor(std::unique_ptr<Impl> implementation);
  std::unique_ptr<Impl> implementation_;

  friend struct ActivePresetFileMonitorCreateResult;
  friend ActivePresetFileMonitorCreateResult
  createActivePresetFileMonitor(const std::filesystem::path &path);
};

struct ActivePresetFileMonitorCreateResult {
  std::unique_ptr<ActivePresetFileMonitor> monitor;
  std::string error;
};

ActivePresetFileMonitorCreateResult
createActivePresetFileMonitor(const std::filesystem::path &path);

} // namespace pipetune

#endif
