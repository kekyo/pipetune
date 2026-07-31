#include "action-log.h"
#include "localization.h"
#include "ui-message.h"

#include <iostream>
#include <string>
#include <string_view>

namespace {

const auto check = [](bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
  }
  return condition;
};

const auto testDeferredTranslation =
    [](const std::filesystem::path &localeDirectory) {
      const auto original =
          pipetune_gtk::captureUiLocalizationEnvironment();
      auto log = pipetune_gtk::createActionLog(4);
      pipetune_gtk::appendAction(
          log, 1000, pipetune_gtk::ActionLogSeverity::error,
          pipetune_gtk::ActionLogCategory::persistence,
          pipetune_gtk::ActionLogState::failure,
          pipetune_gtk::localizedMessage(
              "Unavailable — {0}", {"gtk.conf"}),
          pipetune_gtk::technicalMessage(
              "/tmp/gtk.conf: permission denied"));

      pipetune_gtk::applyUiLanguage(
          original, pipetune_gtk::UiLanguage::english, localeDirectory);
      const auto englishSummary =
          pipetune_gtk::formatUiMessage(log.entries.front().summary);
      const auto englishDetail =
          pipetune_gtk::formatUiMessage(log.entries.front().detail);

      pipetune_gtk::applyUiLanguage(
          original, pipetune_gtk::UiLanguage::japanese, localeDirectory);
      const auto japaneseSummary =
          pipetune_gtk::formatUiMessage(log.entries.front().summary);
      const auto japaneseDetail =
          pipetune_gtk::formatUiMessage(log.entries.front().detail);
      pipetune_gtk::restoreUiLocalizationEnvironment(original);

      return check(englishSummary == "Unavailable — gtk.conf",
                   "English message formatting differs") &&
             check(japaneseSummary == "gtk.conf — 利用不可",
                   "the retained message must translate during rendering") &&
             check(englishDetail ==
                           "/tmp/gtk.conf: permission denied" &&
                       japaneseDetail == englishDetail,
                   "technical detail must remain untranslated") &&
             check(log.entries.front().summary.messageId ==
                       "Unavailable — {0}",
                   "the log must retain the semantic English msgid");
    };

const auto testPendingCompletion = [] {
  auto log = pipetune_gtk::createActionLog(2);
  const auto id = pipetune_gtk::appendPendingAction(
      log, 1000, pipetune_gtk::ActionLogCategory::settings,
      pipetune_gtk::localizedMessage("Saving all settings", {}),
      pipetune_gtk::technicalMessage("/tmp/settings"));
  const auto completed = pipetune_gtk::completePendingAction(
      log, id, 2000, false, pipetune_gtk::ActionLogSeverity::error,
      pipetune_gtk::localizedMessage(
          "Unavailable — {0}", {"settings"}),
      pipetune_gtk::technicalMessage("disk full"));
  return check(completed, "a retained pending action must complete") &&
         check(log.entries.front().summary.messageId ==
                   "Unavailable — {0}",
               "completion must replace the semantic summary") &&
         check(log.entries.front().detail.messageId == "disk full",
               "completion must replace the technical detail");
};

} // namespace

int main(int argc, char **argv) {
  if (argc != 2) {
    std::cerr << "locale directory argument is required\n";
    return 2;
  }
  return testDeferredTranslation(argv[1]) && testPendingCompletion()
             ? 0
             : 1;
}
