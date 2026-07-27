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
  if (!check(ring.write(first, 4) == 4, "initial frames must be accepted")) {
    return false;
  }

  auto prefix = std::vector<float>(6, -1.0F);
  if (!check(ring.read(prefix, 3) == 3, "queued prefix must be readable") ||
      !check(prefix == std::vector<float>({1.0F, 2.0F, 3.0F, 11.0F, 12.0F, 13.0F}),
             "planar prefix differs")) {
    return false;
  }

  const auto second =
      std::vector<float>{5.0F, 6.0F, 7.0F, 8.0F, 15.0F, 16.0F, 17.0F, 18.0F};
  if (!check(ring.write(second, 4) == 4, "wrapped frames must be accepted")) {
    return false;
  }
  auto output = std::vector<float>(10, -1.0F);
  return check(ring.read(output, 5) == 5, "wrapped frames must all be readable") &&
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
  return check(ring.write(input, 2) == 2, "underrun input must be accepted") &&
         check(ring.read(output, 4) == 2, "read must report only queued frames") &&
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
  return check(ring.write(first, 3) == 3, "initial overrun input must fit") &&
         check(ring.write(second, 3) == 1, "only free capacity must be accepted") &&
         check(ring.overrunFrames() == 2, "discarded input frames must be counted") &&
         check(ring.read(output, 4) == 4, "full ring must remain readable") &&
         check(output ==
                   std::vector<float>({1.0F, 2.0F, 3.0F, 4.0F,
                                       11.0F, 12.0F, 13.0F, 14.0F}),
               "overrun must discard only the newest tail");
}

static bool testMalformedBufferIsRejected() {
  auto ring = pipetune::PlanarAudioRing(2, 4);
  const auto malformedInput = std::vector<float>(3, 1.0F);
  auto malformedOutput = std::vector<float>(3, 1.0F);
  return check(ring.write(malformedInput, 2) == 0,
               "incorrectly shaped input must be rejected") &&
         check(ring.read(malformedOutput, 2) == 0,
               "incorrectly shaped output must be rejected") &&
         check(malformedOutput == std::vector<float>({1.0F, 1.0F, 1.0F}),
               "rejected output must remain unchanged") &&
         check(ring.overrunFrames() == 0 && ring.underrunFrames() == 0,
               "rejected shapes must not change xrun counters");
}

int main() {
  const auto passed = testPlanarWraparound() && testUnderrunSilence() &&
                      testOverrunDropsNewestTail() && testMalformedBufferIsRejected();
  return passed ? 0 : 1;
}
