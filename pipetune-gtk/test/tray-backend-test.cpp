#include "tray-backend.h"

#include <gio/gio.h>

#include <array>
#include <cstdint>
#include <iostream>
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
  if (!check(
          std::string(pipetune_gtk::applicationId()) ==
              "net.kekyo.pipetune-gtk",
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
                 pipetune_gtk::TrayIconState::active) == "pipetune",
             "active icon name differs") ||
      !check(pipetune_gtk::trayIconName(
                 pipetune_gtk::TrayIconState::attention) ==
                 "pipetune-attention",
             "attention icon name differs") ||
      !check(pipetune_gtk::trayIconName(
                 pipetune_gtk::TrayIconState::disconnected) ==
                 "pipetune-disconnected",
             "disconnected icon name differs")) {
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
