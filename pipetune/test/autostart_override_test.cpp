/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include "autostart_override.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <unistd.h>

static bool check(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
  }
  return condition;
}

static void writeFile(const std::filesystem::path &path,
                      std::string_view contents) {
  std::filesystem::create_directories(path.parent_path());
  auto stream = std::ofstream(path, std::ios::binary);
  stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
}

static std::string readFile(const std::filesystem::path &path) {
  auto stream = std::ifstream(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(stream),
                     std::istreambuf_iterator<char>());
}

static bool testManagedMaskLifecycle(
    const std::filesystem::path &target,
    const std::filesystem::path &backup) {
  const auto masked = pipetune::maskGtkAutostart(target, backup);
  const auto repeated = pipetune::maskGtkAutostart(target, backup);
  if (!check(masked.success, masked.error) ||
      !check(repeated.success, repeated.error) ||
      !check(pipetune::isPipeTuneManagedAutostartMask(target),
             "autostart mask must be recognized as PipeTune-managed") ||
      !check(!std::filesystem::exists(backup),
             "masking an absent override must not create a backup")) {
    return false;
  }

  const auto restored = pipetune::restoreGtkAutostart(target, backup);
  return check(restored.success, restored.error) &&
         check(!std::filesystem::exists(target),
               "setup must remove a managed mask without a backup");
}

static bool testCustomOverrideRoundTrip(
    const std::filesystem::path &target,
    const std::filesystem::path &backup) {
  constexpr auto custom =
      "[Desktop Entry]\nType=Application\nHidden=false\nX-Custom=true\n";
  writeFile(target, custom);
  const auto masked = pipetune::maskGtkAutostart(target, backup);
  if (!check(masked.success, masked.error) ||
      !check(pipetune::isPipeTuneManagedAutostartMask(target),
             "custom override must be replaced by a managed mask") ||
      !check(readFile(backup) == custom,
             "custom override backup differs")) {
    return false;
  }

  const auto restored = pipetune::restoreGtkAutostart(target, backup);
  return check(restored.success, restored.error) &&
         check(readFile(target) == custom,
               "setup must restore the exact custom override") &&
         check(!std::filesystem::exists(backup),
               "restored custom override backup must be consumed");
}

static bool testCollisionAndUnmanagedPreservation(
    const std::filesystem::path &target,
    const std::filesystem::path &backup) {
  constexpr auto custom = "[Desktop Entry]\nX-Custom=target\n";
  constexpr auto existingBackup = "[Desktop Entry]\nX-Custom=backup\n";
  writeFile(target, custom);
  writeFile(backup, existingBackup);
  const auto collision = pipetune::maskGtkAutostart(target, backup);
  if (!check(!collision.success,
             "an existing backup must prevent overwriting a custom override") ||
      !check(readFile(target) == custom && readFile(backup) == existingBackup,
             "backup collision must preserve both files")) {
    return false;
  }

  std::filesystem::remove(backup);
  const auto unmanaged = pipetune::restoreGtkAutostart(target, backup);
  if (!check(unmanaged.success, unmanaged.error) ||
      !check(!unmanaged.warnings.empty(),
             "an unmanaged override must produce a setup warning") ||
      !check(readFile(target) == custom,
             "setup must not remove an unmanaged override")) {
    return false;
  }

  std::filesystem::remove(target);
  writeFile(backup, existingBackup);
  const auto orphan = pipetune::restoreGtkAutostart(target, backup);
  return check(orphan.success, orphan.error) &&
         check(!orphan.warnings.empty(),
               "an orphaned backup must produce a setup warning") &&
         check(!std::filesystem::exists(target) &&
                   readFile(backup) == existingBackup,
               "setup must preserve an orphaned backup for manual recovery");
}

int main() {
  const auto directory =
      std::filesystem::temp_directory_path() /
      ("pipetune-autostart-test-" +
       std::to_string(static_cast<long long>(getpid())));
  const auto target =
      directory / "autostart" / "net.kekyo.pipetune_gtk.desktop";
  const auto backup =
      directory / "autostart" /
      "net.kekyo.pipetune_gtk.desktop.pipetune-backup";
  const auto passed =
      testManagedMaskLifecycle(target, backup) &&
      testCustomOverrideRoundTrip(target, backup) &&
      testCollisionAndUnmanagedPreservation(target, backup);
  std::filesystem::remove_all(directory);
  return passed ? 0 : 1;
}
