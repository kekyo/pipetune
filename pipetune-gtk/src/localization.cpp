#include "localization.h"

#include <libintl.h>

#include <array>
#include <clocale>
#include <cstdlib>
#include <cstring>
#include <string>

namespace pipetune_gtk {

constexpr char kGettextDomain[] = "pipetune-gtk";

static std::string currentLocale(int category) {
  const auto *locale = std::setlocale(category, nullptr);
  return locale == nullptr ? std::string("C") : std::string(locale);
}

static void restoreLanguageEnvironment(
    const UiLocalizationEnvironment &environment) noexcept {
  if (environment.languageDefined) {
    ::setenv("LANGUAGE", environment.language.c_str(), 1);
  } else {
    ::unsetenv("LANGUAGE");
  }
}

static const char *selectUtf8Locale(int category, UiLanguage language) {
  const auto candidates =
      language == UiLanguage::japanese
          ? std::array<const char *, 4>{
                "ja_JP.UTF-8", "ja_JP.utf8", "C.UTF-8", "C.utf8"}
          : std::array<const char *, 4>{
                "en_US.UTF-8", "en_US.utf8", "C.UTF-8", "C.utf8"};
  std::setlocale(category, "C");
  for (const auto *candidate : candidates) {
    if (std::setlocale(category, candidate) != nullptr) {
      return candidate;
    }
  }
  return nullptr;
}

UiLocalizationEnvironment captureUiLocalizationEnvironment() {
  const auto *language = std::getenv("LANGUAGE");
  return {
      .characterTypeLocale = currentLocale(LC_CTYPE),
      .messagesLocale = currentLocale(LC_MESSAGES),
      .languageDefined = language != nullptr,
      .language = language == nullptr ? std::string{}
                                     : std::string(language),
  };
}

void restoreUiLocalizationEnvironment(
    const UiLocalizationEnvironment &environment) noexcept {
  restoreLanguageEnvironment(environment);
  std::setlocale(LC_CTYPE, environment.characterTypeLocale.c_str());
  std::setlocale(LC_MESSAGES, environment.messagesLocale.c_str());
}

UiLocalizationResult applyUiLanguage(
    const UiLocalizationEnvironment &originalEnvironment,
    UiLanguage language,
    const std::filesystem::path &localeDirectory) {
  restoreUiLocalizationEnvironment(originalEnvironment);
  if (::bindtextdomain(kGettextDomain, localeDirectory.c_str()) == nullptr) {
    return {.warning = "Cannot configure the GTK translation directory"};
  }
  if (::bind_textdomain_codeset(kGettextDomain, "UTF-8") == nullptr) {
    return {.warning = "Cannot configure the GTK translation encoding"};
  }
  if (::textdomain(kGettextDomain) == nullptr) {
    return {.warning = "Cannot select the GTK translation domain"};
  }

  if (language == UiLanguage::system) {
    if (std::setlocale(LC_CTYPE, "") == nullptr ||
        std::setlocale(LC_MESSAGES, "") == nullptr) {
      restoreUiLocalizationEnvironment(originalEnvironment);
      return {.warning = "Cannot apply the system message locale"};
    }
    return {.warning = {}};
  }

  const auto id = uiLanguageId(language);
  ::setenv("LANGUAGE", std::string(id).c_str(), 1);
  const auto *characterTypeLocale =
      selectUtf8Locale(LC_CTYPE, language);
  const auto *messagesLocale =
      selectUtf8Locale(LC_MESSAGES, language);
  if (characterTypeLocale == nullptr || messagesLocale == nullptr) {
    restoreUiLocalizationEnvironment(originalEnvironment);
    return {
        .warning =
            "Cannot select a UTF-8 locale for the requested GTK language",
    };
  }
  return {.warning = {}};
}

const char *translate(const char *message) noexcept {
  return ::dgettext(kGettextDomain, message);
}

const char *translateContext(const char *context,
                             const char *message) noexcept {
  auto qualified = std::string(context);
  qualified.push_back('\004');
  qualified += message;
  const auto *translated =
      ::dgettext(kGettextDomain, qualified.c_str());
  return std::strchr(translated, '\004') == nullptr ? translated : message;
}

const char *translatePlural(const char *singular, const char *plural,
                            unsigned long count) noexcept {
  return ::dngettext(kGettextDomain, singular, plural, count);
}

} // namespace pipetune_gtk
