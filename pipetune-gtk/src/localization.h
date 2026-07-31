#ifndef PIPETUNE_GTK_LOCALIZATION_H
#define PIPETUNE_GTK_LOCALIZATION_H

#include "ui-language.h"

#include <filesystem>
#include <string>

namespace pipetune_gtk {

/**
 * Captures locale state that PipeTune GTK may temporarily replace.
 */
struct UiLocalizationEnvironment {
  /** Locale name active for character classification. */
  std::string characterTypeLocale;
  /** Locale name active for translated messages. */
  std::string messagesLocale;
  /** Whether LANGUAGE existed when this state was captured. */
  bool languageDefined;
  /** Original LANGUAGE value, empty when it was undefined. */
  std::string language;
};

/**
 * Contains the result of applying a presentation language.
 */
struct UiLocalizationResult {
  /** English technical diagnostic, empty when selection succeeded. */
  std::string warning;
};

/**
 * Captures the current locale categories and LANGUAGE environment variable.
 *
 * @return Restorable localization environment.
 */
UiLocalizationEnvironment captureUiLocalizationEnvironment();

/**
 * Restores locale categories and LANGUAGE from a prior capture.
 *
 * @param environment Environment to restore.
 */
void restoreUiLocalizationEnvironment(
    const UiLocalizationEnvironment &environment) noexcept;

/**
 * Applies a UI language without changing numeric, time, or collation locales.
 *
 * Each call starts from the supplied original environment so a future
 * presentation rebuild can switch language repeatedly without relying on
 * gettext internals.
 *
 * @param originalEnvironment Process environment captured before localization.
 * @param language System, English, or Japanese presentation language.
 * @param localeDirectory Root containing locale catalogs.
 * @return Empty warning on success, otherwise an English diagnostic.
 */
UiLocalizationResult applyUiLanguage(
    const UiLocalizationEnvironment &originalEnvironment,
    UiLanguage language,
    const std::filesystem::path &localeDirectory);

/**
 * Returns the gettext domain used by PipeTune GTK presentation strings.
 *
 * @return Process-lifetime translation domain string.
 */
const char *translationDomain() noexcept;

/**
 * Translates one GUI message in the PipeTune GTK gettext domain.
 *
 * @param message English message identifier.
 * @return Catalog translation or message when no translation is available.
 */
const char *translate(const char *message) noexcept;

/**
 * Translates one context-qualified GUI message.
 *
 * @param context Stable translator context.
 * @param message English message identifier.
 * @return Catalog translation or message when no translation is available.
 */
const char *translateContext(const char *context,
                             const char *message) noexcept;

/**
 * Translates a plural GUI message.
 *
 * @param singular English singular message identifier.
 * @param plural English plural message identifier.
 * @param count Quantity used to select the plural form.
 * @return Selected catalog translation or English identifier.
 */
const char *translatePlural(const char *singular, const char *plural,
                            unsigned long count) noexcept;

} // namespace pipetune_gtk

#define PIPETUNE_GTK_TRANSLATE(message) \
  ::pipetune_gtk::translate(message)
#define PIPETUNE_GTK_TRANSLATE_CONTEXT(context, message) \
  ::pipetune_gtk::translateContext(context, message)
#define PIPETUNE_GTK_TRANSLATE_PLURAL(singular, plural, count) \
  ::pipetune_gtk::translatePlural(singular, plural, count)

#endif
