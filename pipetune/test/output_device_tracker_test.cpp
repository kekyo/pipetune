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
                                     std::string description,
                                     std::int32_t priority,
                                     bool virtualNode = false) {
  return {.id = id,
          .name = std::move(name),
          .description = std::move(description),
          .priority = priority,
          .virtualNode = virtualNode};
}

static bool testAutomaticDefaultAndFallback() {
  auto tracker = pipetune::OutputDeviceTracker("pipetune_sink", "");
  if (!check(tracker.updateDevice(
                 device(10, "speaker", "Built-in Speakers", 100)),
             "first physical sink must become selected") ||
      !check(tracker.selectedTarget() == "speaker",
             "first automatic target differs") ||
      !check(tracker.selectionReason() ==
                 pipetune::OutputSelectionReason::systemDefault,
             "automatic selection must report the system-default reason")) {
    return false;
  }
  tracker.commitSelection();
  if (!check(!tracker.updateDevice(
                 device(20, "headphones", "USB Headphones", 200)),
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
  return check(tracker.updateDevice(device(20, "low", "Low", 100)),
               "first enumerated sink must create an initial candidate") &&
         check(tracker.updateDevice(device(30, "high", "High", 300)),
               "a better initial candidate must replace a lower priority one") &&
         check(tracker.updateDevice(
                   device(10, "high-earlier", "High earlier", 300)),
               "lower global id must break an initial priority tie") &&
         check(tracker.selectedTarget() == "high-earlier",
               "initial enumeration must select the best physical sink");
}

static bool testVirtualAndSelfAreExcluded() {
  auto tracker = pipetune::OutputDeviceTracker("pipetune_sink", "");
  return check(!tracker.updateDevice(
                   device(1, "pipetune_sink", "PipeTune", 1000, true)),
               "PipeTune sink must not become a target") &&
         check(!tracker.updateDevice(
                   device(2, "virtual_effect", "Virtual Effect", 900, true)),
               "other virtual sinks must not become automatic targets") &&
         check(!tracker.setDefaultTarget("pipetune_sink"),
               "self default must not create a loop") &&
         check(tracker.selectedTarget().empty(),
               "virtual-only registry must have no physical target") &&
         check(tracker.selectionReason() ==
                   pipetune::OutputSelectionReason::unavailable,
               "virtual-only registry must report unavailable output") &&
         check(tracker.updateDevice(
                   device(3, "physical", "Physical Output", 10)),
               "physical sink must become usable") &&
         check(tracker.selectedTarget() == "physical",
               "physical sink selection differs");
}

static bool testPreferredTargetFallsBackAndReturns() {
  auto tracker = pipetune::OutputDeviceTracker("pipetune_sink", "usb");
  if (!check(tracker.hasPreferredTarget(),
             "preferred target must be reported") ||
      !check(tracker.preferredTarget() == "usb",
             "preferred target name differs") ||
      !check(tracker.updateDevice(
                 device(1, "speaker", "Built-in Speakers", 100)),
             "fallback sink must become selected") ||
      !check(!tracker.setDefaultTarget("speaker"),
             "setting the already selected fallback must not change it") ||
      !check(tracker.selectedTarget() == "speaker",
             "missing preference must use the system default") ||
      !check(tracker.selectionReason() ==
                 pipetune::OutputSelectionReason::fallback,
             "missing preference must report fallback")) {
    return false;
  }

  tracker.commitSelection();
  if (!check(tracker.updateDevice(device(2, "usb", "USB DAC", 1)),
             "available preference must replace the fallback") ||
      !check(tracker.selectedTarget() == "usb",
             "preferred sink must become selected") ||
      !check(tracker.selectionReason() ==
                 pipetune::OutputSelectionReason::preferred,
             "preferred sink must report preferred selection") ||
      !check(tracker.removeDevice(2),
             "preferred target removal must select the fallback") ||
      !check(tracker.selectedTarget() == "speaker",
             "preferred target removal must restore the system default") ||
      !check(tracker.selectionReason() ==
                 pipetune::OutputSelectionReason::fallback,
             "preferred target removal must report fallback") ||
      !check(tracker.updateDevice(device(3, "usb", "USB DAC", 1)),
             "returning preference must replace the fallback") ||
      !check(tracker.selectedTarget() == "usb",
             "returning preference must be restored automatically")) {
    return false;
  }

  return check(tracker.clearPreferredTarget(),
               "clearing a preference must change the selection") &&
         check(!tracker.hasPreferredTarget(),
               "cleared preference must no longer be reported") &&
         check(tracker.selectedTarget() == "speaker",
               "clearing a preference must select the system default") &&
         check(tracker.selectionReason() ==
                   pipetune::OutputSelectionReason::systemDefault,
               "clearing a preference must report system-default") &&
         check(!tracker.removeDevice(3),
               "removing an unpreferred sink must not change selection") &&
         check(!tracker.updateDevice(device(4, "usb", "USB DAC", 1)),
               "returning a cleared preference must not change selection") &&
         check(tracker.selectedTarget() == "speaker",
               "a cleared preference must not be restored");
}

static bool testDefaultChangesWhilePreferredTargetIsActive() {
  auto tracker = pipetune::OutputDeviceTracker("pipetune_sink", "usb");
  tracker.updateDevice(device(1, "speaker", "Built-in Speakers", 100));
  tracker.updateDevice(device(2, "headphones", "Headphones", 200));
  tracker.updateDevice(device(3, "usb", "USB DAC", 10));
  tracker.setDefaultTarget("speaker");
  tracker.commitSelection();

  return check(!tracker.setDefaultTarget("pipetune_sink"),
               "PipeTune becoming default must not replace physical fallback") &&
         check(tracker.systemDefaultTarget() == "speaker",
               "self default must preserve the physical default") &&
         check(!tracker.setDefaultTarget("headphones"),
               "default changes must not interrupt an available preference") &&
         check(tracker.systemDefaultTarget() == "headphones",
               "new physical default must be remembered") &&
         check(tracker.selectedTarget() == "usb",
               "preferred sink must remain selected") &&
         check(tracker.removeDevice(3),
               "removing preference must activate the new physical default") &&
         check(tracker.selectedTarget() == "headphones",
               "latest physical default must be used as fallback");
}

static bool testInitialPriorityTieBreak() {
  auto tracker = pipetune::OutputDeviceTracker("pipetune_sink", "");
  tracker.updateDevice(device(20, "later-id", "Later", 100));
  tracker.removeDevice(20);
  tracker.updateDevice(device(20, "later-id", "Later", 100));
  tracker.updateDevice(
      device(10, "higher-priority", "Higher priority", 200));
  tracker.removeDevice(20);
  if (!check(tracker.selectedTarget() == "higher-priority",
             "highest priority must win when selecting a fallback")) {
    return false;
  }

  tracker.removeDevice(10);
  tracker.updateDevice(device(20, "later-id", "Later", 100));
  tracker.updateDevice(device(10, "earlier-id", "Earlier", 100));
  tracker.removeDevice(20);
  return check(tracker.selectedTarget() == "earlier-id",
               "lower global id must break equal-priority fallback ties");
}

static bool testAvailableDevicesAreSortedForPresentation() {
  auto tracker = pipetune::OutputDeviceTracker("pipetune_sink", "usb");
  tracker.updateDevice(device(20, "z-name", "Speakers", 10));
  tracker.updateDevice(device(30, "usb", "Headphones", 20));
  tracker.updateDevice(device(10, "a-name", "Speakers", 30));
  tracker.updateDevice(
      device(40, "virtual", "Ignored virtual sink", 100, true));
  const auto available = tracker.availableDevices();

  return check(available.size() == 3,
               "only eligible physical devices must be listed") &&
         check(available[0].name == "usb" &&
                   available[1].name == "a-name" &&
                   available[2].name == "z-name",
               "devices must be sorted by description and node name");
}

static bool testSampleRateStateFollowsSelectedDevice() {
  auto tracker = pipetune::OutputDeviceTracker("pipetune_sink", "");
  tracker.updateDevice(device(10, "speaker", "Speakers", 100));
  tracker.updateDevice(device(20, "headphones", "Headphones", 200));
  tracker.setDefaultTarget("speaker");
  tracker.commitSelection();

  auto speakerCapabilities = pipetune::SampleRateCapabilities{
      .known = true,
      .constraints =
          {{.kind = pipetune::SampleRateConstraintKind::range,
            .minimum = 44100,
            .maximum = 192000,
            .step = 0}}};
  auto headphoneCapabilities = pipetune::SampleRateCapabilities{
      .known = true,
      .constraints =
          {{.kind = pipetune::SampleRateConstraintKind::discrete,
            .minimum = 48000,
            .maximum = 48000,
            .step = 0},
           {.kind = pipetune::SampleRateConstraintKind::discrete,
            .minimum = 96000,
            .maximum = 96000,
            .step = 0}}};
  if (!check(pipetune::normalizeSampleRateCapabilities(
                 speakerCapabilities),
             "speaker capabilities must normalize") ||
      !check(pipetune::normalizeSampleRateCapabilities(
                 headphoneCapabilities),
             "headphone capabilities must normalize") ||
      !check(tracker.updateSampleRateCapabilities(
                 10, speakerCapabilities),
             "first capability update must be observable") ||
      !check(tracker.updateSampleRateCapabilities(
                 20, headphoneCapabilities),
             "second capability update must be observable") ||
      !check(!tracker.updateSampleRateCapabilities(
                 20, headphoneCapabilities),
             "identical capabilities must not report a change") ||
      !check(tracker.updateActiveSampleRate(10, 192000),
             "active device rate must be observable") ||
      !check(tracker.selectedActiveSampleRate() == 192000,
             "selected active device rate differs") ||
      !check(pipetune::sampleRateCapabilitiesSupport(
                 tracker.selectedSampleRateCapabilities(), 192000),
             "selected capabilities must belong to the selected device")) {
    return false;
  }

  if (!check(tracker.setDefaultTarget("headphones"),
             "selected device must change") ||
      !check(tracker.selectedActiveSampleRate() == 0,
             "idle selected device must report no active rate") ||
      !check(!pipetune::sampleRateCapabilitiesSupport(
                 tracker.selectedSampleRateCapabilities(), 192000),
             "selected capabilities must change with the device") ||
      !check(tracker.updateActiveSampleRate(20, 96000),
             "selected active rate update must be observable") ||
      !check(tracker.selectedActiveSampleRate() == 96000,
             "updated active rate differs")) {
    return false;
  }

  const auto available = tracker.availableDevices();
  return check(available.size() == 2,
               "rate state must preserve both devices") &&
         check(available[0].name == "headphones" &&
                   available[0].sampleRateCapabilities.known &&
                   available[0].activeSampleRate == 96000,
               "presentation copy must include device rate state");
}

int main() {
  const auto passed = testAutomaticDefaultAndFallback() &&
                      testInitialEnumerationChoosesHighestPriority() &&
                      testVirtualAndSelfAreExcluded() &&
                      testPreferredTargetFallsBackAndReturns() &&
                      testDefaultChangesWhilePreferredTargetIsActive() &&
                      testInitialPriorityTieBreak() &&
                      testAvailableDevicesAreSortedForPresentation() &&
                      testSampleRateStateFollowsSelectedDevice();
  return passed ? 0 : 1;
}
