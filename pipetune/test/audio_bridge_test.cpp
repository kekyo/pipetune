/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include "audio_bridge.h"

#include <cstdint>
#include <exception>
#include <iostream>
#include <span>
#include <string_view>
#include <vector>

static bool check(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
  }
  return condition;
}

static bool testPlanarWraparound() {
  auto ring = pipetune::PlanarAudioRing(2, 5);
  const auto first = std::vector<float>{1.0F, 2.0F, 3.0F, 4.0F,
                                        11.0F, 12.0F, 13.0F, 14.0F};
  if (!check(ring.write(first, 4, 0) == 4,
             "initial frames must be accepted")) {
    return false;
  }

  auto prefix = std::vector<float>(6, -1.0F);
  if (!check(ring.read(prefix, 3, 0) == 3,
             "queued prefix must be readable") ||
      !check(prefix == std::vector<float>({1.0F, 2.0F, 3.0F, 11.0F, 12.0F, 13.0F}),
             "planar prefix differs")) {
    return false;
  }

  const auto second =
      std::vector<float>{5.0F, 6.0F, 7.0F, 8.0F, 15.0F, 16.0F, 17.0F, 18.0F};
  if (!check(ring.write(second, 4, 0) == 4,
             "wrapped frames must be accepted")) {
    return false;
  }
  auto output = std::vector<float>(10, -1.0F);
  return check(ring.read(output, 5, 0) == 5,
               "wrapped frames must all be readable") &&
         check(output == std::vector<float>({4.0F, 5.0F, 6.0F, 7.0F, 8.0F,
                                             14.0F, 15.0F, 16.0F, 17.0F, 18.0F}),
               "wraparound must preserve every channel's frame order") &&
         check(ring.overrunFrames() == 0 && ring.underrunFrames() == 0,
               "balanced transfers must not report xruns");
}

static bool testUnderrunSilence() {
  auto ring = pipetune::PlanarAudioRing(2, 4);
  const auto input = std::vector<float>{1.0F, 2.0F, 11.0F, 12.0F};
  auto output = std::vector<float>(8, -1.0F);
  return check(ring.write(input, 2, 0) == 2,
               "underrun input must be accepted") &&
         check(ring.read(output, 4, 0) == 2,
               "read must report only queued frames") &&
         check(output ==
                   std::vector<float>({1.0F, 2.0F, 0.0F, 0.0F,
                                       11.0F, 12.0F, 0.0F, 0.0F}),
               "unavailable frames must become per-channel silence") &&
         check(ring.underrunFrames() == 2, "missing frames must be counted once per frame");
}

static bool testMeasuredBridgeDelayPreservesQueuedAudio() {
  auto ring = pipetune::PlanarAudioRing(1, 8);
  auto latency = pipetune::AudioBridgeLatencyController{};
  latency.configure(3);
  const auto input = std::vector<float>{1.0F, 2.0F, 3.0F, 4.0F};
  if (!check(ring.write(input, 4, 7) == 4,
             "bridge delay input must be queued")) {
    return false;
  }

  auto first = std::vector<float>(2, -1.0F);
  const auto firstRead = ring.readWithSilencePrefix(
      first, 2, 7, latency.consumePrefixFrames(2));
  if (!check(firstRead.queuedFrames == 0 &&
                 firstRead.availableFrames == 2 &&
                 first == std::vector<float>({0.0F, 0.0F}),
             "the first bridge block must be an intentional delay") ||
      !check(ring.underrunFrames() == 0,
             "intentional bridge delay must not count as underrun")) {
    return false;
  }

  auto second = std::vector<float>(4, -1.0F);
  const auto secondRead = ring.readWithSilencePrefix(
      second, 4, 7, latency.consumePrefixFrames(4));
  if (!check(secondRead.queuedFrames == 3 &&
                 secondRead.availableFrames == 4 &&
                 second ==
                     std::vector<float>({0.0F, 1.0F, 2.0F, 3.0F}),
             "bridge delay must expire without consuming queued PCM") ||
      !check(ring.underrunFrames() == 0,
             "available PCM after the delay must not underrun")) {
    return false;
  }

  latency.reset();
  auto reset = std::vector<float>(3, -1.0F);
  const auto resetRead = ring.readWithSilencePrefix(
      reset, 3, 7, latency.consumePrefixFrames(3));
  return check(resetRead.queuedFrames == 0 &&
                   resetRead.availableFrames == 3 &&
                   reset == std::vector<float>({0.0F, 0.0F, 0.0F}),
               "bridge reset must restore the measured delay") &&
         check(ring.underrunFrames() == 0,
               "reset bridge delay must remain intentional");
}

static bool testOverrunDropsNewestTail() {
  auto ring = pipetune::PlanarAudioRing(2, 4);
  const auto first = std::vector<float>{1.0F, 2.0F, 3.0F, 11.0F, 12.0F, 13.0F};
  const auto second = std::vector<float>{4.0F, 5.0F, 6.0F, 14.0F, 15.0F, 16.0F};
  auto output = std::vector<float>(8, 0.0F);
  return check(ring.write(first, 3, 0) == 3,
               "initial overrun input must fit") &&
         check(ring.write(second, 3, 0) == 1,
               "only free capacity must be accepted") &&
         check(ring.overrunFrames() == 2, "discarded input frames must be counted") &&
         check(ring.read(output, 4, 0) == 4,
               "full ring must remain readable") &&
         check(output ==
                   std::vector<float>({1.0F, 2.0F, 3.0F, 4.0F,
                                       11.0F, 12.0F, 13.0F, 14.0F}),
               "overrun must discard only the newest tail");
}

static bool testMalformedBufferIsRejected() {
  auto ring = pipetune::PlanarAudioRing(2, 4);
  const auto malformedInput = std::vector<float>(3, 1.0F);
  auto malformedOutput = std::vector<float>(3, 1.0F);
  return check(ring.write(malformedInput, 2, 0) == 0,
               "incorrectly shaped input must be rejected") &&
         check(ring.read(malformedOutput, 2, 0) == 0,
               "incorrectly shaped output must be rejected") &&
         check(malformedOutput == std::vector<float>({1.0F, 1.0F, 1.0F}),
               "rejected output must remain unchanged") &&
         check(ring.overrunFrames() == 0 && ring.underrunFrames() == 0,
               "rejected shapes must not change xrun counters");
}

static bool testQueuedAudioCanBeDiscardedBeforeChangingOutput() {
  auto ring = pipetune::PlanarAudioRing(2, 4);
  const auto oldOutput =
      std::vector<float>{1.0F, 2.0F, 11.0F, 12.0F};
  const auto newOutput =
      std::vector<float>{3.0F, 4.0F, 13.0F, 14.0F};
  auto output = std::vector<float>(4, 0.0F);

  return check(ring.write(oldOutput, 2, 0) == 2,
               "old output frames must be queued") &&
         check(ring.discardQueuedFrames() == 2,
               "discard must report every removed frame") &&
         check(ring.write(newOutput, 2, 0) == 2,
               "new output frames must be accepted after discard") &&
         check(ring.read(output, 2, 0) == 2,
               "new output frames must remain readable") &&
         check(output == newOutput,
               "discarded output must not play on the new device") &&
         check(ring.overrunFrames() == 0 && ring.underrunFrames() == 0,
               "intentional discard must not be counted as an xrun");
}

static bool testPipelineTransitionProducesSilence() {
  auto ring = pipetune::PlanarAudioRing(2, 8);
  const auto oldOutput =
      std::vector<float>{1.0F, 2.0F, 11.0F, 12.0F};
  const auto newOutput = std::vector<float>{
      3.0F, 4.0F, 5.0F, 6.0F, 13.0F, 14.0F, 15.0F, 16.0F};
  auto output = std::vector<float>(12, -1.0F);
  if (!check(ring.write(oldOutput, 2, 7) == 2,
             "old pipeline frames must be queued") ||
      !check(ring.write(newOutput, 4, 8) == 4,
             "new pipeline frames must be queued") ||
      !check(ring.read(output, 6, 8) == 6,
             "transition frames must all be consumed") ||
      !check(output ==
                 std::vector<float>({0.0F, 0.0F, 3.0F, 4.0F, 5.0F, 6.0F,
                                     0.0F, 0.0F, 13.0F, 14.0F, 15.0F,
                                     16.0F}),
             "frames from the superseded pipeline must be silent")) {
    return false;
  }

  auto silencer = pipetune::AudioTransitionSilencer(7);
  if (!check(silencer.apply(output, 2, 6, 6, 8, 7, 0) == 6,
             "a pipeline change must start the requested silence") ||
      !check(output ==
                 std::vector<float>(12, 0.0F),
             "transition silence must cover every planar channel")) {
    return false;
  }

  auto next = std::vector<float>{7.0F, 8.0F, 17.0F, 18.0F};
  if (!check(silencer.apply(next, 2, 2, 2, 8, 7, 0) == 1,
             "transition silence must continue across output blocks") ||
      !check(next == std::vector<float>({0.0F, 8.0F, 0.0F, 18.0F}),
             "only the remaining transition prefix must be silent")) {
    return false;
  }
  auto final = std::vector<float>{9.0F, 19.0F};
  return check(silencer.apply(final, 2, 1, 1, 8, 7, 0) == 0,
               "completed transition silence must not repeat") &&
         check(final == std::vector<float>({9.0F, 19.0F}),
               "audio after the silence interval must pass unchanged");
}

static bool testStreamRestartProducesSilenceWithoutPipelineChange() {
  auto silencer = pipetune::AudioTransitionSilencer(11);
  auto beforeRestart = std::vector<float>{1.0F, 2.0F, 11.0F, 12.0F};
  if (!check(silencer.apply(beforeRestart, 2, 2, 2, 11, 4, 0) == 0,
             "stable pipeline audio must initially pass unchanged")) {
    return false;
  }

  silencer.start(3, 0);
  auto afterRestart =
      std::vector<float>{3.0F, 4.0F, 5.0F, 13.0F, 14.0F, 15.0F};
  return check(silencer.apply(afterRestart, 2, 3, 3, 11, 4, 0) == 3,
               "a stream restart must silence the requested interval") &&
         check(afterRestart == std::vector<float>(6, 0.0F),
               "a stream restart must be silent even without a DSP change");
}

static bool testStreamRestartIsSmoothedAroundSilence() {
  auto silencer = pipetune::AudioTransitionSilencer(11);
  auto beforeRestart = std::vector<float>{1.0F, 1.0F, -1.0F, -1.0F};
  if (!check(silencer.apply(beforeRestart, 2, 2, 2, 11, 3, 2) == 0,
             "stable audio must pass unchanged before a restart")) {
    return false;
  }

  silencer.start(3, 2);
  auto afterRestart =
      std::vector<float>{1.0F, 1.0F, 1.0F, 1.0F, 1.0F, 1.0F, 1.0F,
                         -1.0F, -1.0F, -1.0F, -1.0F, -1.0F, -1.0F, -1.0F};
  return check(silencer.apply(afterRestart, 2, 7, 7, 11, 3, 2) == 7,
               "every restart transition frame must be adjusted") &&
         check(afterRestart ==
                   std::vector<float>({0.5F, 0.0F, 0.0F, 0.0F, 0.0F,
                                       0.5F, 1.0F, -0.5F, 0.0F, 0.0F,
                                       0.0F, 0.0F, -0.5F, -1.0F}),
               "a restart must fade out, remain silent, and fade in");
}

static bool testUnderrunBoundariesAreSmoothed() {
  auto silencer = pipetune::AudioTransitionSilencer(4);
  auto active = std::vector<float>{1.0F, -1.0F};
  if (!check(silencer.apply(active, 2, 1, 1, 4, 0, 2) == 0,
             "available audio must initially pass unchanged")) {
    return false;
  }

  auto missing = std::vector<float>(6, 0.0F);
  if (!check(silencer.apply(missing, 2, 3, 0, 4, 0, 2) == 3,
             "an underrun boundary must be adjusted") ||
      !check(missing ==
                 std::vector<float>({0.5F, 0.0F, 0.0F,
                                     -0.5F, 0.0F, 0.0F}),
             "an underrun must fade the previous PCM into silence")) {
    return false;
  }

  auto resumed =
      std::vector<float>{1.0F, 1.0F, 1.0F, -1.0F, -1.0F, -1.0F};
  return check(silencer.apply(resumed, 2, 3, 3, 4, 0, 2) == 2,
               "resumed PCM must use the configured fade-in") &&
         check(resumed ==
                   std::vector<float>({0.5F, 1.0F, 1.0F,
                                       -0.5F, -1.0F, -1.0F}),
               "audio must fade in only when new PCM becomes available");
}

static bool testLateAudioAfterUnderrunIsDiscarded() {
  auto silencer = pipetune::AudioTransitionSilencer(6);
  auto active = std::vector<float>{1.0F};
  if (!check(silencer.apply(active, 1, 1, 1, 6, 4, 2) == 0,
             "available audio must initially pass unchanged")) {
    return false;
  }

  auto firstGap = std::vector<float>(6, 0.0F);
  if (!check(silencer.apply(firstGap, 1, 6, 0, 6, 4, 2) == 6,
             "a long input gap must remain silent") ||
      !check(firstGap ==
                 std::vector<float>({0.5F, 0.0F, 0.0F, 0.0F, 0.0F,
                                     0.0F}),
             "the input gap must contain only the fade-out")) {
    return false;
  }

  auto lateOldAudio = std::vector<float>{1.0F, 1.0F};
  if (!check(silencer.apply(lateOldAudio, 1, 2, 2, 6, 4, 2) == 2,
             "late PCM must be consumed by the silence guard") ||
      !check(lateOldAudio == std::vector<float>(2, 0.0F),
             "late PCM from the previous stream must not become audible")) {
    return false;
  }

  auto secondGap = std::vector<float>(4, 0.0F);
  if (!check(silencer.apply(secondGap, 1, 4, 0, 6, 4, 2) == 4,
             "a second gap must retain the remaining silence guard") ||
      !check(secondGap == std::vector<float>(4, 0.0F),
             "a second gap must not replay the previous sample")) {
    return false;
  }

  auto newAudio = std::vector<float>{-1.0F, -1.0F, -1.0F, -1.0F};
  return check(silencer.apply(newAudio, 1, 4, 4, 6, 4, 2) == 4,
               "stable replacement PCM must complete the silence guard") &&
         check(newAudio ==
                   std::vector<float>({0.0F, 0.0F, -0.5F, -1.0F}),
               "replacement PCM must fade in after the late tail is discarded");
}

static bool testDisconnectedOutputDoesNotReplayOldSample() {
  auto silencer = pipetune::AudioTransitionSilencer(5);
  auto active = std::vector<float>{1.0F, -1.0F};
  if (!check(silencer.apply(active, 2, 1, 1, 5, 3, 2) == 0,
             "connected output must initially pass unchanged")) {
    return false;
  }

  silencer.reset(3, 2);
  auto reconnected =
      std::vector<float>{1.0F, 1.0F, 1.0F, 1.0F, 1.0F,
                         -1.0F, -1.0F, -1.0F, -1.0F, -1.0F};
  return check(silencer.apply(reconnected, 2, 5, 5, 5, 3, 2) == 5,
               "reconnected output must remain guarded") &&
         check(reconnected ==
                   std::vector<float>({0.0F, 0.0F, 0.0F, 0.5F, 1.0F,
                                       0.0F, 0.0F, 0.0F, -0.5F, -1.0F}),
               "a disconnected output must resume from silence");
}

static bool testSixteenChannelAudioBridge() {
  constexpr auto channelCount = std::uint32_t{16};
  constexpr auto frameCount = std::uint32_t{2};
  auto input = std::vector<float>(channelCount * frameCount, 0.0F);
  for (auto channel = std::uint32_t{0}; channel < channelCount; ++channel) {
    input[channel * frameCount] = static_cast<float>(channel + 1u);
    input[channel * frameCount + 1u] =
        static_cast<float>((channel + 1u) * 10u);
  }

  try {
    auto ring = pipetune::PlanarAudioRing(channelCount, 4u);
    auto output = std::vector<float>(input.size(), 0.0F);
    if (!check(ring.write(input, frameCount, 0u) == frameCount,
               "a 16-channel block must fit in the audio ring") ||
        !check(ring.read(output, frameCount, 0u) == frameCount,
               "a 16-channel block must be readable") ||
        !check(output == input,
               "the audio ring must preserve all 16 planar channels")) {
      return false;
    }
  } catch (const std::exception &error) {
    return check(false, error.what());
  }

  auto transition = pipetune::AudioTransitionSilencer(3u);
  transition.start(1u, 0u);
  auto transitioned = std::vector<float>(channelCount * frameCount, 1.0F);
  if (!check(transition.apply(transitioned, channelCount, frameCount,
                              frameCount, 3u, 1u, 0u) == 1u,
             "a transition must cover every channel in its silence frame")) {
    return false;
  }
  for (auto channel = std::uint32_t{0}; channel < channelCount; ++channel) {
    if (!check(transitioned[channel * frameCount] == 0.0F &&
                   transitioned[channel * frameCount + 1u] == 1.0F,
               "transition silence must affect every planar channel")) {
      return false;
    }
  }
  return true;
}

int main() {
  const auto passed = testPlanarWraparound() && testUnderrunSilence() &&
                      testMeasuredBridgeDelayPreservesQueuedAudio() &&
                      testOverrunDropsNewestTail() &&
                      testMalformedBufferIsRejected() &&
                      testQueuedAudioCanBeDiscardedBeforeChangingOutput() &&
                      testPipelineTransitionProducesSilence() &&
                      testStreamRestartProducesSilenceWithoutPipelineChange() &&
                      testStreamRestartIsSmoothedAroundSilence() &&
                      testUnderrunBoundariesAreSmoothed() &&
                      testLateAudioAfterUnderrunIsDiscarded() &&
                      testDisconnectedOutputDoesNotReplayOldSample() &&
                      testSixteenChannelAudioBridge();
  return passed ? 0 : 1;
}
