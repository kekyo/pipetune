/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#ifndef PIPETUNE_GTK_UI_MESSAGE_H
#define PIPETUNE_GTK_UI_MESSAGE_H

#include <string>
#include <string_view>
#include <vector>

namespace pipetune_gtk {

/**
 * Retains a GUI message until the current presentation renders it.
 */
struct UiMessage {
  /** True when messageId is an English gettext message identifier. */
  bool translatable;
  /** English msgid or untranslated technical text. */
  std::string messageId;
  /** Values substituted for numbered placeholders during rendering. */
  std::vector<std::string> arguments;
};

/**
 * Creates a message translated only when it is rendered.
 *
 * Numbered placeholders such as {0} may be reordered by a translation.
 *
 * @param messageId English gettext message identifier.
 * @param arguments Placeholder values retained as semantic data.
 * @return Deferred localized message.
 */
UiMessage localizedMessage(std::string_view messageId,
                           std::vector<std::string> arguments);

/**
 * Creates untranslated technical or user-provided text.
 *
 * @param text Text that must remain unchanged across presentation languages.
 * @return Deferred raw message.
 */
UiMessage technicalMessage(std::string_view text);

/**
 * Reports whether a deferred message contains no visible text.
 *
 * @param message Message to inspect.
 * @return True when its message identifier is empty.
 */
bool uiMessageIsEmpty(const UiMessage &message) noexcept;

/**
 * Translates and formats a deferred message for the current presentation.
 *
 * @param message Semantic message to render.
 * @return Current localized text with numbered placeholders substituted.
 */
std::string formatUiMessage(const UiMessage &message);

} // namespace pipetune_gtk

#endif
