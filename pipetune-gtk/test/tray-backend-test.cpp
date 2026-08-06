#include "tray-backend.h"

#include <gio/gio.h>

#include <array>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

static bool check(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
  }
  return condition;
}

int main() {
  auto *dialogIcon = pipetune_gtk::loadPipeTuneIconPixbuf(
      48, pipetune_gtk::TrayIconColorMode::color);
  if (!check(dialogIcon != nullptr &&
                 gdk_pixbuf_get_width(dialogIcon) == 48 &&
                 gdk_pixbuf_get_height(dialogIcon) == 48,
             "dialog PipeTune icon differs")) {
    if (dialogIcon != nullptr) {
      g_object_unref(dialogIcon);
    }
    return 1;
  }
  g_object_unref(dialogIcon);

  const auto iconSizes = std::array<int, 8>{
      16, 22, 24, 32, 48, 64, 128, 256};
  const auto iconPixmaps = pipetune_gtk::loadTrayIconPixmaps(
      pipetune_gtk::TrayIconColorMode::color);
  const auto grayscalePixmaps = pipetune_gtk::loadTrayIconPixmaps(
      pipetune_gtk::TrayIconColorMode::grayscale);
  if (!check(iconPixmaps.size() == iconSizes.size(),
             "embedded tray icon size count differs") ||
      !check(grayscalePixmaps.size() == iconPixmaps.size(),
             "grayscale tray icon size count differs")) {
    return 1;
  }
  for (auto index = std::size_t{0}; index < iconSizes.size(); ++index) {
    const auto expectedSize = iconSizes[index];
    const auto &pixmap = iconPixmaps[index];
    const auto &grayscalePixmap = grayscalePixmaps[index];
    if (!check(pixmap.width == expectedSize &&
                   pixmap.height == expectedSize &&
                   pixmap.argbPixels.size() ==
                       static_cast<std::size_t>(expectedSize) *
                           static_cast<std::size_t>(expectedSize) * 4U,
               "embedded tray icon pixmap differs") ||
        !check(grayscalePixmap.width == pixmap.width &&
                   grayscalePixmap.height == pixmap.height &&
                   grayscalePixmap.argbPixels.size() ==
                       pixmap.argbPixels.size(),
               "grayscale tray icon pixmap differs")) {
      return 1;
    }
    for (auto offset = std::size_t{0};
         offset < grayscalePixmap.argbPixels.size(); offset += 4U) {
      if (!check(grayscalePixmap.argbPixels[offset] ==
                     pixmap.argbPixels[offset] &&
                     grayscalePixmap.argbPixels[offset + 1U] ==
                         grayscalePixmap.argbPixels[offset + 2U] &&
                     grayscalePixmap.argbPixels[offset + 2U] ==
                         grayscalePixmap.argbPixels[offset + 3U],
                 "grayscale tray icon pixel differs")) {
        return 1;
      }
    }
  }

  if (!check(
          std::string(pipetune_gtk::applicationId()) ==
              "net.kekyo.pipetune_gtk",
          "application id differs") ||
      !check(g_application_id_is_valid(pipetune_gtk::applicationId()),
             "application id must be valid") ||
      !check(pipetune_gtk::selectTrayBackendKind(
                 {.hasStatusNotifierItem = true, .hasXEmbed = true}) ==
                 pipetune_gtk::TrayBackendKind::statusNotifierItem,
             "StatusNotifierItem must take priority") ||
      !check(pipetune_gtk::selectTrayBackendKind(
                 {.hasStatusNotifierItem = false, .hasXEmbed = true}) ==
                 pipetune_gtk::TrayBackendKind::xembed,
             "GtkStatusIcon must be the compatibility fallback") ||
      !check(pipetune_gtk::selectTrayBackendKind(
                 {.hasStatusNotifierItem = false, .hasXEmbed = false}) ==
                 pipetune_gtk::TrayBackendKind::none,
             "unavailable transports must select no backend") ||
      !check(pipetune_gtk::trayIconName(
                 pipetune_gtk::TrayIconColorMode::color) ==
                 "pipetune",
             "color icon name differs") ||
      !check(pipetune_gtk::trayIconName(
                 pipetune_gtk::TrayIconColorMode::grayscale)
                 .empty(),
             "grayscale icon name must defer to its pixmap")) {
    return 1;
  }

  const auto timedActivation =
      pipetune_gtk::buildTrayActivationContext(1234U);
  const auto untimedActivation =
      pipetune_gtk::buildTrayActivationContext(0U);
  if (!check(timedActivation.userInteractionTime ==
                 std::optional<std::uint32_t>{1234U},
             "tray activation must preserve an event timestamp") ||
      !check(!untimedActivation.userInteractionTime.has_value(),
             "an unavailable tray timestamp must stay absent")) {
    return 1;
  }

  const auto rgba = std::array<std::uint8_t, 4>{
      0x11U, 0x22U, 0x33U, 0x44U};
  const auto argb = pipetune_gtk::convertTrayIconPixelsToArgb(
      rgba.data(), 1, 1, 4, 4);
  if (!check(argb == std::vector<std::uint8_t>{
                         0x44U, 0x11U, 0x22U, 0x33U},
             "RGBA to SNI ARGB conversion differs")) {
    return 1;
  }
  auto *variant = pipetune_gtk::buildTrayIconPixmapVariant(
      {{.width = 1, .height = 1, .argbPixels = argb}});
  const auto valid =
      g_variant_is_of_type(variant, G_VARIANT_TYPE("a(iiay)")) &&
      g_variant_n_children(variant) == 1;
  g_variant_unref(variant);
  return check(valid, "SNI pixmap variant differs") ? 0 : 1;
}
