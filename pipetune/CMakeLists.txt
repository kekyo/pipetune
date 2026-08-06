cmake_minimum_required(VERSION 3.24)

project(PipeTune LANGUAGES C CXX)

include(CTest)
include(GNUInstallDirs)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT DEFINED PIPETUNE_BUILD_VERSION)
  find_program(PIPETUNE_NPX_EXECUTABLE npx REQUIRED)
  execute_process(
    COMMAND
      "${PIPETUNE_NPX_EXECUTABLE}" screw-up format -e "{version}" -f
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/.."
    RESULT_VARIABLE PIPETUNE_VERSION_RESULT
    OUTPUT_VARIABLE PIPETUNE_BUILD_VERSION
    ERROR_VARIABLE PIPETUNE_VERSION_ERROR
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_STRIP_TRAILING_WHITESPACE)
  if(NOT PIPETUNE_VERSION_RESULT EQUAL 0)
    message(
      FATAL_ERROR
      "screw-up could not resolve the PipeTune version: "
      "${PIPETUNE_VERSION_ERROR}")
  endif()
endif()
if(NOT PIPETUNE_BUILD_VERSION MATCHES "^[0-9][-0-9A-Za-z.+:~]*$")
  message(FATAL_ERROR "PIPETUNE_BUILD_VERSION is not a valid package version")
endif()

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
set(
  PIPETUNE_EFFETUNE_PACKAGE_JSON
  "${PIPETUNE_EFFETUNE_DIR}/package.json")
if(NOT EXISTS "${PIPETUNE_EFFETUNE_PACKAGE_JSON}")
  message(
    FATAL_ERROR
    "EffeTune package metadata is unavailable: "
    "${PIPETUNE_EFFETUNE_PACKAGE_JSON}")
endif()
file(
  READ "${PIPETUNE_EFFETUNE_PACKAGE_JSON}"
  PIPETUNE_EFFETUNE_PACKAGE_CONTENT)
string(
  JSON PIPETUNE_EFFETUNE_VERSION
  ERROR_VARIABLE PIPETUNE_EFFETUNE_VERSION_ERROR
  GET "${PIPETUNE_EFFETUNE_PACKAGE_CONTENT}" version)
if(NOT PIPETUNE_EFFETUNE_VERSION_ERROR STREQUAL "NOTFOUND")
  message(
    FATAL_ERROR
    "EffeTune package version could not be resolved: "
    "${PIPETUNE_EFFETUNE_VERSION_ERROR}")
endif()
if(NOT PIPETUNE_EFFETUNE_VERSION MATCHES
   "^[0-9]+\\.[0-9]+\\.[0-9]+(-[0-9A-Za-z.-]+)?([+][0-9A-Za-z.-]+)?$")
  message(
    FATAL_ERROR
    "EffeTune package version is not a semantic version: "
    "${PIPETUNE_EFFETUNE_VERSION}")
endif()
set_property(
  DIRECTORY APPEND PROPERTY
  CMAKE_CONFIGURE_DEPENDS "${PIPETUNE_EFFETUNE_PACKAGE_JSON}")
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
find_program(PIPETUNE_SYSTEMCTL_EXECUTABLE systemctl REQUIRED)
file(
  GLOB_RECURSE PIPETUNE_DSP_PARAM_SPECS
  CONFIGURE_DEPENDS
  "${PIPETUNE_EFFETUNE_DIR}/dsp/plugins/*/params.json")
set(PIPETUNE_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
configure_file(
  src/version.cpp.in
  "${PIPETUNE_GENERATED_DIR}/version.cpp"
  @ONLY)
set(
  PIPETUNE_INSTALLED_GTK_EXECUTABLE
  "${CMAKE_INSTALL_FULL_BINDIR}/pipetune-gtk")
configure_file(
  src/installed_tools.h.in
  "${PIPETUNE_GENERATED_DIR}/installed_tools.h"
  @ONLY)
file(
  RELATIVE_PATH
  PIPETUNE_INSTALLED_DSP_BACKEND_RELATIVE_PATH
  "${CMAKE_INSTALL_FULL_BINDIR}"
  "${CMAKE_INSTALL_FULL_LIBDIR}/pipetune")
configure_file(
  src/dsp_backend_paths.h.in
  "${PIPETUNE_GENERATED_DIR}/dsp_backend_paths.h"
  @ONLY)
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
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/EffeTuneNativeBackends.cmake")
pipetune_add_effetune_native_backends(
  "${PIPETUNE_EFFETUNE_DIR}/dsp"
  "${CMAKE_BINARY_DIR}"
  PIPETUNE_DSP_BACKEND_TARGETS)

add_library(yyjson STATIC "${PIPETUNE_YYJSON_DIR}/src/yyjson.c")
target_include_directories(yyjson PUBLIC "${PIPETUNE_YYJSON_DIR}/src")
set_target_properties(yyjson PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(
  pipetune_core STATIC
  "${PIPETUNE_GENERATED_DIR}/version.cpp"
  src/audio_bridge.cpp
  src/dsp_backend.cpp
  src/dsp_catalog.cpp
  src/dsp_pipeline.cpp
  src/sample_rate.cpp
  src/startup_config.cpp
  src/startup_pipeline.cpp
  "${PIPETUNE_GENERATED_DSP_CATALOG}")
target_include_directories(
  pipetune_core
  PUBLIC include
  PRIVATE
    src
    "${PIPETUNE_GENERATED_DIR}"
    "${PIPETUNE_EFFETUNE_DIR}/dsp/include")
target_link_libraries(
  pipetune_core
  PUBLIC yyjson
  PRIVATE "${CMAKE_DL_LIBS}")

if(MSVC)
  target_compile_options(pipetune_core PRIVATE /W4 /WX)
else()
  target_compile_options(pipetune_core PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

add_library(
  pipetune_control STATIC
  src/control_protocol.cpp
  src/control_socket.cpp)
target_include_directories(pipetune_control PUBLIC include)
target_link_libraries(
  pipetune_control PUBLIC pipetune_core yyjson PRIVATE Threads::Threads)
if(MSVC)
  target_compile_options(pipetune_control PRIVATE /W4 /WX)
else()
  target_compile_options(
    pipetune_control PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

add_library(
  pipetune_pipewire STATIC
  src/autostart_override.cpp
  src/bypass_command.cpp
  src/command_line.cpp
  src/config_reset_command.cpp
  src/dsp_backend_command.cpp
  src/dsp_backend_runtime.cpp
  src/dsp_pipeline_slot.cpp
  src/filter_graph_properties.cpp
  src/input_telemetry.cpp
  src/pipewire_buffer_io.cpp
  src/pipewire_pipeline.cpp
  src/process_runner.cpp
  src/rate_command.cpp
  src/user_setup.cpp)
target_include_directories(pipetune_pipewire PUBLIC include PRIVATE src)
target_link_libraries(
  pipetune_pipewire
  PUBLIC pipetune_core pipetune_control
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
target_include_directories(pipetune PRIVATE src "${PIPETUNE_GENERATED_DIR}")
target_link_libraries(pipetune PRIVATE pipetune_pipewire)
if(MSVC)
  target_compile_options(pipetune PRIVATE /W4 /WX)
else()
  target_compile_options(pipetune PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

add_executable(
  pipetune-dsp-benchmark
  tools/dsp-backend-benchmark.cpp)
set_target_properties(
  pipetune-dsp-benchmark
  PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
target_link_libraries(pipetune-dsp-benchmark PRIVATE pipetune_core)
add_dependencies(
  pipetune-dsp-benchmark
  ${PIPETUNE_DSP_BACKEND_TARGETS})
if(MSVC)
  target_compile_options(pipetune-dsp-benchmark PRIVATE /W4 /WX)
else()
  target_compile_options(
    pipetune-dsp-benchmark PRIVATE
    -Wall -Wextra -Wpedantic -Werror)
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
  TARGETS ${PIPETUNE_DSP_BACKEND_TARGETS}
  LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}/pipetune")
install(
  FILES "${CMAKE_CURRENT_BINARY_DIR}/pipetune.service"
  DESTINATION "${PIPETUNE_SYSTEMD_USER_UNIT_DIR}")
install(
  FILES packaging/pipetune.environment.example
  DESTINATION "${PIPETUNE_DOCUMENTATION_DIR}"
  RENAME environment.example)
install(
  FILES docs/dsp-backends.md
  DESTINATION "${PIPETUNE_DOCUMENTATION_DIR}")

if(BUILD_TESTING)
  set(PIPETUNE_DSP_BACKEND_COPY_COMMANDS)
  foreach(PIPETUNE_DSP_BACKEND_TARGET IN LISTS PIPETUNE_DSP_BACKEND_TARGETS)
    list(
      APPEND
      PIPETUNE_DSP_BACKEND_COPY_COMMANDS
      COMMAND
        "${CMAKE_COMMAND}" -E copy_if_different
        "$<TARGET_FILE:${PIPETUNE_DSP_BACKEND_TARGET}>"
        "${CMAKE_CURRENT_BINARY_DIR}/$<TARGET_FILE_NAME:${PIPETUNE_DSP_BACKEND_TARGET}>"
      COMMAND
        "${CMAKE_COMMAND}" -E copy_if_different
        "$<TARGET_FILE:${PIPETUNE_DSP_BACKEND_TARGET}>"
        "${CMAKE_BINARY_DIR}/pipetune-gtk-build/$<TARGET_FILE_NAME:${PIPETUNE_DSP_BACKEND_TARGET}>")
  endforeach()
  add_custom_target(
    pipetune_test_dsp_backend_copies ALL
    COMMAND
      "${CMAKE_COMMAND}" -E make_directory
      "${CMAKE_CURRENT_BINARY_DIR}"
      "${CMAKE_BINARY_DIR}/pipetune-gtk-build"
    ${PIPETUNE_DSP_BACKEND_COPY_COMMANDS}
    DEPENDS
      ${PIPETUNE_DSP_BACKEND_TARGETS}
    VERBATIM)
  add_dependencies(
    pipetune-dsp-benchmark
    pipetune_test_dsp_backend_copies)

  add_library(
    pipetune_dsp_backend_wrong_abi_fixture SHARED
    test/dsp_backend_fixture.c)
  target_include_directories(
    pipetune_dsp_backend_wrong_abi_fixture
    PRIVATE
      src
      "${PIPETUNE_EFFETUNE_DIR}/dsp/include")
  target_compile_definitions(
    pipetune_dsp_backend_wrong_abi_fixture
    PRIVATE PIPETUNE_FIXTURE_WRONG_ABI=1)

  add_library(
    pipetune_dsp_backend_missing_symbol_fixture SHARED
    test/dsp_backend_fixture.c)
  target_include_directories(
    pipetune_dsp_backend_missing_symbol_fixture
    PRIVATE
      src
      "${PIPETUNE_EFFETUNE_DIR}/dsp/include")

  add_executable(
    pipetune_dsp_backend_tests
    test/dsp_backend_test.cpp)
  target_include_directories(
    pipetune_dsp_backend_tests
    PRIVATE
      src
      "${PIPETUNE_EFFETUNE_DIR}/dsp/include")
  target_link_libraries(
    pipetune_dsp_backend_tests PRIVATE pipetune_core)
  add_dependencies(
    pipetune_dsp_backend_tests
    ${PIPETUNE_DSP_BACKEND_TARGETS})
  if(MSVC)
    target_compile_options(pipetune_dsp_backend_tests PRIVATE /W4 /WX)
  else()
    target_compile_options(
      pipetune_dsp_backend_tests
      PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
  add_test(
    NAME pipetune_dsp_backend_tests
    COMMAND
      pipetune_dsp_backend_tests
      "$<TARGET_FILE:pipetune_effetune_dsp_scalar_shared>"
      "$<TARGET_FILE:pipetune_effetune_dsp_simd_shared>"
      "$<TARGET_FILE:pipetune_dsp_backend_wrong_abi_fixture>"
      "$<TARGET_FILE:pipetune_dsp_backend_missing_symbol_fixture>")

  add_test(
    NAME pipetune_native_dsp_architecture_tests
    COMMAND
      "${CMAKE_COMMAND}"
      -P
      "${CMAKE_CURRENT_SOURCE_DIR}/test/native_dsp_architecture_test.cmake")

  find_program(PIPETUNE_NM_EXECUTABLE nm REQUIRED)
  find_program(PIPETUNE_READELF_EXECUTABLE readelf REQUIRED)
  add_test(
    NAME pipetune_dsp_linkage
    COMMAND
      "${PIPETUNE_NODE_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/test/dsp-linkage.mjs"
      "$<TARGET_FILE:pipetune>"
      "${PIPETUNE_NM_EXECUTABLE}"
      "${PIPETUNE_READELF_EXECUTABLE}")

  add_executable(
    pipetune_effetune_backend_artifact_tests
    test/effetune_backend_artifact_test.cpp)
  target_include_directories(
    pipetune_effetune_backend_artifact_tests
    PRIVATE
      src
      "${PIPETUNE_EFFETUNE_DIR}/dsp/include")
  target_link_libraries(
    pipetune_effetune_backend_artifact_tests PRIVATE "${CMAKE_DL_LIBS}")
  add_dependencies(
    pipetune_effetune_backend_artifact_tests
    ${PIPETUNE_DSP_BACKEND_TARGETS})
  if(MSVC)
    target_compile_options(
      pipetune_effetune_backend_artifact_tests PRIVATE /W4 /WX)
  else()
    target_compile_options(
      pipetune_effetune_backend_artifact_tests
      PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
  set(PIPETUNE_DSP_BACKEND_ARTIFACT_TEST_ARGS)
  foreach(PIPETUNE_DSP_BACKEND_TARGET IN LISTS PIPETUNE_DSP_BACKEND_TARGETS)
    list(
      APPEND
      PIPETUNE_DSP_BACKEND_ARTIFACT_TEST_ARGS
      "$<TARGET_FILE:${PIPETUNE_DSP_BACKEND_TARGET}>")
  endforeach()
  add_test(
    NAME pipetune_effetune_backend_artifact_tests
    COMMAND
      pipetune_effetune_backend_artifact_tests
      ${PIPETUNE_DSP_BACKEND_ARTIFACT_TEST_ARGS})

  add_test(NAME pipetune_cli_help COMMAND pipetune --help)
  set_tests_properties(
    pipetune_cli_help PROPERTIES
    PASS_REGULAR_EXPRESSION "\\.effetune_preset")

  add_executable(pipetune_bootstrap_tests test/bootstrap_test.cpp)
  target_link_libraries(
    pipetune_bootstrap_tests
    PRIVATE pipetune_core pipetune_effetune_pffft_scalar)
  if(MSVC)
    target_compile_options(pipetune_bootstrap_tests PRIVATE /W4 /WX)
  else()
    target_compile_options(
      pipetune_bootstrap_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
  add_test(NAME pipetune_bootstrap_tests COMMAND pipetune_bootstrap_tests)

  add_executable(
    pipetune_startup_config_tests
    test/startup_config_test.cpp)
  target_link_libraries(pipetune_startup_config_tests PRIVATE pipetune_core)
  if(MSVC)
    target_compile_options(pipetune_startup_config_tests PRIVATE /W4 /WX)
  else()
    target_compile_options(
      pipetune_startup_config_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
  add_test(
    NAME pipetune_startup_config_tests
    COMMAND pipetune_startup_config_tests)

  add_executable(
    pipetune_sample_rate_tests
    test/sample_rate_test.cpp)
  target_link_libraries(pipetune_sample_rate_tests PRIVATE pipetune_core)
  if(MSVC)
    target_compile_options(pipetune_sample_rate_tests PRIVATE /W4 /WX)
  else()
    target_compile_options(
      pipetune_sample_rate_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
  add_test(
    NAME pipetune_sample_rate_tests
    COMMAND pipetune_sample_rate_tests)

  add_executable(
    pipetune_startup_pipeline_tests
    test/startup_pipeline_test.cpp)
  target_include_directories(pipetune_startup_pipeline_tests PRIVATE src)
  target_link_libraries(pipetune_startup_pipeline_tests PRIVATE pipetune_core)
  if(MSVC)
    target_compile_options(pipetune_startup_pipeline_tests PRIVATE /W4 /WX)
  else()
    target_compile_options(
      pipetune_startup_pipeline_tests PRIVATE
      -Wall -Wextra -Wpedantic -Werror)
  endif()
  add_test(
    NAME pipetune_startup_pipeline_tests
    COMMAND pipetune_startup_pipeline_tests)

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
    pipetune_filter_graph_properties_tests
    test/filter_graph_properties_test.cpp)
  target_include_directories(
    pipetune_filter_graph_properties_tests PRIVATE src)
  target_link_libraries(
    pipetune_filter_graph_properties_tests PRIVATE pipetune_pipewire)
  if(MSVC)
    target_compile_options(
      pipetune_filter_graph_properties_tests PRIVATE /W4 /WX)
  else()
    target_compile_options(
      pipetune_filter_graph_properties_tests
      PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
  add_test(
    NAME pipetune_filter_graph_properties_tests
    COMMAND pipetune_filter_graph_properties_tests)

  add_executable(
    pipetune_legacy_output_surface_removal_tests
    test/legacy_output_surface_removal_test.cpp)
  target_link_libraries(
    pipetune_legacy_output_surface_removal_tests
    PRIVATE pipetune_control pipetune_core)
  if(MSVC)
    target_compile_options(
      pipetune_legacy_output_surface_removal_tests PRIVATE /W4 /WX)
  else()
    target_compile_options(
      pipetune_legacy_output_surface_removal_tests
      PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
  add_test(
    NAME pipetune_legacy_output_surface_removal_tests
    COMMAND
      pipetune_legacy_output_surface_removal_tests
      "${CMAKE_CURRENT_BINARY_DIR}/pipetune.test.service")

  add_executable(
    pipetune_input_telemetry_tests
    test/input_telemetry_test.cpp)
  target_include_directories(pipetune_input_telemetry_tests PRIVATE src)
  target_link_libraries(
    pipetune_input_telemetry_tests PRIVATE pipetune_pipewire)
  if(MSVC)
    target_compile_options(pipetune_input_telemetry_tests PRIVATE /W4 /WX)
  else()
    target_compile_options(
      pipetune_input_telemetry_tests PRIVATE
      -Wall -Wextra -Wpedantic -Werror)
  endif()
  add_test(
    NAME pipetune_input_telemetry_tests
    COMMAND pipetune_input_telemetry_tests)

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
    pipetune_config_reset_command_tests
    test/config_reset_command_test.cpp)
  target_include_directories(
    pipetune_config_reset_command_tests PRIVATE src)
  target_link_libraries(
    pipetune_config_reset_command_tests PRIVATE pipetune_pipewire)
  if(MSVC)
    target_compile_options(
      pipetune_config_reset_command_tests PRIVATE /W4 /WX)
  else()
    target_compile_options(
      pipetune_config_reset_command_tests PRIVATE
      -Wall -Wextra -Wpedantic -Werror)
  endif()
  add_test(
    NAME pipetune_config_reset_command_tests
    COMMAND pipetune_config_reset_command_tests)

  add_executable(
    pipetune_bypass_command_tests
    test/bypass_command_test.cpp)
  target_include_directories(pipetune_bypass_command_tests PRIVATE src)
  target_link_libraries(
    pipetune_bypass_command_tests
    PRIVATE pipetune_pipewire Threads::Threads)
  if(MSVC)
    target_compile_options(pipetune_bypass_command_tests PRIVATE /W4 /WX)
  else()
    target_compile_options(
      pipetune_bypass_command_tests PRIVATE
      -Wall -Wextra -Wpedantic -Werror)
  endif()
  add_test(
    NAME pipetune_bypass_command_tests
    COMMAND pipetune_bypass_command_tests)

  add_executable(
    pipetune_process_runner_tests
    test/process_runner_test.cpp)
  target_include_directories(pipetune_process_runner_tests PRIVATE src)
  target_link_libraries(pipetune_process_runner_tests PRIVATE pipetune_pipewire)
  if(MSVC)
    target_compile_options(pipetune_process_runner_tests PRIVATE /W4 /WX)
  else()
    target_compile_options(
      pipetune_process_runner_tests PRIVATE
      -Wall -Wextra -Wpedantic -Werror)
  endif()
  add_test(
    NAME pipetune_process_runner_tests
    COMMAND pipetune_process_runner_tests)

  add_executable(
    pipetune_autostart_override_tests
    test/autostart_override_test.cpp)
  target_include_directories(pipetune_autostart_override_tests PRIVATE src)
  target_link_libraries(
    pipetune_autostart_override_tests PRIVATE pipetune_pipewire)
  if(MSVC)
    target_compile_options(pipetune_autostart_override_tests PRIVATE /W4 /WX)
  else()
    target_compile_options(
      pipetune_autostart_override_tests PRIVATE
      -Wall -Wextra -Wpedantic -Werror)
  endif()
  add_test(
    NAME pipetune_autostart_override_tests
    COMMAND pipetune_autostart_override_tests)

  add_executable(
    pipetune_user_setup_tests
    test/user_setup_test.cpp)
  target_include_directories(pipetune_user_setup_tests PRIVATE src)
  target_link_libraries(
    pipetune_user_setup_tests PRIVATE pipetune_pipewire)
  if(MSVC)
    target_compile_options(pipetune_user_setup_tests PRIVATE /W4 /WX)
  else()
    target_compile_options(
      pipetune_user_setup_tests PRIVATE
      -Wall -Wextra -Wpedantic -Werror)
  endif()
  add_test(
    NAME pipetune_user_setup_tests
    COMMAND pipetune_user_setup_tests)

  add_executable(
    pipetune_rate_command_tests
    test/rate_command_test.cpp)
  target_include_directories(pipetune_rate_command_tests PRIVATE src)
  target_link_libraries(
    pipetune_rate_command_tests PRIVATE pipetune_pipewire)
  if(MSVC)
    target_compile_options(pipetune_rate_command_tests PRIVATE /W4 /WX)
  else()
    target_compile_options(
      pipetune_rate_command_tests PRIVATE
      -Wall -Wextra -Wpedantic -Werror)
  endif()
  add_test(
    NAME pipetune_rate_command_tests
    COMMAND pipetune_rate_command_tests)

  add_executable(
    pipetune_dsp_backend_command_tests
    test/dsp_backend_command_test.cpp)
  target_include_directories(
    pipetune_dsp_backend_command_tests PRIVATE src)
  target_link_libraries(
    pipetune_dsp_backend_command_tests PRIVATE pipetune_pipewire)
  add_dependencies(
    pipetune_dsp_backend_command_tests
    pipetune_test_dsp_backend_copies)
  if(MSVC)
    target_compile_options(
      pipetune_dsp_backend_command_tests PRIVATE /W4 /WX)
  else()
    target_compile_options(
      pipetune_dsp_backend_command_tests PRIVATE
      -Wall -Wextra -Wpedantic -Werror)
  endif()
  add_test(
    NAME pipetune_dsp_backend_command_tests
    COMMAND pipetune_dsp_backend_command_tests)

  add_executable(
    pipetune_pipewire_buffer_io_tests
    test/pipewire_buffer_io_test.cpp)
  target_include_directories(
    pipetune_pipewire_buffer_io_tests PRIVATE src)
  target_link_libraries(
    pipetune_pipewire_buffer_io_tests
    PRIVATE pipetune_pipewire PkgConfig::PIPEWIRE)
  if(MSVC)
    target_compile_options(
      pipetune_pipewire_buffer_io_tests PRIVATE /W4 /WX)
  else()
    target_compile_options(
      pipetune_pipewire_buffer_io_tests PRIVATE
      -Wall -Wextra -Wpedantic -Werror)
  endif()
  add_test(
    NAME pipetune_pipewire_buffer_io_tests
    COMMAND pipetune_pipewire_buffer_io_tests)

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
    pipetune_dsp_backend_runtime_tests
    test/dsp_backend_runtime_test.cpp)
  target_include_directories(
    pipetune_dsp_backend_runtime_tests PRIVATE src)
  target_link_libraries(
    pipetune_dsp_backend_runtime_tests PRIVATE pipetune_pipewire)
  add_dependencies(
    pipetune_dsp_backend_runtime_tests
    pipetune_test_dsp_backend_copies)
  if(MSVC)
    target_compile_options(
      pipetune_dsp_backend_runtime_tests PRIVATE /W4 /WX)
  else()
    target_compile_options(
      pipetune_dsp_backend_runtime_tests PRIVATE
      -Wall -Wextra -Wpedantic -Werror)
  endif()
  add_test(
    NAME pipetune_dsp_backend_runtime_tests
    COMMAND pipetune_dsp_backend_runtime_tests)

  add_executable(
    pipetune_control_protocol_tests
    test/control_protocol_test.cpp)
  target_link_libraries(
    pipetune_control_protocol_tests PRIVATE pipetune_control)
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
  target_link_libraries(
    pipetune_control_socket_tests PRIVATE pipetune_control Threads::Threads)
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

  add_test(
    NAME pipetune_dsp_backend_benchmark
    COMMAND
      ${PIPETUNE_NODE_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/test/dsp-backend-benchmark.mjs
      $<TARGET_FILE:pipetune-dsp-benchmark>
      ${PIPETUNE_EFFETUNE_DIR}/presets/processor/bbe.effetune_preset)

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
      ${CMAKE_INSTALL_LIBDIR}
      ${PIPETUNE_DOCUMENTATION_DIR}
      ${CMAKE_CURRENT_BINARY_DIR}/pipetune.test.service)

  add_test(
    NAME pipetune_component_build
    COMMAND
      ${PIPETUNE_NODE_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/test/component-build.mjs
      ${CMAKE_COMMAND}
      ${CMAKE_CURRENT_SOURCE_DIR}
      $<TARGET_FILE:pipetune>)
  set_tests_properties(
    pipetune_component_build PROPERTIES TIMEOUT 180)
endif()
