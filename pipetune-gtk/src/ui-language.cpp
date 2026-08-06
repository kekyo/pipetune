#include "ui-language.h"

#include <glib.h>
#include <glib/gstdio.h>

#include <cerrno>
#include <cstring>
#include <system_error>

namespace pipetune_gtk {

constexpr char kUiGroup[] = "ui";
constexpr char kLanguageKey[] = "language";

static std::string gErrorMessage(std::string_view prefix,
                                 const GError *error) {
  auto message = std::string(prefix);
  if (error != nullptr && error->message != nullptr) {
    message += ": ";
    message += error->message;
  }
  return message;
}

static bool isAbsolutePath(std::string_view path) {
  return !path.empty() && std::filesystem::path(path).is_absolute();
}

std::filesystem::path resolveUiLanguageConfigPath(
    std::string_view xdgConfigHome, std::string_view home) {
  if (isAbsolutePath(xdgConfigHome)) {
    return std::filesystem::path(xdgConfigHome) / "pipetune" /
           "gtk.conf";
  }
  if (!home.empty()) {
    return std::filesystem::path(home) / ".config" / "pipetune" /
           "gtk.conf";
  }
  return std::filesystem::path(g_get_user_config_dir()) / "pipetune" /
         "gtk.conf";
}

std::string_view uiLanguageId(UiLanguage language) noexcept {
  switch (language) {
  case UiLanguage::system:
    return "system";
  case UiLanguage::english:
    return "en";
  case UiLanguage::arabic:
    return "ar";
  case UiLanguage::spanish:
    return "es";
  case UiLanguage::french:
    return "fr";
  case UiLanguage::hindi:
    return "hi";
  case UiLanguage::japanese:
    return "ja";
  case UiLanguage::korean:
    return "ko";
  case UiLanguage::portuguese:
    return "pt";
  case UiLanguage::russian:
    return "ru";
  case UiLanguage::chinese:
    return "zh";
  }
  return "system";
}

bool parseUiLanguageId(std::string_view id,
                       UiLanguage &language) noexcept {
  if (id == "system") {
    language = UiLanguage::system;
    return true;
  }
  if (id == "en") {
    language = UiLanguage::english;
    return true;
  }
  if (id == "ar") {
    language = UiLanguage::arabic;
    return true;
  }
  if (id == "es") {
    language = UiLanguage::spanish;
    return true;
  }
  if (id == "fr") {
    language = UiLanguage::french;
    return true;
  }
  if (id == "hi") {
    language = UiLanguage::hindi;
    return true;
  }
  if (id == "ja") {
    language = UiLanguage::japanese;
    return true;
  }
  if (id == "ko") {
    language = UiLanguage::korean;
    return true;
  }
  if (id == "pt") {
    language = UiLanguage::portuguese;
    return true;
  }
  if (id == "ru") {
    language = UiLanguage::russian;
    return true;
  }
  if (id == "zh") {
    language = UiLanguage::chinese;
    return true;
  }
  return false;
}

UiLanguageLoadResult
loadUiLanguagePreference(const std::filesystem::path &path) {
  auto *keyFile = g_key_file_new();
  auto *error = static_cast<GError *>(nullptr);
  const auto loaded = g_key_file_load_from_file(
      keyFile, path.c_str(),
      static_cast<GKeyFileFlags>(G_KEY_FILE_KEEP_COMMENTS |
                                 G_KEY_FILE_KEEP_TRANSLATIONS),
      &error);
  if (!loaded) {
    const auto missing =
        error != nullptr && error->domain == G_FILE_ERROR &&
        error->code == G_FILE_ERROR_NOENT;
    const auto warning =
        missing ? std::string{}
                : gErrorMessage("Cannot load GTK language preference",
                                error);
    g_clear_error(&error);
    g_key_file_unref(keyFile);
    return {.language = UiLanguage::system, .warning = warning};
  }

  auto *value =
      g_key_file_get_string(keyFile, kUiGroup, kLanguageKey, &error);
  if (value == nullptr) {
    const auto warning =
        gErrorMessage("Cannot read GTK language preference", error);
    g_clear_error(&error);
    g_key_file_unref(keyFile);
    return {.language = UiLanguage::system, .warning = warning};
  }

  auto language = UiLanguage::system;
  const auto recognized = parseUiLanguageId(value, language);
  const auto unknownValue = std::string(value);
  g_free(value);
  g_key_file_unref(keyFile);
  if (!recognized) {
    return {
        .language = UiLanguage::system,
        .warning =
            "Unsupported GTK language preference: " + unknownValue,
    };
  }
  return {.language = language, .warning = {}};
}

UiLanguageSaveResult
saveUiLanguagePreference(const std::filesystem::path &path,
                         UiLanguage language) {
  const auto parent = path.parent_path();
  if (parent.empty()) {
    return {.error = "GTK language preference has no parent directory"};
  }
  if (g_mkdir_with_parents(parent.c_str(), 0700) != 0) {
    return {
        .error = "Cannot create GTK preference directory: " +
                 std::string(std::strerror(errno)),
    };
  }
  if (g_chmod(parent.c_str(), 0700) != 0) {
    return {
        .error = "Cannot protect GTK preference directory: " +
                 std::string(std::strerror(errno)),
    };
  }

  auto *keyFile = g_key_file_new();
  auto *loadError = static_cast<GError *>(nullptr);
  if (!g_key_file_load_from_file(
          keyFile, path.c_str(),
          static_cast<GKeyFileFlags>(G_KEY_FILE_KEEP_COMMENTS |
                                     G_KEY_FILE_KEEP_TRANSLATIONS),
          &loadError)) {
    g_clear_error(&loadError);
  }
  const auto id = uiLanguageId(language);
  g_key_file_set_string(keyFile, kUiGroup, kLanguageKey,
                        std::string(id).c_str());

  auto length = gsize{};
  auto *serializationError = static_cast<GError *>(nullptr);
  auto *contents =
      g_key_file_to_data(keyFile, &length, &serializationError);
  g_key_file_unref(keyFile);
  if (contents == nullptr) {
    const auto message = gErrorMessage(
        "Cannot serialize GTK language preference", serializationError);
    g_clear_error(&serializationError);
    return {.error = message};
  }

  auto *writeError = static_cast<GError *>(nullptr);
  const auto flags = static_cast<GFileSetContentsFlags>(
      G_FILE_SET_CONTENTS_CONSISTENT | G_FILE_SET_CONTENTS_DURABLE);
  const auto saved = g_file_set_contents_full(
      path.c_str(), contents, static_cast<gssize>(length), flags, 0600,
      &writeError);
  g_free(contents);
  if (!saved) {
    const auto message =
        gErrorMessage("Cannot save GTK language preference", writeError);
    g_clear_error(&writeError);
    return {.error = message};
  }
  if (g_chmod(path.c_str(), 0600) != 0) {
    return {
        .error = "Cannot protect GTK language preference: " +
                 std::string(std::strerror(errno)),
    };
  }
  return {.error = {}};
}

} // namespace pipetune_gtk
