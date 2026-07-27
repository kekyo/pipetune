cmake_minimum_required(VERSION 3.24)

project(PipeTune VERSION 0.1.0 LANGUAGES C CXX)

include(CTest)
include(GNUInstallDirs)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

get_filename_component(
  PIPETUNE_DEFAULT_DEPS_DIR
  "${CMAKE_CURRENT_SOURCE_DIR}/../deps"
  ABSOLUTE)
set(
  PIPETUNE_DEPS_DIR
  "${PIPETUNE_DEFAULT_DEPS_DIR}"
  CACHE PATH
  "Directory containing the EffeTune and yyjson dependencies")
set(PIPETUNE_EFFETUNE_DIR "${PIPETUNE_DEPS_DIR}/effetune")
set(PIPETUNE_YYJSON_DIR "${PIPETUNE_DEPS_DIR}/yyjson")
if(NOT EXISTS "${PIPETUNE_EFFETUNE_DIR}/dsp/CMakeLists.txt")
  message(
    FATAL_ERROR
    "EffeTune is unavailable under ${PIPETUNE_EFFETUNE_DIR}; "
    "initialize the workspace submodules")
endif()
if(NOT EXISTS "${PIPETUNE_YYJSON_DIR}/src/yyjson.c")
  message(
    FATAL_ERROR
    "yyjson is unavailable under ${PIPETUNE_YYJSON_DIR}; "
    "initialize the workspace submodules")
endif()

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)

find_program(PIPETUNE_NODE_EXECUTABLE node REQUIRED)
file(
  GLOB_RECURSE PIPETUNE_DSP_PARAM_SPECS
  CONFIGURE_DEPENDS
  "${PIPETUNE_EFFETUNE_DIR}/dsp/plugins/*/params.json")
set(PIPETUNE_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(PIPETUNE_GENERATED_DSP_CATALOG
    "${PIPETUNE_GENERATED_DIR}/dsp_catalog.generated.cpp")
add_custom_command(
  OUTPUT "${PIPETUNE_GENERATED_DSP_CATALOG}"
  COMMAND
    "${PIPETUNE_NODE_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate-dsp-catalog.mjs"
    "${PIPETUNE_GENERATED_DSP_CATALOG}"
  DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate-dsp-catalog.mjs"
    "${PIPETUNE_EFFETUNE_DIR}/scripts/gen-dsp-params.mjs"
    "${PIPETUNE_EFFETUNE_DIR}/plugins/plugins.txt"
    "${PIPETUNE_EFFETUNE_DIR}/dsp/registry.inc"
    ${PIPETUNE_DSP_PARAM_SPECS}
  VERBATIM)

set(ET_SIMD OFF CACHE BOOL "Match EffeTune's native scalar DSP configuration" FORCE)
add_subdirectory(
  "${PIPETUNE_EFFETUNE_DIR}/dsp"
  "${CMAKE_CURRENT_BINARY_DIR}/effetune-dsp")

add_library(yyjson STATIC "${PIPETUNE_YYJSON_DIR}/src/yyjson.c")
target_include_directories(yyjson PUBLIC "${PIPETUNE_YYJSON_DIR}/src")
set_target_properties(yyjson PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(
  pipetune_core STATIC
  src/version.cpp
  src/audio_bridge.cpp
  src/dsp_catalog.cpp
  src/dsp_pipeline.cpp
  "${PIPETUNE_GENERATED_DSP_CATALOG}")
target_include_directories(pipetune_core PUBLIC include PRIVATE src)
target_link_libraries(pipetune_core PUBLIC effetune_dsp_core yyjson)

if(MSVC)
  target_compile_options(pipetune_core PRIVATE /W4 /WX)
else()
  target_compile_options(pipetune_core PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

add_library(
  pipetune_pipewire STATIC
  src/command_line.cpp
  src/control_protocol.cpp
  src/control_socket.cpp
  src/default_sink_restore.cpp
  src/dsp_pipeline_slot.cpp
  src/output_device_tracker.cpp
  src/pipewire_pipeline.cpp)
target_include_directories(pipetune_pipewire PUBLIC include PRIVATE src)
target_link_libraries(
  pipetune_pipewire
  PUBLIC pipetune_core
  PRIVATE PkgConfig::PIPEWIRE Threads::Threads)
if(MSVC)
  target_compile_options(pipetune_pipewire PRIVATE /W4 /WX)
else()
  target_compile_options(
    pipetune_pipewire PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

add_executable(pipetune src/main.cpp)
set_target_properties(
  pipetune PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
target_include_directories(pipetune PRIVATE src)
target_link_libraries(pipetune PRIVATE pipetune_pipewire)
if(MSVC)
  target_compile_options(pipetune PRIVATE /W4 /WX)
else()
  target_compile_options(pipetune PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

set(
  PIPETUNE_SYSTEMD_USER_UNIT_DIR
  "lib/systemd/user"
  CACHE PATH
  "Installation directory for systemd user units")
set(
  PIPETUNE_INSTALLED_EXECUTABLE
  "${CMAKE_INSTALL_FULL_BINDIR}/pipetune")
configure_file(
  packaging/systemd/pipetune.service.in
  "${CMAKE_CURRENT_BINARY_DIR}/pipetune.service"
  @ONLY)
if(BUILD_TESTING)
  set(
    PIPETUNE_INSTALLED_EXECUTABLE
    "${CMAKE_BINARY_DIR}/pipetune")
  configure_file(
    packaging/systemd/pipetune.service.in
    "${CMAKE_CURRENT_BINARY_DIR}/pipetune.test.service"
    @ONLY)
endif()
set(
  PIPETUNE_DOCUMENTATION_DIR
  "${CMAKE_INSTALL_DATADIR}/doc/pipetune")

install(TARGETS pipetune RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(
  FILES "${CMAKE_CURRENT_BINARY_DIR}/pipetune.service"
  DESTINATION "${PIPETUNE_SYSTEMD_USER_UNIT_DIR}")
install(
  FILES packaging/pipetune.environment.example
  DESTINATION "${PIPETUNE_DOCUMENTATION_DIR}"
  RENAME environment.example)

if(BUILD_TESTING)
  add_test(NAME pipetune_cli_help COMMAND pipetune --help)
  set_tests_properties(
    pipetune_cli_help PROPERTIES
    PASS_REGULAR_EXPRESSION "\\.effetune_preset")

  add_executable(pipetune_bootstrap_tests test/bootstrap_test.cpp)
  target_link_libraries(pipetune_bootstrap_tests PRIVATE pipetune_core)
  if(MSVC)
    target_compile_options(pipetune_bootstrap_tests PRIVATE /W4 /WX)
  else()
    target_compile_options(
      pipetune_bootstrap_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
  add_test(NAME pipetune_bootstrap_tests COMMAND pipetune_bootstrap_tests)

  add_executable(
    pipetune_preset_pipeline_tests
    test/preset_pipeline_test.cpp)
  target_link_libraries(pipetune_preset_pipeline_tests PRIVATE pipetune_core)
  if(MSVC)
    target_compile_options(pipetune_preset_pipeline_tests PRIVATE /W4 /WX)
  else()
    target_compile_options(
      pipetune_preset_pipeline_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
  add_test(
    NAME pipetune_preset_pipeline_tests
    COMMAND pipetune_preset_pipeline_tests)

  add_executable(
    pipetune_audio_bridge_tests
    test/audio_bridge_test.cpp)
  target_include_directories(pipetune_audio_bridge_tests PRIVATE src)
  target_link_libraries(pipetune_audio_bridge_tests PRIVATE pipetune_core)
  if(MSVC)
    target_compile_options(pipetune_audio_bridge_tests PRIVATE /W4 /WX)
  else()
    target_compile_options(
      pipetune_audio_bridge_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
  add_test(
    NAME pipetune_audio_bridge_tests
    COMMAND pipetune_audio_bridge_tests)

  add_executable(
    pipetune_pipewire_pipeline_tests
    test/pipewire_pipeline_test.cpp)
  target_include_directories(pipetune_pipewire_pipeline_tests PRIVATE src)
  target_link_libraries(
    pipetune_pipewire_pipeline_tests PRIVATE pipetune_pipewire)
  if(MSVC)
    target_compile_options(pipetune_pipewire_pipeline_tests PRIVATE /W4 /WX)
  else()
    target_compile_options(
      pipetune_pipewire_pipeline_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
  add_test(
    NAME pipetune_pipewire_pipeline_tests
    COMMAND pipetune_pipewire_pipeline_tests)
  set_tests_properties(
    pipetune_pipewire_pipeline_tests PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 20)

  add_executable(
    pipetune_command_line_tests
    test/command_line_test.cpp)
  target_include_directories(pipetune_command_line_tests PRIVATE src)
  target_link_libraries(
    pipetune_command_line_tests PRIVATE pipetune_pipewire)
  if(MSVC)
    target_compile_options(pipetune_command_line_tests PRIVATE /W4 /WX)
  else()
    target_compile_options(
      pipetune_command_line_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
  add_test(
    NAME pipetune_command_line_tests
    COMMAND pipetune_command_line_tests)

  add_executable(
    pipetune_output_device_tracker_tests
    test/output_device_tracker_test.cpp)
  target_include_directories(pipetune_output_device_tracker_tests PRIVATE src)
  target_link_libraries(
    pipetune_output_device_tracker_tests PRIVATE pipetune_pipewire)
  if(MSVC)
    target_compile_options(pipetune_output_device_tracker_tests PRIVATE /W4 /WX)
  else()
    target_compile_options(
      pipetune_output_device_tracker_tests PRIVATE
      -Wall -Wextra -Wpedantic -Werror)
  endif()
  add_test(
    NAME pipetune_output_device_tracker_tests
    COMMAND pipetune_output_device_tracker_tests)

  add_executable(
    pipetune_dsp_pipeline_slot_tests
    test/dsp_pipeline_slot_test.cpp)
  target_include_directories(pipetune_dsp_pipeline_slot_tests PRIVATE src)
  target_link_libraries(
    pipetune_dsp_pipeline_slot_tests PRIVATE pipetune_pipewire Threads::Threads)
  if(MSVC)
    target_compile_options(pipetune_dsp_pipeline_slot_tests PRIVATE /W4 /WX)
  else()
    target_compile_options(
      pipetune_dsp_pipeline_slot_tests PRIVATE
      -Wall -Wextra -Wpedantic -Werror)
  endif()
  add_test(
    NAME pipetune_dsp_pipeline_slot_tests
    COMMAND pipetune_dsp_pipeline_slot_tests)

  add_executable(
    pipetune_control_protocol_tests
    test/control_protocol_test.cpp)
  target_include_directories(pipetune_control_protocol_tests PRIVATE src)
  target_link_libraries(
    pipetune_control_protocol_tests PRIVATE pipetune_pipewire)
  if(MSVC)
    target_compile_options(pipetune_control_protocol_tests PRIVATE /W4 /WX)
  else()
    target_compile_options(
      pipetune_control_protocol_tests PRIVATE
      -Wall -Wextra -Wpedantic -Werror)
  endif()
  add_test(
    NAME pipetune_control_protocol_tests
    COMMAND pipetune_control_protocol_tests)

  add_executable(
    pipetune_control_socket_tests
    test/control_socket_test.cpp)
  target_include_directories(pipetune_control_socket_tests PRIVATE src)
  target_link_libraries(
    pipetune_control_socket_tests PRIVATE pipetune_pipewire Threads::Threads)
  if(MSVC)
    target_compile_options(pipetune_control_socket_tests PRIVATE /W4 /WX)
  else()
    target_compile_options(
      pipetune_control_socket_tests PRIVATE
      -Wall -Wextra -Wpedantic -Werror)
  endif()
  add_test(
    NAME pipetune_control_socket_tests
    COMMAND pipetune_control_socket_tests)

  add_executable(
    pipetune_default_sink_restore_tests
    test/default_sink_restore_test.cpp)
  target_include_directories(pipetune_default_sink_restore_tests PRIVATE src)
  target_link_libraries(
    pipetune_default_sink_restore_tests PRIVATE pipetune_pipewire)
  if(MSVC)
    target_compile_options(pipetune_default_sink_restore_tests PRIVATE /W4 /WX)
  else()
    target_compile_options(
      pipetune_default_sink_restore_tests PRIVATE
      -Wall -Wextra -Wpedantic -Werror)
  endif()
  add_test(
    NAME pipetune_default_sink_restore_tests
    COMMAND pipetune_default_sink_restore_tests)
  set_tests_properties(
    pipetune_default_sink_restore_tests
    PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 10)

  add_executable(
    pipetune_param_packer_runner
    test/param_packer_runner.cpp)
  target_include_directories(pipetune_param_packer_runner PRIVATE src)
  target_link_libraries(pipetune_param_packer_runner PRIVATE pipetune_core)
  if(MSVC)
    target_compile_options(pipetune_param_packer_runner PRIVATE /W4 /WX)
  else()
    target_compile_options(
      pipetune_param_packer_runner PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()

  add_test(
    NAME pipetune_parameter_packer_parity
    COMMAND
      ${PIPETUNE_NODE_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/test/parameter-packer-parity.mjs
      $<TARGET_FILE:pipetune_param_packer_runner>
      ${PIPETUNE_EFFETUNE_DIR})

  add_test(
    NAME pipetune_effetune_native_parity
    COMMAND
      ${PIPETUNE_NODE_EXECUTABLE}
      ${PIPETUNE_EFFETUNE_DIR}/tools/dsp-parity/run.mjs
      --native
      --root ${PIPETUNE_EFFETUNE_DIR}
      --native-runner
      ${CMAKE_CURRENT_BINARY_DIR}/effetune-dsp/effetune-dsp-parity-runner)

  find_program(PIPETUNE_SYSTEMD_ANALYZE_EXECUTABLE systemd-analyze REQUIRED)
  add_test(
    NAME pipetune_install_layout
    COMMAND
      ${PIPETUNE_NODE_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/test/install-layout.mjs
      ${CMAKE_COMMAND}
      ${CMAKE_BINARY_DIR}
      ${PIPETUNE_SYSTEMD_ANALYZE_EXECUTABLE}
      ${CMAKE_INSTALL_PREFIX}
      ${CMAKE_INSTALL_BINDIR}
      ${PIPETUNE_SYSTEMD_USER_UNIT_DIR}
      ${PIPETUNE_DOCUMENTATION_DIR}
      ${CMAKE_CURRENT_BINARY_DIR}/pipetune.test.service)

  add_test(
    NAME pipetune_component_build
    COMMAND
      ${PIPETUNE_NODE_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/test/component-build.mjs
      ${CMAKE_COMMAND}
      ${CMAKE_CURRENT_SOURCE_DIR})
  set_tests_properties(
    pipetune_component_build PROPERTIES TIMEOUT 180)
endif()
