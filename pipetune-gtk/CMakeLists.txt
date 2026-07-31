cmake_minimum_required(VERSION 3.24)

include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(GIO REQUIRED IMPORTED_TARGET gio-2.0 gio-unix-2.0)
pkg_check_modules(GTK3 REQUIRED IMPORTED_TARGET gtk+-3.0 gdk-x11-3.0)
find_program(
  PIPETUNE_GTK_GLIB_COMPILE_RESOURCES
  glib-compile-resources
  REQUIRED)

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
  PIPETUNE_GTK_RESOURCE_DIRECTORY
  "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(
  PIPETUNE_GTK_RESOURCE_SOURCE
  "${PIPETUNE_GTK_RESOURCE_DIRECTORY}/pipetune-gtk-resources.c")
set(
  PIPETUNE_GTK_RESOURCE_HEADER
  "${PIPETUNE_GTK_RESOURCE_DIRECTORY}/pipetune-gtk-resources.h")
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
  VERBATIM)

add_library(
  pipetune_gtk_core STATIC
  src/action-log.cpp
  src/application-state.cpp
  src/configuration-reset-client.cpp
  src/control-client.cpp
  src/dsp-backend-operation.cpp
  src/dsp-backend-selection-model.cpp
  src/dsp-idle-operation.cpp
  src/dsp-idle-selection-model.cpp
  src/output-operation.cpp
  src/output-selection-model.cpp
  src/rate-operation.cpp
  src/rate-selection-model.cpp
  src/settings-transaction.cpp
  src/preset-catalog.cpp
  src/preset-file-monitor.cpp
  src/status-model.cpp
  src/status-text.cpp)
target_include_directories(pipetune_gtk_core PUBLIC src)
target_link_libraries(
  pipetune_gtk_core
  PUBLIC pipetune_control pipetune_core
  PRIVATE PkgConfig::GIO)
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
  src/status-icon.cpp
  src/tray-backend.cpp)
target_include_directories(
  pipetune_gtk_ui_core
  PUBLIC src
  PRIVATE "${PIPETUNE_GTK_RESOURCE_DIRECTORY}")
target_link_libraries(
  pipetune_gtk_ui_core
  PUBLIC pipetune_gtk_core PkgConfig::GTK3)
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
  DIRECTORY "${PIPETUNE_GTK_EFFETUNE_PRESET_SOURCE_DIR}/"
  DESTINATION "${PIPETUNE_GTK_EFFETUNE_PRESET_DIR}"
  FILES_MATCHING
  PATTERN "presets.txt"
  PATTERN "*.effetune_preset")

if(BUILD_TESTING)
  find_program(PIPETUNE_GTK_XVFB_RUN xvfb-run REQUIRED)

  add_test(NAME pipetune_gtk_help COMMAND pipetune-gtk --help)
  set_tests_properties(
    pipetune_gtk_help PROPERTIES
    PASS_REGULAR_EXPRESSION "--hidden")
  add_test(NAME pipetune_gtk_version COMMAND pipetune-gtk --version)
  set_tests_properties(
    pipetune_gtk_version PROPERTIES
    PASS_REGULAR_EXPRESSION "PipeTune GTK .* EffeTune DSP")

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
    pipetune_gtk_output_selection_model_tests
    test/output-selection-model-test.cpp)
  target_link_libraries(
    pipetune_gtk_output_selection_model_tests PRIVATE pipetune_gtk_core)
  if(MSVC)
    target_compile_options(
      pipetune_gtk_output_selection_model_tests PRIVATE /W4 /WX)
  else()
    target_compile_options(
      pipetune_gtk_output_selection_model_tests PRIVATE
      -Wall -Wextra -Wpedantic -Werror)
  endif()
  add_test(
    NAME pipetune_gtk_output_selection_model_tests
    COMMAND pipetune_gtk_output_selection_model_tests)

  add_executable(
    pipetune_gtk_output_operation_tests
    test/output-operation-test.cpp)
  target_link_libraries(
    pipetune_gtk_output_operation_tests PRIVATE pipetune_gtk_core)
  if(MSVC)
    target_compile_options(
      pipetune_gtk_output_operation_tests PRIVATE /W4 /WX)
  else()
    target_compile_options(
      pipetune_gtk_output_operation_tests PRIVATE
      -Wall -Wextra -Wpedantic -Werror)
  endif()
  add_test(
    NAME pipetune_gtk_output_operation_tests
    COMMAND pipetune_gtk_output_operation_tests)

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
    pipetune_gtk_dsp_idle_selection_model_tests
    test/dsp-idle-selection-model-test.cpp)
  target_link_libraries(
    pipetune_gtk_dsp_idle_selection_model_tests
    PRIVATE pipetune_gtk_core)
  if(MSVC)
    target_compile_options(
      pipetune_gtk_dsp_idle_selection_model_tests PRIVATE /W4 /WX)
  else()
    target_compile_options(
      pipetune_gtk_dsp_idle_selection_model_tests PRIVATE
      -Wall -Wextra -Wpedantic -Werror)
  endif()
  add_test(
    NAME pipetune_gtk_dsp_idle_selection_model_tests
    COMMAND pipetune_gtk_dsp_idle_selection_model_tests)

  add_executable(
    pipetune_gtk_dsp_idle_operation_tests
    test/dsp-idle-operation-test.cpp)
  target_link_libraries(
    pipetune_gtk_dsp_idle_operation_tests
    PRIVATE pipetune_gtk_core)
  if(MSVC)
    target_compile_options(
      pipetune_gtk_dsp_idle_operation_tests PRIVATE /W4 /WX)
  else()
    target_compile_options(
      pipetune_gtk_dsp_idle_operation_tests PRIVATE
      -Wall -Wextra -Wpedantic -Werror)
  endif()
  add_test(
    NAME pipetune_gtk_dsp_idle_operation_tests
    COMMAND pipetune_gtk_dsp_idle_operation_tests)

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
      $<TARGET_FILE:pipetune_gtk_main_window_tests>)

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

  find_program(PIPETUNE_GTK_NODE_EXECUTABLE node REQUIRED)
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
      ${PIPETUNE_GTK_EFFETUNE_PRESET_DIR})
endif()
