#include "output_device_tracker.h"

#include <iostream>
#include <string_view>

static bool check(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
  }
  return condition;
}

static pipetune::OutputDevice device(std::uint32_t id, std::string name,
                                     std::string serial, std::int32_t priority,
                                     bool virtualNode = false) {
  return {.id = id,
          .name = std::move(name),
          .objectSerial = std::move(serial),
          .priority = priority,
          .virtualNode = virtualNode};
}

static bool testAutomaticDefaultAndFallback() {
  auto tracker = pipetune::OutputDeviceTracker("pipetune_sink", "");
  if (!check(tracker.updateDevice(device(10, "speaker", "100", 100)),
             "first physical sink must become selected") ||
      !check(tracker.selectedTarget() == "speaker",
             "first automatic target differs")) {
    return false;
  }
  tracker.commitSelection();
  if (!check(!tracker.updateDevice(device(20, "headphones", "200", 200)),
             "adding a device must not interrupt a usable current target") ||
      !check(tracker.setDefaultTarget("headphones"),
             "physical default change must change target") ||
      !check(tracker.selectedTarget() == "headphones",
             "metadata default must be preferred")) {
    return false;
  }

  return check(tracker.removeDevice(20),
               "removing the selected target must choose a fallback") &&
         check(tracker.selectedTarget() == "speaker",
               "remaining physical sink must be the fallback") &&
         check(tracker.removeDevice(10), "removing the last sink must clear target") &&
         check(tracker.selectedTarget().empty(),
               "no target may remain after all physical sinks disappear");
}

static bool testInitialEnumerationChoosesHighestPriority() {
  auto tracker = pipetune::OutputDeviceTracker("pipetune_sink", "");
  return check(tracker.updateDevice(device(20, "low", "20", 100)),
               "first enumerated sink must create an initial candidate") &&
         check(tracker.updateDevice(device(30, "high", "30", 300)),
               "a better initial candidate must replace a lower priority one") &&
         check(tracker.updateDevice(device(10, "high-earlier", "10", 300)),
               "lower global id must break an initial priority tie") &&
         check(tracker.selectedTarget() == "high-earlier",
               "initial enumeration must select the best physical sink");
}

static bool testVirtualAndSelfAreExcluded() {
  auto tracker = pipetune::OutputDeviceTracker("pipetune_sink", "");
  return check(!tracker.updateDevice(device(1, "pipetune_sink", "1", 1000, true)),
               "PipeTune sink must not become a target") &&
         check(!tracker.updateDevice(device(2, "virtual_effect", "2", 900, true)),
               "other virtual sinks must not become automatic targets") &&
         check(!tracker.setDefaultTarget("pipetune_sink"),
               "self default must not create a loop") &&
         check(tracker.selectedTarget().empty(),
               "virtual-only registry must have no physical target") &&
         check(tracker.updateDevice(device(3, "physical", "3", 10)),
               "physical sink must become usable") &&
         check(tracker.selectedTarget() == "physical",
               "physical sink selection differs");
}

static bool testExplicitTargetWaitsAndReturns() {
  auto tracker = pipetune::OutputDeviceTracker("pipetune_sink", "42");
  if (!check(tracker.hasExplicitTarget(), "explicit target must be reported") ||
      !check(!tracker.updateDevice(device(1, "other", "10", 100)),
             "unrequested sink must not be selected") ||
      !check(tracker.selectedTarget().empty(),
             "explicit tracker must wait for requested sink") ||
      !check(tracker.updateDevice(device(2, "requested", "42", 1)),
             "matching object serial must select its node name") ||
      !check(tracker.selectedTarget() == "requested",
             "serial target must resolve to node.name")) {
    return false;
  }

  if (!check(tracker.removeDevice(2),
             "explicit target removal must clear selection") ||
      !check(tracker.selectedTarget().empty(),
             "explicit tracker must not fall back to another sink") ||
      !check(tracker.updateDevice(device(3, "restored", "42", 1)),
             "matching object serial must restore explicit target") ||
      !check(tracker.selectedTarget() == "restored",
             "restored serial target differs")) {
    return false;
  }

  auto namedTracker = pipetune::OutputDeviceTracker("pipetune_sink", "named");
  return check(namedTracker.updateDevice(device(4, "named", "99", 1)),
               "matching node name must select an explicit target") &&
         check(namedTracker.selectedTarget() == "named",
               "node-name target differs");
}

static bool testInitialPriorityTieBreak() {
  auto tracker = pipetune::OutputDeviceTracker("pipetune_sink", "");
  tracker.updateDevice(device(20, "later-id", "20", 100));
  tracker.removeDevice(20);
  tracker.updateDevice(device(20, "later-id", "20", 100));
  tracker.updateDevice(device(10, "higher-priority", "10", 200));
  tracker.removeDevice(20);
  if (!check(tracker.selectedTarget() == "higher-priority",
             "highest priority must win when selecting a fallback")) {
    return false;
  }

  tracker.removeDevice(10);
  tracker.updateDevice(device(20, "later-id", "20", 100));
  tracker.updateDevice(device(10, "earlier-id", "10", 100));
  tracker.removeDevice(20);
  return check(tracker.selectedTarget() == "earlier-id",
               "lower global id must break equal-priority fallback ties");
}

int main() {
  const auto passed = testAutomaticDefaultAndFallback() &&
                      testInitialEnumerationChoosesHighestPriority() &&
                      testVirtualAndSelfAreExcluded() &&
                      testExplicitTargetWaitsAndReturns() &&
                      testInitialPriorityTieBreak();
  return passed ? 0 : 1;
}
