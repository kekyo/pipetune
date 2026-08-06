#include "localization.h"
#include "ui-language.h"

#include <glib.h>

#include <sys/stat.h>

#include <clocale>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

namespace {

struct LanguageExpectation {
  pipetune_gtk::UiLanguage language;
  std::string_view id;
  std::string_view systemDefaultTranslation;
};

constexpr auto translatedLanguages =
    std::array<LanguageExpectation, 9>{
        LanguageExpectation{pipetune_gtk::UiLanguage::arabic, "ar",
                            "النظام الافتراضي"},
        LanguageExpectation{pipetune_gtk::UiLanguage::spanish, "es",
                            "Valor predeterminado del sistema"},
        LanguageExpectation{pipetune_gtk::UiLanguage::french, "fr",
                            "Valeur par défaut du système"},
        LanguageExpectation{pipetune_gtk::UiLanguage::hindi, "hi",
                            "सिस्टम डिफ़ॉल्ट"},
        LanguageExpectation{pipetune_gtk::UiLanguage::japanese, "ja",
                            "システム設定"},
        LanguageExpectation{pipetune_gtk::UiLanguage::korean, "ko",
                            "시스템 기본값"},
        LanguageExpectation{pipetune_gtk::UiLanguage::portuguese, "pt",
                            "Padrão do sistema"},
        LanguageExpectation{pipetune_gtk::UiLanguage::russian, "ru",
                            "Система по умолчанию"},
        LanguageExpectation{pipetune_gtk::UiLanguage::chinese, "zh",
                            "系统默认"},
    };

const auto check = [](bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
  }
  return condition;
};

const auto readFile = [](const std::filesystem::path &path) {
  auto stream = std::ifstream(path);
  return std::string(std::istreambuf_iterator<char>(stream),
                     std::istreambuf_iterator<char>());
};

const auto writeFile = [](const std::filesystem::path &path,
                          std::string_view contents) {
  auto stream = std::ofstream(path);
  stream << contents;
  return stream.good();
};

const auto testPreferencePath = [] {
  return check(
             pipetune_gtk::resolveUiLanguageConfigPath(
                 "/tmp/xdg-config", "/tmp/home") ==
                 std::filesystem::path("/tmp/xdg-config/pipetune/gtk.conf"),
             "XDG_CONFIG_HOME must take precedence") &&
         check(
             pipetune_gtk::resolveUiLanguageConfigPath({}, "/tmp/home") ==
                 std::filesystem::path(
                     "/tmp/home/.config/pipetune/gtk.conf"),
             "HOME must provide the fallback configuration path");
};

const auto testLanguageIdentifiers = [] {
  for (const auto &expectation : translatedLanguages) {
    auto parsed = pipetune_gtk::UiLanguage::system;
    if (!check(pipetune_gtk::uiLanguageId(expectation.language) ==
                   expectation.id,
               "a translated language identifier differs") ||
        !check(pipetune_gtk::parseUiLanguageId(expectation.id, parsed) &&
                   parsed == expectation.language,
               "a translated language identifier must round-trip")) {
      return false;
    }
  }
  auto unsupported = pipetune_gtk::UiLanguage::system;
  return check(!pipetune_gtk::parseUiLanguageId("de", unsupported),
               "an unsupported language identifier must be rejected");
};

const auto testPreferencePersistence =
    [](const std::filesystem::path &temporaryDirectory) {
      const auto path =
          temporaryDirectory / "configuration" / "pipetune" / "gtk.conf";
      const auto missing = pipetune_gtk::loadUiLanguagePreference(path);
      if (!check(missing.language == pipetune_gtk::UiLanguage::system,
                 "a missing preference must use the system language") ||
          !check(missing.warning.empty(),
                 "a missing preference must not report a warning")) {
        return false;
      }

      const auto firstSave = pipetune_gtk::saveUiLanguagePreference(
          path, pipetune_gtk::UiLanguage::japanese);
      if (!check(firstSave.error.empty(),
                 "the Japanese preference must be saved") ||
          !check(pipetune_gtk::loadUiLanguagePreference(path).language ==
                     pipetune_gtk::UiLanguage::japanese,
                 "the Japanese preference must round-trip")) {
        return false;
      }

      struct stat directoryStatus {};
      struct stat fileStatus {};
      if (!check(::stat(path.parent_path().c_str(), &directoryStatus) == 0,
                 "the preference directory must exist") ||
          !check((directoryStatus.st_mode & 0777) == 0700,
                 "the preference directory must be private") ||
          !check(::stat(path.c_str(), &fileStatus) == 0,
                 "the preference file must exist") ||
          !check((fileStatus.st_mode & 0777) == 0600,
                 "the preference file must be private")) {
        return false;
      }

      if (!writeFile(path,
                     "# retained comment\n"
                     "[ui]\n"
                     "language=en\n"
                     "future-option=retained\n"
                     "\n"
                     "[future]\n"
                     "value=retained\n")) {
        return check(false, "the preservation fixture must be writable");
      }
      const auto secondSave = pipetune_gtk::saveUiLanguagePreference(
          path, pipetune_gtk::UiLanguage::system);
      const auto persisted = readFile(path);
      return check(secondSave.error.empty(),
                   "the system preference must be saved") &&
             check(persisted.find("# retained comment") !=
                       std::string::npos,
                   "well-formed comments must be retained") &&
             check(persisted.find("future-option=retained") !=
                       std::string::npos,
                   "unknown keys must be retained") &&
             check(persisted.find("[future]") != std::string::npos,
                   "unknown groups must be retained") &&
             check(persisted.find("language=system") !=
                       std::string::npos,
                   "the selected language must be updated");
    };

const auto testPreferenceDiagnostics =
    [](const std::filesystem::path &temporaryDirectory) {
      const auto malformed = temporaryDirectory / "malformed.conf";
      const auto unknown = temporaryDirectory / "unknown.conf";
      if (!writeFile(malformed, "[ui\nlanguage=ja\n") ||
          !writeFile(unknown, "[ui]\nlanguage=de\n")) {
        return check(false, "diagnostic fixtures must be writable");
      }
      const auto malformedResult =
          pipetune_gtk::loadUiLanguagePreference(malformed);
      const auto unknownResult =
          pipetune_gtk::loadUiLanguagePreference(unknown);
      const auto blockedParent = temporaryDirectory / "not-a-directory";
      if (!writeFile(blockedParent, "blocked")) {
        return check(false, "the failed-save fixture must be writable");
      }
      const auto failedSave = pipetune_gtk::saveUiLanguagePreference(
          blockedParent / "gtk.conf",
          pipetune_gtk::UiLanguage::english);
      return check(
                 malformedResult.language ==
                     pipetune_gtk::UiLanguage::system &&
                     !malformedResult.warning.empty(),
                 "malformed preferences must fall back with a warning") &&
             check(unknownResult.language ==
                           pipetune_gtk::UiLanguage::system &&
                       !unknownResult.warning.empty(),
                   "unknown preferences must fall back with a warning") &&
             check(!failedSave.error.empty(),
                   "an unwritable preference must report an error");
    };

const auto testLocalization = [](const std::filesystem::path &localeDirectory) {
  const auto original =
      pipetune_gtk::captureUiLocalizationEnvironment();
  for (const auto &expectation : translatedLanguages) {
    const auto localization = pipetune_gtk::applyUiLanguage(
        original, expectation.language, localeDirectory);
    if (!check(localization.warning.empty(),
               "a translated locale must be selectable") ||
        !check(
            std::string_view(pipetune_gtk::translate("System default")) ==
                expectation.systemDefaultTranslation,
            "a translated catalog must provide its GUI messages")) {
      pipetune_gtk::restoreUiLocalizationEnvironment(original);
      return false;
    }
  }

  const auto english = pipetune_gtk::applyUiLanguage(
      original, pipetune_gtk::UiLanguage::english, localeDirectory);
  const auto englishSelected =
      check(english.warning.empty(), "the English locale must be selectable") &&
      check(std::string_view(pipetune_gtk::translate("System default")) ==
                "System default",
            "the English preference must use English msgids");

  ::setenv("LC_ALL", "C", 1);
  ::unsetenv("LANGUAGE");
  ::setlocale(LC_ALL, "C");
  const auto cEnvironment =
      pipetune_gtk::captureUiLocalizationEnvironment();
  const auto japaneseFromC = pipetune_gtk::applyUiLanguage(
      cEnvironment, pipetune_gtk::UiLanguage::japanese, localeDirectory);
  const auto cLocaleSelected =
      check(japaneseFromC.warning.empty(),
            "an explicit language must work from the C locale") &&
      check(std::string_view(pipetune_gtk::translate("System default")) ==
                "システム設定",
            "the C locale must not suppress an explicit Japanese choice");

  pipetune_gtk::restoreUiLocalizationEnvironment(original);
  return englishSelected && cLocaleSelected;
};

} // namespace

int main(int argc, char **argv) {
  if (argc != 2) {
    std::cerr << "locale directory argument is required\n";
    return 2;
  }
  auto *temporary = g_dir_make_tmp("pipetune-gtk-language-test-XXXXXX",
                                   nullptr);
  if (temporary == nullptr) {
    std::cerr << "temporary directory creation failed\n";
    return 2;
  }
  const auto temporaryDirectory = std::filesystem::path(temporary);
  g_free(temporary);
  const auto passed =
      testPreferencePath() &&
      testLanguageIdentifiers() &&
      testPreferencePersistence(temporaryDirectory) &&
      testPreferenceDiagnostics(temporaryDirectory) &&
      testLocalization(argv[1]);
  auto cleanupError = std::error_code{};
  std::filesystem::remove_all(temporaryDirectory, cleanupError);
  if (cleanupError) {
    std::cerr << "temporary directory cleanup failed: "
              << cleanupError.message() << '\n';
    return 2;
  }
  return passed ? 0 : 1;
}
