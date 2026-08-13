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
