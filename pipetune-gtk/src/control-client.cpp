#include "control-client.h"

#include "pipetune/control_protocol.h"

#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace pipetune_gtk {

constexpr auto kMaximumControlMessageBytes = std::size_t{256 * 1024};

struct ControlClientImplementation {
  std::filesystem::path socketPath;
  ControlClientCallbacks callbacks;
  bool destroyed;
  bool subscriptionStarted;
  std::uint64_t subscriptionGeneration;
  GCancellable *subscriptionCancellable;
  GCancellable *requestCancellable;

  ControlClientImplementation(std::filesystem::path path,
                              const ControlClientCallbacks &clientCallbacks)
      : socketPath(std::move(path)), callbacks(clientCallbacks),
        destroyed(false), subscriptionStarted(false),
        subscriptionGeneration(0), subscriptionCancellable(nullptr),
        requestCancellable(g_cancellable_new()) {}

  ~ControlClientImplementation() {
    if (subscriptionCancellable != nullptr) {
      g_object_unref(subscriptionCancellable);
    }
    g_object_unref(requestCancellable);
  }
};

struct ControlClient {
  std::shared_ptr<ControlClientImplementation> implementation;
};

struct SubscriptionOperation {
  std::shared_ptr<ControlClientImplementation> implementation;
  std::uint64_t generation;
  GSocketClient *socketClient;
  GSocketAddress *address;
  GSocketConnection *connection;
  GDataInputStream *input;
  GCancellable *cancellable;
  std::string request;
  bool connectedNotified;
};

struct RequestOperation {
  std::shared_ptr<ControlClientImplementation> implementation;
  GSocketClient *socketClient;
  GSocketAddress *address;
  GSocketConnection *connection;
  GDataInputStream *input;
  GCancellable *cancellable;
  std::string request;
  ControlClientReplyCallback callback;
  void *userData;
};

static pipetune::ControlResponseParseResult emptyResponse() {
  return {.valid = false,
          .success = false,
          .kind = pipetune::ControlResponseKind::response,
          .status = {.processingMode = pipetune::ProcessingMode::bypass,
                     .activePreset = {},
                     .configurationError = {},
                     .activePluginCount = 0,
                     .preferredTarget = {},
                     .selectedTarget = {},
                     .outputSelectionReason =
                         pipetune::ControlOutputSelectionReason::unavailable,
                     .availableOutputs = {},
                     .defaultSinkActive = false,
                     .overrunFrames = 0,
                     .underrunFrames = 0,
                     .processingErrors = 0,
                     .dspProcessedFrames = 0,
                     .dspProcessingNanoseconds = 0,
                     .inputSampleFormat = {},
                     .inputSampleRate = 0,
                     .inputChannelCount = 0,
                     .inputFramesReceived = 0,
                     .inputLastReceivedUnixMilliseconds = 0},
          .warnings = {},
          .error = {}};
}

static std::string errorMessage(GError *error, std::string_view fallback) {
  if (error == nullptr) {
    return std::string(fallback);
  }
  auto message = std::string(error->message);
  g_error_free(error);
  return message;
}

static bool subscriptionIsCurrent(
    const SubscriptionOperation &operation) {
  return !operation.implementation->destroyed &&
         operation.implementation->subscriptionStarted &&
         operation.implementation->subscriptionGeneration ==
             operation.generation;
}

static void destroySubscriptionOperation(
    SubscriptionOperation *operation) {
  if (operation->input != nullptr) {
    g_object_unref(operation->input);
  }
  if (operation->connection != nullptr) {
    g_object_unref(operation->connection);
  }
  if (operation->address != nullptr) {
    g_object_unref(operation->address);
  }
  g_object_unref(operation->socketClient);
  g_object_unref(operation->cancellable);
  delete operation;
}

static void finishSubscription(SubscriptionOperation *operation,
                               std::string error) {
  const auto current = subscriptionIsCurrent(*operation);
  if (current) {
    auto &implementation = *operation->implementation;
    implementation.subscriptionStarted = false;
    if (implementation.subscriptionCancellable == operation->cancellable) {
      g_object_unref(implementation.subscriptionCancellable);
      implementation.subscriptionCancellable = nullptr;
    }
    if (implementation.callbacks.connectionChanged != nullptr) {
      implementation.callbacks.connectionChanged(
          false, error, implementation.callbacks.userData);
    }
  }
  destroySubscriptionOperation(operation);
}

static void readSubscriptionLine(SubscriptionOperation *operation);

static void onSubscriptionRead(GObject *source, GAsyncResult *result,
                               gpointer userData) {
  auto *operation = static_cast<SubscriptionOperation *>(userData);
  auto length = gsize{0};
  auto *error = static_cast<GError *>(nullptr);
  auto *line = g_data_input_stream_read_line_finish_utf8(
      G_DATA_INPUT_STREAM(source), result, &length, &error);
  if (!subscriptionIsCurrent(*operation)) {
    g_free(line);
    if (error != nullptr) {
      g_error_free(error);
    }
    destroySubscriptionOperation(operation);
    return;
  }
  if (line == nullptr) {
    finishSubscription(
        operation,
        errorMessage(error, "PipeTune control subscription closed"));
    return;
  }
  if (length > kMaximumControlMessageBytes) {
    g_free(line);
    finishSubscription(operation,
                       "PipeTune control status event is too large");
    return;
  }

  const auto message = pipetune::parseControlResponse(
      std::string_view(line, static_cast<std::size_t>(length)));
  g_free(line);
  if (!message.valid || !message.success ||
      message.kind != pipetune::ControlResponseKind::statusEvent) {
    const auto diagnostic =
        message.error.empty()
            ? std::string("PipeTune subscription returned an invalid event")
            : message.error;
    finishSubscription(operation, diagnostic);
    return;
  }

  auto &implementation = *operation->implementation;
  if (!operation->connectedNotified) {
    operation->connectedNotified = true;
    if (implementation.callbacks.connectionChanged != nullptr) {
      implementation.callbacks.connectionChanged(
          true, {}, implementation.callbacks.userData);
    }
  }
  if (implementation.callbacks.message != nullptr) {
    implementation.callbacks.message(message,
                                     implementation.callbacks.userData);
  }
  if (!subscriptionIsCurrent(*operation)) {
    destroySubscriptionOperation(operation);
    return;
  }
  readSubscriptionLine(operation);
}

static void readSubscriptionLine(SubscriptionOperation *operation) {
  g_data_input_stream_read_line_async(
      operation->input, G_PRIORITY_DEFAULT, operation->cancellable,
      onSubscriptionRead, operation);
}

static void onSubscriptionWrite(GObject *source, GAsyncResult *result,
                                gpointer userData) {
  auto *operation = static_cast<SubscriptionOperation *>(userData);
  auto written = gsize{0};
  auto *error = static_cast<GError *>(nullptr);
  const auto success = g_output_stream_write_all_finish(
      G_OUTPUT_STREAM(source), result, &written, &error);
  if (!subscriptionIsCurrent(*operation)) {
    if (error != nullptr) {
      g_error_free(error);
    }
    destroySubscriptionOperation(operation);
    return;
  }
  if (!success || written != operation->request.size()) {
    finishSubscription(
        operation,
        errorMessage(error, "cannot send PipeTune subscription request"));
    return;
  }
  readSubscriptionLine(operation);
}

static void onSubscriptionConnected(GObject *source, GAsyncResult *result,
                                    gpointer userData) {
  auto *operation = static_cast<SubscriptionOperation *>(userData);
  auto *error = static_cast<GError *>(nullptr);
  operation->connection = g_socket_client_connect_finish(
      G_SOCKET_CLIENT(source), result, &error);
  if (!subscriptionIsCurrent(*operation)) {
    if (error != nullptr) {
      g_error_free(error);
    }
    destroySubscriptionOperation(operation);
    return;
  }
  if (operation->connection == nullptr) {
    finishSubscription(
        operation,
        errorMessage(error, "cannot connect to PipeTune control socket"));
    return;
  }
  operation->input = G_DATA_INPUT_STREAM(g_data_input_stream_new(
      g_io_stream_get_input_stream(G_IO_STREAM(operation->connection))));
  auto *output =
      g_io_stream_get_output_stream(G_IO_STREAM(operation->connection));
  g_output_stream_write_all_async(
      output, operation->request.data(), operation->request.size(),
      G_PRIORITY_DEFAULT, operation->cancellable, onSubscriptionWrite,
      operation);
}

static void startRequest(ControlClient *client, std::string request,
                         ControlClientReplyCallback callback,
                         void *userData);

static void destroyRequestOperation(RequestOperation *operation) {
  if (operation->input != nullptr) {
    g_object_unref(operation->input);
  }
  if (operation->connection != nullptr) {
    g_object_unref(operation->connection);
  }
  if (operation->address != nullptr) {
    g_object_unref(operation->address);
  }
  g_object_unref(operation->socketClient);
  g_object_unref(operation->cancellable);
  delete operation;
}

static void finishRequest(
    RequestOperation *operation,
    pipetune::ControlResponseParseResult response,
    std::string transportError) {
  if (!operation->implementation->destroyed &&
      operation->callback != nullptr) {
    const auto reply = ControlClientReply{
        .response = std::move(response),
        .transportError = std::move(transportError),
    };
    operation->callback(reply, operation->userData);
  }
  destroyRequestOperation(operation);
}

static void onRequestRead(GObject *source, GAsyncResult *result,
                          gpointer userData) {
  auto *operation = static_cast<RequestOperation *>(userData);
  auto length = gsize{0};
  auto *error = static_cast<GError *>(nullptr);
  auto *line = g_data_input_stream_read_line_finish_utf8(
      G_DATA_INPUT_STREAM(source), result, &length, &error);
  if (operation->implementation->destroyed) {
    g_free(line);
    if (error != nullptr) {
      g_error_free(error);
    }
    destroyRequestOperation(operation);
    return;
  }
  if (line == nullptr) {
    finishRequest(
        operation, emptyResponse(),
        errorMessage(error, "PipeTune control socket closed without a reply"));
    return;
  }
  if (length > kMaximumControlMessageBytes) {
    g_free(line);
    finishRequest(operation, emptyResponse(),
                  "PipeTune control reply is too large");
    return;
  }
  auto response = pipetune::parseControlResponse(
      std::string_view(line, static_cast<std::size_t>(length)));
  g_free(line);
  finishRequest(operation, std::move(response), {});
}

static void onRequestWrite(GObject *source, GAsyncResult *result,
                           gpointer userData) {
  auto *operation = static_cast<RequestOperation *>(userData);
  auto written = gsize{0};
  auto *error = static_cast<GError *>(nullptr);
  const auto success = g_output_stream_write_all_finish(
      G_OUTPUT_STREAM(source), result, &written, &error);
  if (operation->implementation->destroyed) {
    if (error != nullptr) {
      g_error_free(error);
    }
    destroyRequestOperation(operation);
    return;
  }
  if (!success || written != operation->request.size()) {
    finishRequest(
        operation, emptyResponse(),
        errorMessage(error, "cannot send PipeTune control request"));
    return;
  }
  g_data_input_stream_read_line_async(
      operation->input, G_PRIORITY_DEFAULT, operation->cancellable,
      onRequestRead, operation);
}

static void onRequestConnected(GObject *source, GAsyncResult *result,
                               gpointer userData) {
  auto *operation = static_cast<RequestOperation *>(userData);
  auto *error = static_cast<GError *>(nullptr);
  operation->connection = g_socket_client_connect_finish(
      G_SOCKET_CLIENT(source), result, &error);
  if (operation->implementation->destroyed) {
    if (error != nullptr) {
      g_error_free(error);
    }
    destroyRequestOperation(operation);
    return;
  }
  if (operation->connection == nullptr) {
    finishRequest(
        operation, emptyResponse(),
        errorMessage(error, "cannot connect to PipeTune control socket"));
    return;
  }
  operation->input = G_DATA_INPUT_STREAM(g_data_input_stream_new(
      g_io_stream_get_input_stream(G_IO_STREAM(operation->connection))));
  auto *output =
      g_io_stream_get_output_stream(G_IO_STREAM(operation->connection));
  g_output_stream_write_all_async(
      output, operation->request.data(), operation->request.size(),
      G_PRIORITY_DEFAULT, operation->cancellable, onRequestWrite, operation);
}

static void startRequest(ControlClient *client, std::string request,
                         ControlClientReplyCallback callback,
                         void *userData) {
  if (client == nullptr || callback == nullptr) {
    return;
  }
  auto implementation = client->implementation;
  if (implementation->destroyed) {
    return;
  }
  request.push_back('\n');
  auto *operation = new RequestOperation{
      .implementation = implementation,
      .socketClient = g_socket_client_new(),
      .address = g_unix_socket_address_new(
          implementation->socketPath.c_str()),
      .connection = nullptr,
      .input = nullptr,
      .cancellable =
          G_CANCELLABLE(g_object_ref(implementation->requestCancellable)),
      .request = std::move(request),
      .callback = callback,
      .userData = userData,
  };
  g_socket_client_connect_async(
      operation->socketClient, G_SOCKET_CONNECTABLE(operation->address),
      operation->cancellable, onRequestConnected, operation);
}

ControlClient *
createControlClient(const std::filesystem::path &socketPath,
                    const ControlClientCallbacks &callbacks) {
  return new ControlClient{
      .implementation =
          std::make_shared<ControlClientImplementation>(socketPath, callbacks),
  };
}

void destroyControlClient(ControlClient *client) {
  if (client == nullptr) {
    return;
  }
  auto implementation = client->implementation;
  implementation->destroyed = true;
  implementation->subscriptionStarted = false;
  ++implementation->subscriptionGeneration;
  g_cancellable_cancel(implementation->subscriptionCancellable);
  g_cancellable_cancel(implementation->requestCancellable);
  delete client;
}

void startControlSubscription(ControlClient *client) {
  if (client == nullptr) {
    return;
  }
  auto implementation = client->implementation;
  if (implementation->destroyed || implementation->subscriptionStarted) {
    return;
  }
  implementation->subscriptionStarted = true;
  ++implementation->subscriptionGeneration;
  implementation->subscriptionCancellable = g_cancellable_new();
  auto request = pipetune::makeSubscribeControlRequest();
  request.push_back('\n');
  auto *operation = new SubscriptionOperation{
      .implementation = implementation,
      .generation = implementation->subscriptionGeneration,
      .socketClient = g_socket_client_new(),
      .address = g_unix_socket_address_new(
          implementation->socketPath.c_str()),
      .connection = nullptr,
      .input = nullptr,
      .cancellable = G_CANCELLABLE(
          g_object_ref(implementation->subscriptionCancellable)),
      .request = std::move(request),
      .connectedNotified = false,
  };
  g_socket_client_connect_async(
      operation->socketClient, G_SOCKET_CONNECTABLE(operation->address),
      operation->cancellable, onSubscriptionConnected, operation);
}

void stopControlSubscription(ControlClient *client) {
  if (client == nullptr) {
    return;
  }
  auto &implementation = *client->implementation;
  if (!implementation.subscriptionStarted) {
    return;
  }
  implementation.subscriptionStarted = false;
  ++implementation.subscriptionGeneration;
  g_cancellable_cancel(implementation.subscriptionCancellable);
  g_object_unref(implementation.subscriptionCancellable);
  implementation.subscriptionCancellable = nullptr;
}

void requestControlStatusAsync(ControlClient *client,
                               ControlClientReplyCallback callback,
                               void *userData) {
  startRequest(client, pipetune::makeStatusControlRequest(), callback,
               userData);
}

void loadControlPresetAsync(ControlClient *client,
                            const std::filesystem::path &presetPath,
                            ControlClientReplyCallback callback,
                            void *userData) {
  startRequest(client, pipetune::makeLoadPresetControlRequest(presetPath),
               callback, userData);
}

void bypassControlAsync(ControlClient *client,
                        ControlClientReplyCallback callback,
                        void *userData) {
  startRequest(client, pipetune::makeBypassControlRequest(), callback,
               userData);
}

void setControlOutputAsync(ControlClient *client, std::string_view target,
                           ControlClientReplyCallback callback,
                           void *userData) {
  startRequest(client, pipetune::makeSetOutputControlRequest(target),
               callback, userData);
}

void clearControlOutputAsync(ControlClient *client,
                             ControlClientReplyCallback callback,
                             void *userData) {
  startRequest(client, pipetune::makeClearOutputControlRequest(), callback,
               userData);
}

void setControlRateAsync(ControlClient *client,
                         const pipetune::SampleRatePolicy &policy,
                         ControlClientReplyCallback callback,
                         void *userData) {
  startRequest(client, pipetune::makeSetRateControlRequest(policy), callback,
               userData);
}

void setControlDspBackendAsync(ControlClient *client,
                               pipetune::DspBackendKind kind,
                               pipetune::DspSimdVariant simdVariant,
                               ControlClientReplyCallback callback,
                               void *userData) {
  startRequest(
      client,
      pipetune::makeSetDspBackendControlRequest(kind, simdVariant),
      callback, userData);
}

} // namespace pipetune_gtk
