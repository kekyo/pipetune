#ifndef PIPETUNE_GTK_CONTROL_CLIENT_H
#define PIPETUNE_GTK_CONTROL_CLIENT_H

#include "pipetune/control_protocol.h"

#include <filesystem>
#include <string>
#include <string_view>

namespace pipetune_gtk {

/** Opaque asynchronous control-client state. */
struct ControlClient;

/**
 * Receives one valid subscribed status event.
 *
 * @param message Parsed status event.
 * @param userData Opaque callback argument.
 */
using ControlClientMessageCallback = void (*)(
    const pipetune::ControlResponseParseResult &message, void *userData);

/**
 * Receives subscription connection changes.
 *
 * @param connected True after the first valid status event.
 * @param error Empty when connected, otherwise a transport/protocol diagnostic.
 * @param userData Opaque callback argument.
 */
using ControlClientConnectionCallback =
    void (*)(bool connected, std::string_view error, void *userData);

/**
 * Configures callbacks for a persistent status subscription.
 */
struct ControlClientCallbacks {
  /** Callback for each valid status event. */
  ControlClientMessageCallback message;
  /** Callback for established and lost subscriptions. */
  ControlClientConnectionCallback connectionChanged;
  /** Opaque argument passed to both callbacks. */
  void *userData;
};

/**
 * Reports one completed asynchronous request.
 */
struct ControlClientReply {
  /** Parsed daemon reply; inspect valid and success when transportError is empty. */
  pipetune::ControlResponseParseResult response;
  /** Local transport diagnostic, or empty after a complete reply. */
  std::string transportError;
};

/**
 * Receives one asynchronous one-shot control reply.
 *
 * @param reply Completed request result.
 * @param userData Opaque request argument.
 */
using ControlClientReplyCallback =
    void (*)(const ControlClientReply &reply, void *userData);

/**
 * Creates a main-context asynchronous Unix-socket client.
 *
 * @param socketPath PipeTune control socket path.
 * @param callbacks Subscription callbacks.
 * @return Client that must be released with destroyControlClient().
 */
ControlClient *
createControlClient(const std::filesystem::path &socketPath,
                    const ControlClientCallbacks &callbacks);

/**
 * Cancels pending operations and releases a client.
 *
 * Cancellation callbacks are suppressed after this function returns.
 *
 * @param client Client to release, or null.
 */
void destroyControlClient(ControlClient *client);

/**
 * Starts a persistent status subscription.
 *
 * Calling this while a subscription is active has no effect.
 *
 * @param client Client to start, or null.
 */
void startControlSubscription(ControlClient *client);

/**
 * Stops the current status subscription without a disconnect callback.
 *
 * @param client Client to stop, or null.
 */
void stopControlSubscription(ControlClient *client);

/**
 * Requests a fresh one-shot runtime status.
 *
 * @param client Client used for the request.
 * @param callback Non-null completion callback.
 * @param userData Opaque callback argument.
 */
void requestControlStatusAsync(ControlClient *client,
                               ControlClientReplyCallback callback,
                               void *userData);

/**
 * Requests a live preset change.
 *
 * @param client Client used for the request.
 * @param presetPath Absolute preset path interpreted by the daemon.
 * @param callback Non-null completion callback.
 * @param userData Opaque callback argument.
 */
void loadControlPresetAsync(ControlClient *client,
                            const std::filesystem::path &presetPath,
                            ControlClientReplyCallback callback,
                            void *userData);

/**
 * Requests live DSP bypass.
 *
 * @param client Client used for the request.
 * @param callback Non-null completion callback.
 * @param userData Opaque callback argument.
 */
void bypassControlAsync(ControlClient *client,
                        ControlClientReplyCallback callback,
                        void *userData);

/**
 * Requests an explicit preferred physical output.
 *
 * @param client Client used for the request.
 * @param target Non-empty PipeWire node.name interpreted by the daemon.
 * @param callback Non-null completion callback.
 * @param userData Opaque callback argument.
 */
void setControlOutputAsync(ControlClient *client, std::string_view target,
                           ControlClientReplyCallback callback,
                           void *userData);

/**
 * Requests system-default output mode.
 *
 * @param client Client used for the request.
 * @param callback Non-null completion callback.
 * @param userData Opaque callback argument.
 */
void clearControlOutputAsync(ControlClient *client,
                             ControlClientReplyCallback callback,
                             void *userData);

/**
 * Requests a live PCM sample-rate policy change.
 *
 * @param client Client used for the request.
 * @param policy Valid automatic/fixed graph-rate policy.
 * @param callback Non-null completion callback.
 * @param userData Opaque callback argument.
 */
void setControlRateAsync(ControlClient *client,
                         const pipetune::SampleRatePolicy &policy,
                         ControlClientReplyCallback callback,
                         void *userData);

/**
 * Requests a live native DSP backend change.
 *
 * @param client Client used for the request.
 * @param kind Scalar compatibility or SIMD acceleration backend.
 * @param simdVariant Automatic or pinned SIMD dispatch preference.
 * @param callback Non-null completion callback.
 * @param userData Opaque callback argument.
 */
void setControlDspBackendAsync(ControlClient *client,
                               pipetune::DspBackendKind kind,
                               pipetune::DspSimdVariant simdVariant,
                               ControlClientReplyCallback callback,
                               void *userData);

} // namespace pipetune_gtk

#endif
