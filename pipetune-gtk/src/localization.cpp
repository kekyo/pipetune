/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include "localization.h"

#include <libintl.h>

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

static const char *localePrefix(UiLanguage language) noexcept {
  switch (language) {
  case UiLanguage::system:
    return nullptr;
  case UiLanguage::english:
    return "en_US";
  case UiLanguage::arabic:
    return "ar_SA";
  case UiLanguage::spanish:
    return "es_ES";
  case UiLanguage::french:
    return "fr_FR";
  case UiLanguage::hindi:
    return "hi_IN";
  case UiLanguage::japanese:
    return "ja_JP";
  case UiLanguage::korean:
    return "ko_KR";
  case UiLanguage::portuguese:
    return "pt_PT";
  case UiLanguage::russian:
    return "ru_RU";
  case UiLanguage::chinese:
    return "zh_CN";
  }
  return nullptr;
}

static bool selectLocaleWithPrefix(int category, const char *prefix) {
  for (const auto &suffix : {std::string(".UTF-8"),
                             std::string(".utf8")}) {
    const auto candidate = std::string(prefix) + suffix;
    if (std::setlocale(category, candidate.c_str()) != nullptr) {
      return true;
    }
  }
  return false;
}

static bool selectUtf8Locale(int category, UiLanguage language) {
  std::setlocale(category, "C");
  const auto *prefix = localePrefix(language);
  if (prefix != nullptr && selectLocaleWithPrefix(category, prefix)) {
    return true;
  }
  for (const auto *fallback : {"en_US", "ja_JP"}) {
    if (selectLocaleWithPrefix(category, fallback)) {
      return true;
    }
  }
  if (category == LC_MESSAGES && language != UiLanguage::english) {
    return false;
  }
  return std::setlocale(category, "C.UTF-8") != nullptr ||
         std::setlocale(category, "C.utf8") != nullptr;
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
  const auto characterTypeLocale =
      selectUtf8Locale(LC_CTYPE, language);
  const auto messagesLocale = selectUtf8Locale(LC_MESSAGES, language);
  if (!characterTypeLocale || !messagesLocale) {
    restoreUiLocalizationEnvironment(originalEnvironment);
    return {
        .warning =
            "Cannot select a UTF-8 locale for the requested GTK language",
    };
  }
  return {.warning = {}};
}

const char *translationDomain() noexcept {
  return kGettextDomain;
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
