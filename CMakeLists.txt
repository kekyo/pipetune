cmake_minimum_required(VERSION 3.24)

project(PipeTune VERSION 0.1.0 LANGUAGES C CXX)

include(CTest)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_program(PIPETUNE_NODE_EXECUTABLE node REQUIRED)
file(
  GLOB_RECURSE PIPETUNE_DSP_PARAM_SPECS
  CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/deps/effetune/dsp/plugins/*/params.json")
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
    tools/generate-dsp-catalog.mjs
    deps/effetune/scripts/gen-dsp-params.mjs
    deps/effetune/plugins/plugins.txt
    deps/effetune/dsp/registry.inc
    ${PIPETUNE_DSP_PARAM_SPECS}
  VERBATIM)

set(ET_SIMD OFF CACHE BOOL "Match EffeTune's native scalar DSP configuration" FORCE)
add_subdirectory(deps/effetune/dsp effetune-dsp)

add_library(yyjson STATIC deps/yyjson/src/yyjson.c)
target_include_directories(yyjson PUBLIC deps/yyjson/src)
set_target_properties(yyjson PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(
  pipetune_core STATIC
  src/version.cpp
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

add_executable(pipetune src/main.cpp)
target_link_libraries(pipetune PRIVATE pipetune_core)

install(TARGETS pipetune RUNTIME DESTINATION bin)

if(BUILD_TESTING)
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
      ${CMAKE_CURRENT_SOURCE_DIR}/deps/effetune)

  add_test(
    NAME pipetune_effetune_native_parity
    COMMAND
      ${PIPETUNE_NODE_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/deps/effetune/tools/dsp-parity/run.mjs
      --native
      --root ${CMAKE_CURRENT_SOURCE_DIR}/deps/effetune
      --native-runner
      ${CMAKE_CURRENT_BINARY_DIR}/effetune-dsp/effetune-dsp-parity-runner)
endif()
