/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#ifndef PIPETUNE_REMOTE_EDITOR_H
#define PIPETUNE_REMOTE_EDITOR_H
#include <cardio.h>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace pipetune {

/** Resolves the editor and preset assets shipped alongside the running executable. */
std::filesystem::path remoteEditorDirectory();

/** Retained recipe and master bypass from one committed processing state. */
struct RemotePipelineSnapshot {
  std::shared_ptr<const std::string> recipe;
  bool bypass;
};

/** Replies are ordered: an optional acknowledgment precedes any requested data. */
struct RemoteReply { std::string ack, data; };

/** Implements EffeTune's remote-v1 base editing protocol on the control dispatcher. */
struct RemoteEditor {
  using Read = std::function<RemotePipelineSnapshot()>;
  using Apply = std::function<cardio::promise<std::string>(std::shared_ptr<const std::string>,
      std::optional<bool>, cardio::cancellation)>;
  Read read;
  Apply apply;
  /** Returns the newest state push only when the accepted content changes. */
  std::string refresh();
  /** Validates and applies one authenticated request. */
  cardio::promise<RemoteReply> request(std::string message, cardio::cancellation cancellation);
  std::uint64_t revision = 0;
  RemotePipelineSnapshot previous = {};
  std::string chain = "[]";
  std::string lastState = {};
};
}
#endif
