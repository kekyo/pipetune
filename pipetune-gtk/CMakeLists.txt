cmake_minimum_required(VERSION 3.24)

include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
find_package(Intl REQUIRED)
pkg_check_modules(GIO REQUIRED IMPORTED_TARGET gio-2.0 gio-unix-2.0)
pkg_check_modules(GTK3 REQUIRED IMPORTED_TARGET gtk+-3.0 gdk-x11-3.0)
find_program(
  PIPETUNE_GTK_GLIB_COMPILE_RESOURCES
  glib-compile-resources
  REQUIRED)

set(PIPETUNE_GTK_GETTEXT_DOMAIN "pipetune-gtk")
set(PIPETUNE_GTK_PO_FILE "${CMAKE_CURRENT_SOURCE_DIR}/po/ja.po")
set(PIPETUNE_GTK_LOCALE_BUILD_DIR "${CMAKE_CURRENT_BINARY_DIR}/locale")
set(
  PIPETUNE_GTK_JAPANESE_CATALOG
  "${PIPETUNE_GTK_LOCALE_BUILD_DIR}/ja/LC_MESSAGES/${PIPETUNE_GTK_GETTEXT_DOMAIN}.mo")
find_program(PIPETUNE_GTK_MSGFMT_EXECUTABLE msgfmt)
if(PIPETUNE_GTK_MSGFMT_EXECUTABLE)
  add_custom_command(
    OUTPUT "${PIPETUNE_GTK_JAPANESE_CATALOG}"
    COMMAND
      "${CMAKE_COMMAND}" -E make_directory
      "${PIPETUNE_GTK_LOCALE_BUILD_DIR}/ja/LC_MESSAGES"
    COMMAND
      "${PIPETUNE_GTK_MSGFMT_EXECUTABLE}"
      --check
      -o
      "${PIPETUNE_GTK_JAPANESE_CATALOG}"
      "${PIPETUNE_GTK_PO_FILE}"
    DEPENDS "${PIPETUNE_GTK_PO_FILE}"
    VERBATIM)
else()
  find_program(PIPETUNE_GTK_TRANSLATION_NODE_EXECUTABLE node REQUIRED)
  add_custom_command(
    OUTPUT "${PIPETUNE_GTK_JAPANESE_CATALOG}"
    COMMAND
      "${PIPETUNE_GTK_TRANSLATION_NODE_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/scripts/compile-po.mjs"
      "${PIPETUNE_GTK_PO_FILE}"
      "${PIPETUNE_GTK_JAPANESE_CATALOG}"
    DEPENDS
      "${PIPETUNE_GTK_PO_FILE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/scripts/compile-po.mjs"
    VERBATIM)
endif()
add_custom_target(
  pipetune_gtk_translations ALL
  DEPENDS "${PIPETUNE_GTK_JAPANESE_CATALOG}")

if(BUILD_TESTING)
  find_program(PIPETUNE_GTK_NPM npm REQUIRED)
  find_program(PIPETUNE_GTK_NODE_EXECUTABLE node REQUIRED)
  set(
    PIPETUNE_GTK_E2E_DIRECTORY
    "${CMAKE_CURRENT_SOURCE_DIR}/test/e2e")
  set(
    PIPETUNE_GTK_E2E_INSTALL_STAMP
    "${PIPETUNE_GTK_E2E_DIRECTORY}/node_modules/.pipetune-ci-stamp")
  add_custom_command(
    OUTPUT "${PIPETUNE_GTK_E2E_INSTALL_STAMP}"
    COMMAND "${PIPETUNE_GTK_NPM}" ci
    COMMAND
      "${CMAKE_COMMAND}" -E touch
      "${PIPETUNE_GTK_E2E_INSTALL_STAMP}"
    WORKING_DIRECTORY "${PIPETUNE_GTK_E2E_DIRECTORY}"
    DEPENDS
      "${PIPETUNE_GTK_E2E_DIRECTORY}/package.json"
      "${PIPETUNE_GTK_E2E_DIRECTORY}/package-lock.json"
    VERBATIM)
  add_custom_target(
    pipetune_gtk_e2e_dependencies ALL
    DEPENDS "${PIPETUNE_GTK_E2E_INSTALL_STAMP}")
endif()

set(
  PIPETUNE_GTK_RESOURCE_MANIFEST
  "${CMAKE_CURRENT_SOURCE_DIR}/resources/pipetune-gtk.gresource.xml")
set(
  PIPETUNE_GTK_ICON_SOURCE
  "${CMAKE_CURRENT_SOURCE_DIR}/packaging/icons/hicolor/scalable/apps/pipetune.svg")
set(
  PIPETUNE_GTK_MAIN_WINDOW_UI
  "${CMAKE_CURRENT_SOURCE_DIR}/resources/main-window.ui")
set(
  PIPETUNE_GTK_STYLE
  "${CMAKE_CURRENT_SOURCE_DIR}/resources/style.css")
set(
  PIPETUNE_GTK_RESOURCE_DIRECTORY
  "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(
  PIPETUNE_GTK_RESOURCE_SOURCE
  "${PIPETUNE_GTK_RESOURCE_DIRECTORY}/pipetune-gtk-resources.c")
set(
  PIPETUNE_GTK_RESOURCE_HEADER
  "${PIPETUNE_GTK_RESOURCE_DIRECTORY}/pipetune-gtk-resources.h")
if(BUILD_TESTING)
  set(
    PIPETUNE_GTK_RUNTIME_LOCALE_DIR
    "${PIPETUNE_GTK_LOCALE_BUILD_DIR}")
else()
  set(
    PIPETUNE_GTK_RUNTIME_LOCALE_DIR
    "${CMAKE_INSTALL_FULL_LOCALEDIR}")
endif()
configure_file(
  src/installed-locales.h.in
  "${PIPETUNE_GTK_RESOURCE_DIRECTORY}/installed-locales.h"
  @ONLY)
add_custom_command(
  OUTPUT
    "${PIPETUNE_GTK_RESOURCE_SOURCE}"
    "${PIPETUNE_GTK_RESOURCE_HEADER}"
  COMMAND
    "${CMAKE_COMMAND}" -E make_directory
    "${PIPETUNE_GTK_RESOURCE_DIRECTORY}"
  COMMAND
    "${PIPETUNE_GTK_GLIB_COMPILE_RESOURCES}"
    "${PIPETUNE_GTK_RESOURCE_MANIFEST}"
    --sourcedir=${CMAKE_CURRENT_SOURCE_DIR}
    --target=${PIPETUNE_GTK_RESOURCE_SOURCE}
    --generate-source
    --manual-register
    --c-name=pipetune_gtk_resources
  COMMAND
    "${PIPETUNE_GTK_GLIB_COMPILE_RESOURCES}"
    "${PIPETUNE_GTK_RESOURCE_MANIFEST}"
    --sourcedir=${CMAKE_CURRENT_SOURCE_DIR}
    --target=${PIPETUNE_GTK_RESOURCE_HEADER}
    --generate-header
    --manual-register
    --c-name=pipetune_gtk_resources
  DEPENDS
    "${PIPETUNE_GTK_RESOURCE_MANIFEST}"
    "${PIPETUNE_GTK_ICON_SOURCE}"
    "${PIPETUNE_GTK_MAIN_WINDOW_UI}"
    "${PIPETUNE_GTK_STYLE}"
  VERBATIM)
if(NOT MSVC)
  set_source_files_properties(
    "${PIPETUNE_GTK_RESOURCE_SOURCE}"
    PROPERTIES COMPILE_OPTIONS "-Wno-overlength-strings")
endif()

add_library(
  pipetune_gtk_core STATIC
  src/action-log.cpp
  src/application-state.cpp
  src/configuration-reset-client.cpp
  src/control-client.cpp
  src/dsp-backend-operation.cpp
  src/dsp-backend-selection-model.cpp
  src/localization.cpp
  src/rate-operation.cpp
  src/rate-selection-model.cpp
  src/settings-transaction.cpp
  src/preset-catalog.cpp
  src/preset-file-monitor.cpp
  src/status-model.cpp
  src/status-text.cpp
  src/ui-language.cpp
  src/ui-message.cpp)
target_include_directories(pipetune_gtk_core PUBLIC src)
target_link_libraries(
  pipetune_gtk_core
  PUBLIC pipetune_control pipetune_core
  PRIVATE Intl::Intl PkgConfig::GIO)
add_dependencies(pipetune_gtk_core pipetune_gtk_translations)
if(MSVC)
  target_compile_options(pipetune_gtk_core PRIVATE /W4 /WX)
else()
  target_compile_options(
    pipetune_gtk_core PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

add_library(
  pipetune_gtk_ui_core STATIC
  "${PIPETUNE_GTK_RESOURCE_SOURCE}"
  src/gtk-resources.cpp
  src/launch-options.cpp
  src/main-window.cpp
  src/status-level-meter.cpp
  src/status-icon.cpp
  src/tray-backend.cpp)
target_include_directories(
  pipetune_gtk_ui_core
  PUBLIC src
  PRIVATE "${PIPETUNE_GTK_RESOURCE_DIRECTORY}")
target_link_libraries(
  pipetune_gtk_ui_core
  PUBLIC pipetune_gtk_core PkgConfig::GTK3)
if(BUILD_TESTING)
  add_dependencies(
    pipetune_gtk_ui_core pipetune_gtk_e2e_dependencies)
  target_include_directories(
    pipetune_gtk_ui_core
    PRIVATE
      "${PIPETUNE_GTK_E2E_DIRECTORY}/node_modules/gestament/include")
  target_compile_definitions(
    pipetune_gtk_ui_core
    PRIVATE PIPETUNE_GTK_E2E_ACCESSIBILITY=1)
endif()
if(MSVC)
  target_compile_options(pipetune_gtk_ui_core PRIVATE /W4 /WX)
else()
  target_compile_options(
    pipetune_gtk_ui_core PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

add_executable(pipetune-gtk src/main.cpp)
set_target_properties(
  pipetune-gtk PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
target_link_libraries(
  pipetune-gtk
  PRIVATE pipetune_gtk_core pipetune_gtk_ui_core pipetune_core
          PkgConfig::GTK3)
if(BUILD_TESTING)
  add_dependencies(pipetune-gtk pipetune_gtk_e2e_dependencies)
  target_include_directories(
    pipetune-gtk
    PRIVATE
      "${PIPETUNE_GTK_E2E_DIRECTORY}/node_modules/gestament/include")
  target_compile_definitions(
    pipetune-gtk
    PRIVATE PIPETUNE_GTK_E2E_ACCESSIBILITY=1)
endif()
if(MSVC)
  target_compile_options(pipetune-gtk PRIVATE /W4 /WX)
else()
  target_compile_options(
    pipetune-gtk PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

set(
  PIPETUNE_GTK_APPLICATION_DIR
  "${CMAKE_INSTALL_DATADIR}/applications"
  CACHE PATH
  "Installation directory for desktop applications")
set(
  PIPETUNE_GTK_AUTOSTART_DIR
  "/etc/xdg/autostart"
  CACHE PATH
  "Installation directory for system-wide XDG autostart entries")
set(
  PIPETUNE_GTK_ICON_DIR
  "${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps"
  CACHE PATH
  "Installation directory for scalable application icons")
set(
  PIPETUNE_GTK_EFFETUNE_PRESET_DIR
  "${CMAKE_INSTALL_DATADIR}/pipetune/effetune-presets"
  CACHE PATH
  "Installation directory for bundled EffeTune presets")
set(
  PIPETUNE_GTK_EFFETUNE_PRESET_SOURCE_DIR
  "${CMAKE_CURRENT_SOURCE_DIR}/../deps/effetune/presets")
if(NOT EXISTS
   "${PIPETUNE_GTK_EFFETUNE_PRESET_SOURCE_DIR}/presets.txt")
  message(
    FATAL_ERROR
    "EffeTune standard presets are unavailable: "
    "${PIPETUNE_GTK_EFFETUNE_PRESET_SOURCE_DIR}")
endif()
if(BUILD_TESTING)
  set(
    PIPETUNE_GTK_RUNTIME_EFFETUNE_PRESET_DIR
    "${PIPETUNE_GTK_EFFETUNE_PRESET_SOURCE_DIR}")
elseif(IS_ABSOLUTE "${PIPETUNE_GTK_EFFETUNE_PRESET_DIR}")
  set(
    PIPETUNE_GTK_RUNTIME_EFFETUNE_PRESET_DIR
    "${PIPETUNE_GTK_EFFETUNE_PRESET_DIR}")
else()
  set(
    PIPETUNE_GTK_RUNTIME_EFFETUNE_PRESET_DIR
    "${CMAKE_INSTALL_PREFIX}/${PIPETUNE_GTK_EFFETUNE_PRESET_DIR}")
endif()
configure_file(
  src/installed-presets.h.in
  "${PIPETUNE_GTK_RESOURCE_DIRECTORY}/installed-presets.h"
  @ONLY)
if(BUILD_TESTING)
  set(
    PIPETUNE_GTK_RUNTIME_PIPETUNE_EXECUTABLE
    "${CMAKE_BINARY_DIR}/pipetune")
else()
  set(
    PIPETUNE_GTK_RUNTIME_PIPETUNE_EXECUTABLE
    "${CMAKE_INSTALL_FULL_BINDIR}/pipetune")
endif()
configure_file(
  src/installed-tools.h.in
  "${PIPETUNE_GTK_RESOURCE_DIRECTORY}/installed-tools.h"
  @ONLY)
target_include_directories(
  pipetune-gtk PRIVATE "${PIPETUNE_GTK_RESOURCE_DIRECTORY}")

install(
  TARGETS pipetune-gtk
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(
  FILES packaging/applications/net.kekyo.pipetune-gtk.desktop
  DESTINATION "${PIPETUNE_GTK_APPLICATION_DIR}")
install(
  FILES packaging/autostart/net.kekyo.pipetune-gtk.desktop
  DESTINATION "${PIPETUNE_GTK_AUTOSTART_DIR}")
install(
  FILES packaging/icons/hicolor/scalable/apps/pipetune.svg
  DESTINATION "${PIPETUNE_GTK_ICON_DIR}")
install(
  FILES "${PIPETUNE_GTK_JAPANESE_CATALOG}"
  DESTINATION
    "${CMAKE_INSTALL_LOCALEDIR}/ja/LC_MESSAGES")
install(
  DIRECTORY "${PIPETUNE_GTK_EFFETUNE_PRESET_SOURCE_DIR}/"
  DESTINATION "${PIPETUNE_GTK_EFFETUNE_PRESET_DIR}"
  FILES_MATCHING
  PATTERN "presets.txt"
  PATTERN "*.effetune_preset")

if(BUILD_TESTING)
  find_program(PIPETUNE_GTK_XVFB_RUN xvfb-run REQUIRED)

  add_executable(
    pipetune_gtk_e2e_daemon
    test/e2e/fake-control-daemon.cpp)
  set_target_properties(
    pipetune_gtk_e2e_daemon PROPERTIES
    OUTPUT_NAME pipetune-gtk-e2e-daemon
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
  target_link_libraries(
    pipetune_gtk_e2e_daemon
    PRIVATE pipetune_control Threads::Threads)
  if(MSVC)
    target_compile_options(pipetune_gtk_e2e_daemon PRIVATE /W4 /WX)
  else()
    target_compile_options(
      pipetune_gtk_e2e_daemon PRIVATE
      -Wall -Wextra -Wpedantic -Werror)
  endif()

  add_test(NAME pipetune_gtk_help COMMAND pipetune-gtk --help)
  set_tests_properties(
    pipetune_gtk_help PROPERTIES
    PASS_REGULAR_EXPRESSION "--hidden")
  add_test(NAME pipetune_gtk_version COMMAND pipetune-gtk --version)
  set_tests_properties(
    pipetune_gtk_version PROPERTIES
    PASS_REGULAR_EXPRESSION "PipeTune GTK .* EffeTune DSP")

  add_executable(
    pipetune_gtk_ui_language_tests
    test/ui-language-test.cpp)
  target_link_libraries(
    pipetune_gtk_ui_language_tests
    PRIVATE pipetune_gtk_core PkgConfig::GIO)
  if(MSVC)
    target_compile_options(
      pipetune_gtk_ui_language_tests PRIVATE /W4 /WX)
  else()
    target_compile_options(
      pipetune_gtk_ui_language_tests PRIVATE
      -Wall -Wextra -Wpedantic -Werror)
  endif()
  add_test(
    NAME pipetune_gtk_ui_language_tests
    COMMAND
      pipetune_gtk_ui_language_tests
      "${CMAKE_CURRENT_BINARY_DIR}/locale")

  add_executable(
    pipetune_gtk_ui_message_tests
    test/ui-message-test.cpp)
  target_link_libraries(
    pipetune_gtk_ui_message_tests
    PRIVATE pipetune_gtk_core)
  if(MSVC)
    target_compile_options(
      pipetune_gtk_ui_message_tests PRIVATE /W4 /WX)
  else()
    target_compile_options(
      pipetune_gtk_ui_message_tests PRIVATE
      -Wall -Wextra -Wpedantic -Werror)
  endif()
  add_test(
    NAME pipetune_gtk_ui_message_tests
    COMMAND
      pipetune_gtk_ui_message_tests
      "${CMAKE_CURRENT_BINARY_DIR}/locale")

  add_test(
    NAME pipetune_gtk_help_remains_english
    COMMAND
      "${CMAKE_COMMAND}" -E env
      "LC_ALL=C.UTF-8"
      "LANGUAGE=ja"
      $<TARGET_FILE:pipetune-gtk> --help)
  set_tests_properties(
    pipetune_gtk_help_remains_english PROPERTIES
    PASS_REGULAR_EXPRESSION "Show this help text")
  add_test(
    NAME pipetune_gtk_cli_remains_english
    COMMAND
      ${PIPETUNE_GTK_NODE_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/test/cli-language.mjs
      $<TARGET_FILE:pipetune-gtk>)
  add_test(
    NAME pipetune_gtk_translation_catalog
    COMMAND
      ${PIPETUNE_GTK_NODE_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check-translations.mjs
      ${CMAKE_CURRENT_SOURCE_DIR})
  if(PIPETUNE_GTK_MSGFMT_EXECUTABLE)
    add_test(
      NAME pipetune_gtk_msgfmt_build
      COMMAND
        "${PIPETUNE_GTK_NODE_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/test/msgfmt-build.mjs"
        "${CMAKE_COMMAND}"
        "${CMAKE_SOURCE_DIR}"
        "${PIPETUNE_GTK_MSGFMT_EXECUTABLE}")
    set_tests_properties(
      pipetune_gtk_msgfmt_build PROPERTIES TIMEOUT 30)
  endif()

  add_executable(
    pipetune_gtk_state_config_tests
    test/state-config-test.cpp)
  target_link_libraries(
    pipetune_gtk_state_config_tests PRIVATE pipetune_gtk_core)
  if(MSVC)
    target_compile_options(pipetune_gtk_state_config_tests PRIVATE /W4 /WX)
  else()
    target_compile_options(
      pipetune_gtk_state_config_tests PRIVATE
      -Wall -Wextra -Wpedantic -Werror)
  endif()
  add_test(
    NAME pipetune_gtk_state_config_tests
    COMMAND pipetune_gtk_state_config_tests)

  add_executable(
    pipetune_gtk_dialog_model_tests
    test/dialog-model-test.cpp)
  target_link_libraries(
    pipetune_gtk_dialog_model_tests PRIVATE pipetune_gtk_core)
  if(MSVC)
    target_compile_options(pipetune_gtk_dialog_model_tests PRIVATE /W4 /WX)
  else()
    target_compile_options(
      pipetune_gtk_dialog_model_tests PRIVATE
      -Wall -Wextra -Wpedantic -Werror)
  endif()
  add_test(
    NAME pipetune_gtk_dialog_model_tests
    COMMAND pipetune_gtk_dialog_model_tests)

  add_executable(
    pipetune_gtk_rate_selection_model_tests
    test/rate-selection-model-test.cpp)
  target_link_libraries(
    pipetune_gtk_rate_selection_model_tests PRIVATE pipetune_gtk_core)
  if(MSVC)
    target_compile_options(
      pipetune_gtk_rate_selection_model_tests PRIVATE /W4 /WX)
  else()
    target_compile_options(
      pipetune_gtk_rate_selection_model_tests PRIVATE
      -Wall -Wextra -Wpedantic -Werror)
  endif()
  add_test(
    NAME pipetune_gtk_rate_selection_model_tests
    COMMAND pipetune_gtk_rate_selection_model_tests)

  add_executable(
    pipetune_gtk_rate_operation_tests
    test/rate-operation-test.cpp)
  target_link_libraries(
    pipetune_gtk_rate_operation_tests PRIVATE pipetune_gtk_core)
  if(MSVC)
    target_compile_options(
      pipetune_gtk_rate_operation_tests PRIVATE /W4 /WX)
  else()
    target_compile_options(
      pipetune_gtk_rate_operation_tests PRIVATE
      -Wall -Wextra -Wpedantic -Werror)
  endif()
  add_test(
    NAME pipetune_gtk_rate_operation_tests
    COMMAND pipetune_gtk_rate_operation_tests)

  add_executable(
    pipetune_gtk_dsp_backend_selection_model_tests
    test/dsp-backend-selection-model-test.cpp)
  target_link_libraries(
    pipetune_gtk_dsp_backend_selection_model_tests
    PRIVATE pipetune_gtk_core)
  if(MSVC)
    target_compile_options(
      pipetune_gtk_dsp_backend_selection_model_tests PRIVATE /W4 /WX)
  else()
    target_compile_options(
      pipetune_gtk_dsp_backend_selection_model_tests PRIVATE
      -Wall -Wextra -Wpedantic -Werror)
  endif()
  add_test(
    NAME pipetune_gtk_dsp_backend_selection_model_tests
    COMMAND pipetune_gtk_dsp_backend_selection_model_tests)

  add_executable(
    pipetune_gtk_dsp_backend_operation_tests
    test/dsp-backend-operation-test.cpp)
  target_link_libraries(
    pipetune_gtk_dsp_backend_operation_tests
    PRIVATE pipetune_gtk_core)
  add_dependencies(
    pipetune_gtk_dsp_backend_operation_tests
    pipetune_test_dsp_backend_copies)
  if(MSVC)
    target_compile_options(
      pipetune_gtk_dsp_backend_operation_tests PRIVATE /W4 /WX)
  else()
    target_compile_options(
      pipetune_gtk_dsp_backend_operation_tests PRIVATE
      -Wall -Wextra -Wpedantic -Werror)
  endif()
  add_test(
    NAME pipetune_gtk_dsp_backend_operation_tests
    COMMAND pipetune_gtk_dsp_backend_operation_tests)

  add_executable(
    pipetune_gtk_preset_catalog_tests
    test/preset-catalog-test.cpp)
  target_link_libraries(
    pipetune_gtk_preset_catalog_tests
    PRIVATE pipetune_gtk_core PkgConfig::GIO)
  if(MSVC)
    target_compile_options(pipetune_gtk_preset_catalog_tests PRIVATE /W4 /WX)
  else()
    target_compile_options(
      pipetune_gtk_preset_catalog_tests PRIVATE
      -Wall -Wextra -Wpedantic -Werror)
  endif()
  add_test(
    NAME pipetune_gtk_preset_catalog_tests
    COMMAND
      pipetune_gtk_preset_catalog_tests
      "${PIPETUNE_GTK_EFFETUNE_PRESET_SOURCE_DIR}")

  add_executable(
    pipetune_gtk_status_text_tests
    test/status-text-test.cpp)
  target_link_libraries(
    pipetune_gtk_status_text_tests PRIVATE pipetune_gtk_core)
  if(MSVC)
    target_compile_options(pipetune_gtk_status_text_tests PRIVATE /W4 /WX)
  else()
    target_compile_options(
      pipetune_gtk_status_text_tests PRIVATE
      -Wall -Wextra -Wpedantic -Werror)
  endif()
  add_test(
    NAME pipetune_gtk_status_text_tests
    COMMAND pipetune_gtk_status_text_tests)

  add_executable(
    pipetune_gtk_control_client_tests
    test/control-client-test.cpp)
  target_link_libraries(
    pipetune_gtk_control_client_tests
    PRIVATE pipetune_gtk_core PkgConfig::GIO Threads::Threads)
  if(MSVC)
    target_compile_options(pipetune_gtk_control_client_tests PRIVATE /W4 /WX)
  else()
    target_compile_options(
      pipetune_gtk_control_client_tests PRIVATE
      -Wall -Wextra -Wpedantic -Werror)
  endif()
  add_test(
    NAME pipetune_gtk_control_client_tests
    COMMAND pipetune_gtk_control_client_tests)
  set_tests_properties(
    pipetune_gtk_control_client_tests PROPERTIES TIMEOUT 10)

  add_executable(
    pipetune_gtk_configuration_reset_helper
    test/configuration-reset-helper.cpp)
  if(MSVC)
    target_compile_options(
      pipetune_gtk_configuration_reset_helper PRIVATE /W4 /WX)
  else()
    target_compile_options(
      pipetune_gtk_configuration_reset_helper PRIVATE
      -Wall -Wextra -Wpedantic -Werror)
  endif()

  add_executable(
    pipetune_gtk_configuration_reset_client_tests
    test/configuration-reset-client-test.cpp)
  target_link_libraries(
    pipetune_gtk_configuration_reset_client_tests
    PRIVATE pipetune_gtk_core PkgConfig::GIO)
  if(MSVC)
    target_compile_options(
      pipetune_gtk_configuration_reset_client_tests PRIVATE /W4 /WX)
  else()
    target_compile_options(
      pipetune_gtk_configuration_reset_client_tests PRIVATE
      -Wall -Wextra -Wpedantic -Werror)
  endif()
  add_test(
    NAME pipetune_gtk_configuration_reset_client_tests
    COMMAND
      pipetune_gtk_configuration_reset_client_tests
      $<TARGET_FILE:pipetune_gtk_configuration_reset_helper>)
  set_tests_properties(
    pipetune_gtk_configuration_reset_client_tests PROPERTIES TIMEOUT 10)

  add_executable(
    pipetune_gtk_tray_backend_tests
    test/tray-backend-test.cpp)
  target_link_libraries(
    pipetune_gtk_tray_backend_tests PRIVATE pipetune_gtk_ui_core)
  if(MSVC)
    target_compile_options(pipetune_gtk_tray_backend_tests PRIVATE /W4 /WX)
  else()
    target_compile_options(
      pipetune_gtk_tray_backend_tests PRIVATE
      -Wall -Wextra -Wpedantic -Werror)
  endif()
  add_test(
    NAME pipetune_gtk_tray_backend_tests
    COMMAND pipetune_gtk_tray_backend_tests)

  add_executable(
    pipetune_gtk_main_window_tests
    test/main-window-test.cpp)
  target_link_libraries(
    pipetune_gtk_main_window_tests PRIVATE pipetune_gtk_ui_core)
  if(MSVC)
    target_compile_options(pipetune_gtk_main_window_tests PRIVATE /W4 /WX)
  else()
    target_compile_options(
      pipetune_gtk_main_window_tests PRIVATE
      -Wall -Wextra -Wpedantic -Werror)
  endif()
  add_test(
    NAME pipetune_gtk_main_window_tests
    COMMAND
      ${PIPETUNE_GTK_XVFB_RUN} -a
      $<TARGET_FILE:pipetune_gtk_main_window_tests>
      "${CMAKE_CURRENT_BINARY_DIR}/locale")

  add_executable(
    pipetune_gtk_status_level_meter_tests
    test/status-level-meter-test.cpp)
  target_link_libraries(
    pipetune_gtk_status_level_meter_tests PRIVATE pipetune_gtk_ui_core)
  if(MSVC)
    target_compile_options(
      pipetune_gtk_status_level_meter_tests PRIVATE /W4 /WX)
  else()
    target_compile_options(
      pipetune_gtk_status_level_meter_tests PRIVATE
      -Wall -Wextra -Wpedantic -Werror)
  endif()
  add_test(
    NAME pipetune_gtk_status_level_meter_tests
    COMMAND
      ${PIPETUNE_GTK_XVFB_RUN} -a
      $<TARGET_FILE:pipetune_gtk_status_level_meter_tests>)

  add_executable(
    pipetune_gtk_status_icon_tests
    test/status-icon-test.cpp)
  target_link_libraries(
    pipetune_gtk_status_icon_tests PRIVATE pipetune_gtk_ui_core)
  if(MSVC)
    target_compile_options(pipetune_gtk_status_icon_tests PRIVATE /W4 /WX)
  else()
    target_compile_options(
      pipetune_gtk_status_icon_tests PRIVATE
      -Wall -Wextra -Wpedantic -Werror)
  endif()
  add_test(
    NAME pipetune_gtk_status_icon_tests
    COMMAND pipetune_gtk_status_icon_tests)

  add_executable(
    pipetune_gtk_launch_options_tests
    test/launch-options-test.cpp)
  target_link_libraries(
    pipetune_gtk_launch_options_tests PRIVATE pipetune_gtk_ui_core)
  if(MSVC)
    target_compile_options(pipetune_gtk_launch_options_tests PRIVATE /W4 /WX)
  else()
    target_compile_options(
      pipetune_gtk_launch_options_tests PRIVATE
      -Wall -Wextra -Wpedantic -Werror)
  endif()
  add_test(
    NAME pipetune_gtk_launch_options_tests
    COMMAND pipetune_gtk_launch_options_tests)

  find_program(PIPETUNE_GTK_DBUS_RUN_SESSION dbus-run-session REQUIRED)
  find_program(PIPETUNE_GTK_GDBUS gdbus REQUIRED)
  find_program(
    PIPETUNE_GTK_DESKTOP_FILE_VALIDATE desktop-file-validate REQUIRED)
  find_program(
    PIPETUNE_GTK_PIXBUF_THUMBNAILER gdk-pixbuf-thumbnailer REQUIRED)
  add_test(
    NAME pipetune_gtk_application_lifecycle
    COMMAND
      ${PIPETUNE_GTK_DBUS_RUN_SESSION}
      --config-file=${CMAKE_CURRENT_SOURCE_DIR}/test/dbus-session.conf --
      ${PIPETUNE_GTK_XVFB_RUN} -a
      ${PIPETUNE_GTK_NODE_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/test/application-lifecycle.mjs
      $<TARGET_FILE:pipetune-gtk>
      $<TARGET_FILE:pipetune>
      ${PIPETUNE_GTK_GDBUS})
  set_tests_properties(
    pipetune_gtk_application_lifecycle PROPERTIES TIMEOUT 60)

  add_test(
    NAME pipetune_gtk_gestament_e2e
    COMMAND
      "${CMAKE_COMMAND}" -E env
      "PIPETUNE_GTK_BINARY=$<TARGET_FILE:pipetune-gtk>"
      "PIPETUNE_GTK_E2E_DAEMON=$<TARGET_FILE:pipetune_gtk_e2e_daemon>"
      "${PIPETUNE_GTK_NPM}" test)
  set_tests_properties(
    pipetune_gtk_gestament_e2e PROPERTIES
    WORKING_DIRECTORY "${PIPETUNE_GTK_E2E_DIRECTORY}"
    TIMEOUT 180)

  add_test(
    NAME pipetune_gtk_install_layout
    COMMAND
      ${PIPETUNE_GTK_NODE_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/test/install-layout.mjs
      ${CMAKE_COMMAND}
      ${CMAKE_BINARY_DIR}
      ${PIPETUNE_GTK_DESKTOP_FILE_VALIDATE}
      ${PIPETUNE_GTK_PIXBUF_THUMBNAILER}
      ${CMAKE_INSTALL_PREFIX}
      ${CMAKE_INSTALL_BINDIR}
      ${PIPETUNE_GTK_APPLICATION_DIR}
      ${PIPETUNE_GTK_AUTOSTART_DIR}
      ${PIPETUNE_GTK_ICON_DIR}
      ${PIPETUNE_GTK_EFFETUNE_PRESET_DIR}
      ${CMAKE_INSTALL_LOCALEDIR})
endif()
