/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include "user-setup-client.h"

#include <glib.h>

#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

static bool check(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
  }
  return condition;
}

struct CallbackProbe {
  GMainLoop *loop;
  bool called = false;
  bool timedOut = false;
  pipetune_gtk::UserSetupClientResult result = {
      .success = false, .standardOutput = {}, .error = {}};
};

static void onSetupCompleted(
    const pipetune_gtk::UserSetupClientResult &result,
    void *userData) {
  auto &probe = *static_cast<CallbackProbe *>(userData);
  probe.called = true;
  probe.result = result;
  g_main_loop_quit(probe.loop);
}

static gboolean onTimeout(gpointer userData) {
  auto &probe = *static_cast<CallbackProbe *>(userData);
  probe.timedOut = true;
  g_main_loop_quit(probe.loop);
  return G_SOURCE_REMOVE;
}

static bool awaitSetup(pipetune_gtk::UserSetupClient *client,
                       CallbackProbe &probe) {
  const auto started = pipetune_gtk::setupUserIfNeededAsync(
      client, onSetupCompleted, &probe);
  const auto duplicate = pipetune_gtk::setupUserIfNeededAsync(
      client, onSetupCompleted, &probe);
  if (!check(started, "per-user setup must start") ||
      !check(!duplicate, "duplicate pending setup must be rejected") ||
      !check(!probe.called,
             "per-user setup callback must not run inline")) {
    return false;
  }
  const auto timeoutSource = g_timeout_add_seconds(5, onTimeout, &probe);
  g_main_loop_run(probe.loop);
  if (!probe.timedOut) {
    g_source_remove(timeoutSource);
  }
  return check(!probe.timedOut, "per-user setup callback timed out") &&
         check(probe.called,
               "per-user setup callback was not delivered");
}

static bool testSuccessfulSetup(
    pipetune_gtk::UserSetupClient *client) {
  auto probe = CallbackProbe{.loop = g_main_loop_new(nullptr, FALSE)};
  const auto awaited = awaitSetup(client, probe);
  const auto passed =
      awaited &&
      check(probe.result.success,
            "successful helper must report setup success") &&
      check(probe.result.standardOutput.find(
                "user setup helper succeeded") != std::string::npos,
            "successful helper output differs") &&
      check(probe.result.error.empty(),
            "successful helper must not report an error");
  g_main_loop_unref(probe.loop);
  return passed;
}

static bool testFailedSetup(
    pipetune_gtk::UserSetupClient *client) {
  g_setenv("PIPETUNE_GTK_SETUP_HELPER_FAIL", "1", TRUE);
  auto probe = CallbackProbe{.loop = g_main_loop_new(nullptr, FALSE)};
  const auto awaited = awaitSetup(client, probe);
  g_unsetenv("PIPETUNE_GTK_SETUP_HELPER_FAIL");
  const auto passed =
      awaited &&
      check(!probe.result.success,
            "failed helper must report setup failure") &&
      check(probe.result.error.find("simulated user setup failure") !=
                std::string::npos,
            "failed helper diagnostic differs");
  g_main_loop_unref(probe.loop);
  return passed;
}

static bool testStartFailure() {
  auto *client = pipetune_gtk::createUserSetupClient(
      "/missing/pipetune-gtk-user-setup-test");
  auto probe = CallbackProbe{.loop = g_main_loop_new(nullptr, FALSE)};
  const auto awaited = awaitSetup(client, probe);
  const auto passed =
      awaited &&
      check(!probe.result.success,
            "spawn failure must report setup failure") &&
      check(!probe.result.error.empty(),
            "spawn failure must include a diagnostic");
  g_main_loop_unref(probe.loop);
  pipetune_gtk::destroyUserSetupClient(client);
  return passed;
}

int main(int argc, char **argv) {
  if (argc != 2) {
    std::cerr << "user setup helper path is required\n";
    return 1;
  }
  auto *client = pipetune_gtk::createUserSetupClient(
      std::filesystem::path(argv[1]));
  const auto passed =
      check(client != nullptr, "per-user setup client is unavailable") &&
      check(pipetune_gtk::createUserSetupClient("relative/pipetune") ==
                nullptr,
            "relative setup executable must be rejected") &&
      testSuccessfulSetup(client) && testFailedSetup(client) &&
      testStartFailure();
  pipetune_gtk::destroyUserSetupClient(client);
  return passed ? 0 : 1;
}
