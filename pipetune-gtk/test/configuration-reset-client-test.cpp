/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include "configuration-reset-client.h"

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
  pipetune_gtk::ConfigurationResetClientResult result = {
      .success = false, .standardOutput = {}, .error = {}};
};

static void onResetCompleted(
    const pipetune_gtk::ConfigurationResetClientResult &result,
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

static bool awaitReset(
    pipetune_gtk::ConfigurationResetClient *client,
    CallbackProbe &probe) {
  const auto started = pipetune_gtk::resetConfigurationAsync(
      client, onResetCompleted, &probe);
  if (!check(started, "configuration reset must start") ||
      !check(!probe.called,
             "configuration reset callback must not run inline")) {
    return false;
  }
  const auto timeoutSource = g_timeout_add_seconds(5, onTimeout, &probe);
  g_main_loop_run(probe.loop);
  if (!probe.timedOut) {
    g_source_remove(timeoutSource);
  }
  return check(!probe.timedOut,
               "configuration reset callback timed out") &&
         check(probe.called,
               "configuration reset callback was not delivered");
}

static bool testSuccessfulReset(
    pipetune_gtk::ConfigurationResetClient *client) {
  auto probe = CallbackProbe{.loop = g_main_loop_new(nullptr, FALSE)};
  const auto awaited = awaitReset(client, probe);
  const auto passed =
      awaited &&
      check(probe.result.success,
            "successful helper must report reset success") &&
      check(probe.result.standardOutput.find(
                "configuration reset helper succeeded") !=
                std::string::npos,
            "successful helper output differs") &&
      check(probe.result.error.empty(),
            "successful helper must not report an error");
  g_main_loop_unref(probe.loop);
  return passed;
}

static bool testFailedReset(
    pipetune_gtk::ConfigurationResetClient *client) {
  g_setenv("PIPETUNE_GTK_RESET_HELPER_FAIL", "1", TRUE);
  auto probe = CallbackProbe{.loop = g_main_loop_new(nullptr, FALSE)};
  const auto awaited = awaitReset(client, probe);
  g_unsetenv("PIPETUNE_GTK_RESET_HELPER_FAIL");
  const auto passed =
      awaited &&
      check(!probe.result.success,
            "failed helper must report reset failure") &&
      check(probe.result.error.find("simulated partial reset failure") !=
                std::string::npos,
            "failed helper diagnostic differs");
  g_main_loop_unref(probe.loop);
  return passed;
}

int main(int argc, char **argv) {
  if (argc != 2) {
    std::cerr << "configuration reset helper path is required\n";
    return 1;
  }
  auto *client = pipetune_gtk::createConfigurationResetClient(
      std::filesystem::path(argv[1]));
  const auto passed =
      check(client != nullptr,
            "configuration reset client is unavailable") &&
      testSuccessfulReset(client) && testFailedReset(client);
  pipetune_gtk::destroyConfigurationResetClient(client);
  return passed ? 0 : 1;
}
