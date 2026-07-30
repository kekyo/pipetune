#include "audio_bridge.h"
#include "pipewire_idle_epoch.h"

#include <cstdint>
#include <iostream>
#include <string_view>
#include <vector>

static bool check(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
  }
  return condition;
}

static bool testPausedEpochStartsWithFreshAudio() {
  auto epoch = pipetune::PipeWireIdleEpoch{};
  auto ring = pipetune::PlanarAudioRing(2, 4);
  const auto previousAudio =
      std::vector<float>{1.0F, 2.0F, 11.0F, 12.0F};
  const auto resumedAudio =
      std::vector<float>{3.0F, 4.0F, 13.0F, 14.0F};

  if (!check(ring.write(previousAudio, 2) == 2,
             "previous audio must be queued before pausing") ||
      !check(!epoch.observeStreamStates(true, false),
             "one paused stream must not start an idle epoch") ||
      !check(epoch.observeStreamStates(true, true),
             "both paused streams must start an idle epoch")) {
    return false;
  }
  static_cast<void>(ring.discardQueuedFrames());
  if (!check(!epoch.observeStreamStates(true, true),
             "one paused epoch must be handled only once") ||
      !check(epoch.playbackShouldTreatMissingAsGap(),
             "playback must wait for capture after pausing")) {
    return false;
  }

  auto waitingOutput = std::vector<float>(4, -1.0F);
  const auto waiting = ring.readWithGaps(
      waitingOutput, 2, epoch.playbackShouldTreatMissingAsGap());
  if (!check(waiting.queuedFrames == 0 &&
                 waiting.missingFrames == 2 &&
                 waiting.gapFrames == 2,
             "playback-before-capture must produce an intentional gap") ||
      !check(waitingOutput == std::vector<float>(4, 0.0F),
             "playback-before-capture must not expose previous audio") ||
      !check(ring.underrunFrames() == 0,
             "intentional resume gaps must not count as underruns") ||
      !check(epoch.takeDspResetRequest(),
             "the resumed capture callback must reset DSP state") ||
      !check(!epoch.takeDspResetRequest(),
             "one idle epoch must request only one DSP reset")) {
    return false;
  }

  if (!check(ring.write(resumedAudio, 2) == 2,
             "resumed audio must fit in the cleared ring")) {
    return false;
  }
  epoch.captureFramesQueued();
  auto resumedOutput = std::vector<float>(4, 0.0F);
  return check(!epoch.playbackShouldTreatMissingAsGap(),
               "queued resumed audio must release playback") &&
         check(ring.read(resumedOutput, 2) == 2,
               "resumed audio must be readable") &&
         check(resumedOutput == resumedAudio,
               "only resumed audio may follow a paused epoch");
}

static bool testSeparatePausedEpochsRequestSeparateResets() {
  auto epoch = pipetune::PipeWireIdleEpoch{};
  if (!check(epoch.observeStreamStates(true, true),
             "first paused epoch was not detected") ||
      !check(epoch.takeDspResetRequest(),
             "first paused epoch did not request a reset")) {
    return false;
  }
  epoch.captureFramesQueued();
  if (!check(!epoch.observeStreamStates(false, true),
             "leaving the paused state must not enter another epoch") ||
      !check(epoch.observeStreamStates(true, true),
             "second paused epoch was not detected")) {
    return false;
  }
  return check(epoch.takeDspResetRequest(),
               "second paused epoch did not request a reset") &&
         check(epoch.playbackShouldTreatMissingAsGap(),
               "second paused epoch must wait for fresh capture");
}

int main() {
  return testPausedEpochStartsWithFreshAudio() &&
                 testSeparatePausedEpochsRequestSeparateResets()
             ? 0
             : 1;
}
