/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include "ui-message.h"

#include "localization.h"

#include <cctype>
#include <limits>
#include <utility>

namespace pipetune_gtk {

static bool parsePlaceholder(std::string_view text, std::size_t start,
                             std::size_t &end,
                             std::size_t &index) noexcept {
  if (start >= text.size() || text[start] != '{') {
    return false;
  }
  auto cursor = start + 1;
  if (cursor >= text.size() ||
      std::isdigit(static_cast<unsigned char>(text[cursor])) == 0) {
    return false;
  }
  auto value = std::size_t{0};
  while (cursor < text.size() &&
         std::isdigit(static_cast<unsigned char>(text[cursor])) != 0) {
    const auto digit =
        static_cast<std::size_t>(text[cursor] - '0');
    if (value >
        (std::numeric_limits<std::size_t>::max() - digit) / 10) {
      return false;
    }
    value = value * 10 + digit;
    ++cursor;
  }
  if (cursor >= text.size() || text[cursor] != '}') {
    return false;
  }
  end = cursor + 1;
  index = value;
  return true;
}

UiMessage localizedMessage(std::string_view messageId,
                           std::vector<std::string> arguments) {
  return {
      .translatable = true,
      .messageId = std::string(messageId),
      .arguments = std::move(arguments),
  };
}

UiMessage technicalMessage(std::string_view text) {
  return {
      .translatable = false,
      .messageId = std::string(text),
      .arguments = {},
  };
}

bool uiMessageIsEmpty(const UiMessage &message) noexcept {
  return message.messageId.empty();
}

std::string formatUiMessage(const UiMessage &message) {
  const auto *translated = message.translatable
                               ? translate(message.messageId.c_str())
                               : message.messageId.c_str();
  const auto source = std::string_view(translated);
  auto formatted = std::string{};
  formatted.reserve(source.size());
  for (auto cursor = std::size_t{0}; cursor < source.size();) {
    auto end = std::size_t{};
    auto index = std::size_t{};
    if (parsePlaceholder(source, cursor, end, index) &&
        index < message.arguments.size()) {
      formatted += message.arguments[index];
      cursor = end;
      continue;
    }
    formatted.push_back(source[cursor]);
    ++cursor;
  }
  return formatted;
}

} // namespace pipetune_gtk
