/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#ifndef PIPETUNE_REMOTE_SERVER_H
#define PIPETUNE_REMOTE_SERVER_H
#include "remote_editor.h"
#include "pipetune/control_protocol.h"

namespace pipetune {

/** Result of a local listener command. */
struct RemoteListenerResult {
  std::string link = {}; /**< Pairing link on an active listener. */
  std::string error = {}; /**< Reason the request could not be applied. */
};

/** Owns an opt-in HTTP(S)/WebSocket editor on the daemon's GLib control context. */
class RemoteServer final {
public:
  RemoteServer(RemoteEditor::Read read, RemoteEditor::Apply apply, std::function<void()> peersChanged);
  ~RemoteServer();
  /** Changes the listener and returns a response for its local owner. */
  RemoteListenerResult configure(const RemoteListenerOptions &options);
  /** Returns listener settings and authenticated client count on the control context. */
  RemoteListenerStatus status() const;
  /** Publishes accepted edits to connected editors, coalescing rapid changes. */
  void changed();
  /** Reports whether authenticated editors are waiting for state changes. */
  bool hasPeers() const noexcept;
  /** Closes all peers and waits for in-flight operations before releasing the dispatcher. */
  cardio::promise<void> shutdown();
  struct Impl;
private:
  std::unique_ptr<Impl> impl_;
};
}
#endif
