#include "audio_bridge.h"

#include <cstdint>
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
  if (!check(silencer.apply(output, 2, 6, 8, 7) == 6,
             "a pipeline change must start the requested silence") ||
      !check(output ==
                 std::vector<float>(12, 0.0F),
             "transition silence must cover every planar channel")) {
    return false;
  }

  auto next = std::vector<float>{7.0F, 8.0F, 17.0F, 18.0F};
  if (!check(silencer.apply(next, 2, 2, 8, 7) == 1,
             "transition silence must continue across output blocks") ||
      !check(next == std::vector<float>({0.0F, 8.0F, 0.0F, 18.0F}),
             "only the remaining transition prefix must be silent")) {
    return false;
  }
  auto final = std::vector<float>{9.0F, 19.0F};
  return check(silencer.apply(final, 2, 1, 8, 7) == 0,
               "completed transition silence must not repeat") &&
         check(final == std::vector<float>({9.0F, 19.0F}),
               "audio after the silence interval must pass unchanged");
}

int main() {
  const auto passed = testPlanarWraparound() && testUnderrunSilence() &&
                      testOverrunDropsNewestTail() &&
                      testMalformedBufferIsRejected() &&
                      testQueuedAudioCanBeDiscardedBeforeChangingOutput() &&
                      testPipelineTransitionProducesSilence();
  return passed ? 0 : 1;
}
