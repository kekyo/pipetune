/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include "output-level-meter.h"
#include "main-window.h"

#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <string_view>

static bool check(bool condition, std::string_view message) {
  if (!condition) std::cerr << message << '\n';
  return condition;
}

int main(int argc, char **argv) {
  gtk_init(&argc, &argv);
  const auto left = pipetune_gtk::createOutputLevelMeter();
  const auto right = pipetune_gtk::createOutputLevelMeter();
  const auto update = [&](std::optional<float> peak) {
    pipetune_gtk::updateOutputLevelMeter(left, "Ch 3", "Bass Left", peak);
  };
  const auto valueText = [&] {
    return std::string_view(gtk_label_get_text(GTK_LABEL(left.valueLabel)));
  };
  update(0.5F);
  pipetune_gtk::updateOutputLevelMeter(right, "Ch 4", "", 0.0F);
  auto valid = check(valueText() == "-6.0", "half-scale peak must be -6.0 dBFS") &&
      check(std::abs(gtk_level_bar_get_value(GTK_LEVEL_BAR(left.levelBar)) - 53.9794) < 0.001,
          "peak bar must use logarithmic amplitude") &&
      check(gtk_level_bar_get_value(GTK_LEVEL_BAR(right.levelBar)) == 0,
          "a silent channel must remain empty independently") &&
      check(std::string_view(gtk_label_get_text(GTK_LABEL(left.channelLabel))) == "Ch 3",
          "logical channel identity must be visible") &&
      check(std::string_view(atk_object_get_name(gtk_widget_get_accessible(left.levelBar))) ==
          "Ch 3 · Bass Left: -6.0 dBFS", "accessible peak must include channel and purpose");
  update(0.0F);
  valid = check(valueText() == "−∞" && gtk_level_bar_get_value(GTK_LEVEL_BAR(left.levelBar)) == 0,
      "silence must clear the previous peak") && valid;
  update(2.0F);
  valid = check(valueText() == "+6.0" && gtk_level_bar_get_value(GTK_LEVEL_BAR(left.levelBar)) == 60 &&
      gtk_style_context_has_class(gtk_widget_get_style_context(left.levelBar), "output-level-overload"),
      "over-full-scale peaks must remain visible with a bounded red bar") && valid;
  for (const auto peak : {std::optional<float>{}, std::optional<float>{-1.0F},
      std::optional<float>{std::numeric_limits<float>::quiet_NaN()}}) {
    update(peak);
    valid = check(valueText() == "—" && gtk_level_bar_get_value(GTK_LEVEL_BAR(left.levelBar)) == 0,
        "unavailable or invalid peaks must clear rather than display stale audio") && valid;
  }
  update(0.0001F);
  valid = check(valueText() == "-80.0" && gtk_level_bar_get_value(GTK_LEVEL_BAR(left.levelBar)) == 0,
      "levels below the drawing floor must keep their numeric value") && valid;
  gtk_widget_destroy(left.root);
  gtk_widget_destroy(right.root);

  auto ui = pipetune_gtk::MainWindowUi{
      .outputLevelMeters = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8),
      .mixedOutputSection = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8),
      .mixedOutputMeters = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8)};
  gtk_container_add(GTK_CONTAINER(ui.mixedOutputSection), ui.mixedOutputMeters);
  auto configuration = pipetune::OutputConfiguration{};
  configuration.channels.resize(4);
  configuration.channels[2].label = "Bass Left";
  pipetune_gtk::renderOutputLevels(ui, configuration, std::array{0.5F, 0.0F}, true);
  valid = check(ui.outputLevelRows.size() == 2,
      "single mode must display actual stereo width rather than inactive assignments") && valid;
  configuration.mode = pipetune::OutputMode::multiple;
  const auto peaks = std::array{0.0F, 0.0F, 0.5F, 0.25F};
  pipetune_gtk::renderOutputLevels(ui, configuration, peaks, true);
  valid = check(ui.outputLevelRows.size() == 4 && std::string_view(atk_object_get_name(
      gtk_widget_get_accessible(ui.outputLevelRows[2].levelBar))) == "Ch 3 · Bass Left: -6.0 dBFS",
      "multiple mode must display actual output order and confirmed purpose labels") && valid;
  pipetune_gtk::renderOutputLevels(ui, configuration, peaks, false);
  for (const auto &row : ui.outputLevelRows)
    valid = check(gtk_level_bar_get_value(GTK_LEVEL_BAR(row.levelBar)) == 0 &&
        std::string_view(gtk_label_get_text(GTK_LABEL(row.valueLabel))) == "—",
        "a lost connection must clear every channel while keeping its identity") && valid;
  configuration.mode = pipetune::OutputMode::single;
  pipetune_gtk::renderOutputLevels(ui, configuration, {}, true);
  valid = check(ui.outputLevelRows.size() == 4 &&
      std::string_view(gtk_label_get_text(GTK_LABEL(ui.outputLevelRows[0].valueLabel))) == "—",
      "missing measurements must retain the last preset width rather than infer stereo from the device mode") && valid;
  pipetune_gtk::renderOutputLevels(ui, configuration, peaks, true);
  valid = check(ui.outputLevelRows.size() == 4 &&
      gtk_level_bar_get_value(GTK_LEVEL_BAR(ui.outputLevelRows[2].levelBar)) > 0,
      "single mode must display generated channels that have no device assignment") && valid;
  configuration.mode = pipetune::OutputMode::multiple;
  configuration.channels.resize(16);
  pipetune_gtk::renderOutputLevels(ui, configuration, std::array{0.5F, 0.25F}, true);
  valid = check(ui.outputLevelRows.size() == 2,
      "sixteen device slots must not expand a stereo preset meter") && valid;
  pipetune_gtk::renderMixedOutputLevels(ui, configuration, std::array{0.0F, 0.5F, 1.25F, 0.25F}, true);
  valid = check(gtk_widget_get_visible(ui.mixedOutputSection) &&
      ui.mixedOutputRows.size() == 4 && ui.outputLevelRows.size() == 2 &&
      gtk_style_context_has_class(gtk_widget_get_style_context(ui.mixedOutputRows[2].levelBar),
          "output-level-overload"),
      "mixed levels must display all routed channels and reuse overload colors without changing DSP meters") && valid;
  pipetune_gtk::renderMixedOutputLevels(ui, configuration, {}, true);
  valid = check(!gtk_widget_get_visible(ui.mixedOutputSection) && ui.mixedOutputRows.empty() &&
      ui.outputLevelRows.size() == 2,
      "missing mixed measurements must hide only the mixed section") && valid;
  gtk_widget_destroy(ui.outputLevelMeters);
  gtk_widget_destroy(ui.mixedOutputSection);
  return valid ? 0 : 1;
}
