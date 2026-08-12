#include "user-setup-client.h"

#include <gio/gio.h>

#include <array>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace pipetune_gtk {

struct UserSetupClientImplementation {
  std::string executable;
  bool destroyed;
  bool pending;
  GCancellable *cancellable;

  explicit UserSetupClientImplementation(std::string path)
      : executable(std::move(path)), destroyed(false), pending(false),
        cancellable(g_cancellable_new()) {}

  ~UserSetupClientImplementation() {
    g_object_unref(cancellable);
  }
};

struct UserSetupClient {
  std::shared_ptr<UserSetupClientImplementation> implementation;
};

struct UserSetupOperation {
  std::shared_ptr<UserSetupClientImplementation> implementation;
  GSubprocess *process;
  UserSetupClientCallback callback;
  void *userData;
  std::string startError;
};

static std::string trimOutput(std::string_view text) {
  while (!text.empty() &&
         (text.front() == ' ' || text.front() == '\t' ||
          text.front() == '\n' || text.front() == '\r')) {
    text.remove_prefix(1);
  }
  while (!text.empty() &&
         (text.back() == ' ' || text.back() == '\t' ||
          text.back() == '\n' || text.back() == '\r')) {
    text.remove_suffix(1);
  }
  return std::string(text);
}

static std::string glibErrorMessage(GError *error,
                                    std::string_view fallback) {
  if (error == nullptr) {
    return std::string(fallback);
  }
  auto message = std::string(error->message);
  g_error_free(error);
  return message;
}

static void finishOperation(UserSetupOperation *operation,
                            UserSetupClientResult result) {
  auto implementation = operation->implementation;
  implementation->pending = false;
  if (!implementation->destroyed && operation->callback != nullptr) {
    operation->callback(result, operation->userData);
  }
  if (operation->process != nullptr) {
    g_object_unref(operation->process);
  }
  delete operation;
}

static gboolean reportStartFailure(gpointer userData) {
  auto *operation = static_cast<UserSetupOperation *>(userData);
  finishOperation(
      operation,
      {.success = false,
       .standardOutput = {},
       .error = std::move(operation->startError)});
  return G_SOURCE_REMOVE;
}

static std::string unsuccessfulProcessMessage(GSubprocess *process) {
  if (g_subprocess_get_if_exited(process)) {
    return "PipeTune user setup exited with status " +
           std::to_string(g_subprocess_get_exit_status(process));
  }
  return "PipeTune user setup was terminated";
}

static void onCommunicated(GObject *source, GAsyncResult *result,
                           gpointer userData) {
  auto *operation = static_cast<UserSetupOperation *>(userData);
  auto *standardOutput = static_cast<gchar *>(nullptr);
  auto *standardError = static_cast<gchar *>(nullptr);
  auto *error = static_cast<GError *>(nullptr);
  const auto communicated = g_subprocess_communicate_utf8_finish(
      G_SUBPROCESS(source), result, &standardOutput, &standardError,
      &error);

  auto completed = UserSetupClientResult{
      .success = false, .standardOutput = {}, .error = {}};
  if (!communicated) {
    completed.error =
        glibErrorMessage(error, "cannot communicate with PipeTune user setup");
  } else {
    completed.standardOutput =
        trimOutput(standardOutput == nullptr
                       ? std::string_view{}
                       : std::string_view(standardOutput));
    completed.success =
        g_subprocess_get_successful(operation->process) != FALSE;
    if (!completed.success) {
      completed.error =
          trimOutput(standardError == nullptr
                         ? std::string_view{}
                         : std::string_view(standardError));
      if (completed.error.empty()) {
        completed.error = unsuccessfulProcessMessage(operation->process);
      }
    }
  }
  g_free(standardOutput);
  g_free(standardError);
  finishOperation(operation, std::move(completed));
}

UserSetupClient *createUserSetupClient(
    const std::filesystem::path &pipeTuneExecutable) {
  const auto executable = pipeTuneExecutable.string();
  if (executable.empty() || !pipeTuneExecutable.is_absolute() ||
      executable.find('\0') != std::string::npos) {
    return nullptr;
  }
  return new UserSetupClient{
      .implementation =
          std::make_shared<UserSetupClientImplementation>(executable)};
}

void destroyUserSetupClient(UserSetupClient *client) {
  if (client == nullptr) {
    return;
  }
  client->implementation->destroyed = true;
  g_cancellable_cancel(client->implementation->cancellable);
  delete client;
}

bool setupUserIfNeededAsync(UserSetupClient *client,
                            UserSetupClientCallback callback,
                            void *userData) {
  if (client == nullptr || callback == nullptr) {
    return false;
  }
  auto implementation = client->implementation;
  if (implementation->destroyed || implementation->pending) {
    return false;
  }
  implementation->pending = true;

  const auto arguments = std::array<const gchar *, 4>{
      implementation->executable.c_str(), "setup", "--no-launch-gtk",
      nullptr};
  auto *error = static_cast<GError *>(nullptr);
  const auto flags = static_cast<GSubprocessFlags>(
      G_SUBPROCESS_FLAGS_STDOUT_PIPE |
      G_SUBPROCESS_FLAGS_STDERR_PIPE);
  auto *process = g_subprocess_newv(arguments.data(), flags, &error);
  auto *operation = new UserSetupOperation{
      .implementation = std::move(implementation),
      .process = process,
      .callback = callback,
      .userData = userData,
      .startError = {},
  };
  if (process == nullptr) {
    operation->startError =
        glibErrorMessage(error, "cannot start PipeTune user setup");
    g_idle_add(reportStartFailure, operation);
    return true;
  }

  g_subprocess_communicate_utf8_async(
      process, nullptr, operation->implementation->cancellable,
      onCommunicated, operation);
  return true;
}

} // namespace pipetune_gtk
