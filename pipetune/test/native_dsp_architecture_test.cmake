include(
  "${CMAKE_CURRENT_LIST_DIR}/../cmake/EffeTuneNativeBackends.cmake")

function(assert_native_processor SYSTEM_PROCESSOR POINTER_SIZE EXPECTED)
  pipetune_resolve_native_processor(
    actual_processor
    "${SYSTEM_PROCESSOR}"
    "${POINTER_SIZE}")
  if(NOT actual_processor STREQUAL EXPECTED)
    message(
      FATAL_ERROR
      "Expected ${SYSTEM_PROCESSOR}/${POINTER_SIZE} to resolve to ${EXPECTED}, "
      "but got ${actual_processor}")
  endif()
endfunction()

function(assert_target_scopes_tube_psabi_option BUILD_DIRECTORY TARGET_PATH)
  set(flags_path "${BUILD_DIRECTORY}/${TARGET_PATH}/flags.make")
  if(NOT EXISTS "${flags_path}")
    message(FATAL_ERROR "Missing generated target flags: ${flags_path}")
  endif()
  file(READ "${flags_path}" flags)
  string(
    REGEX MATCH
    "tube_simulator/kernel\\.cpp\\.o_OPTIONS = [^\n]*-Wno-psabi"
    tube_match
    "${flags}")
  if(NOT tube_match)
    message(
      FATAL_ERROR
      "Expected ${TARGET_PATH} to suppress only the Tube source's psABI warning")
  endif()
  string(
    REGEX MATCH
    "(^|\n)CXX_FLAGS = [^\n]*-Wno-(error=)?psabi"
    target_match
    "${flags}")
  if(target_match)
    message(
      FATAL_ERROR
      "Expected ${TARGET_PATH} to retain the C++ warning policy")
  endif()
endfunction()

function(assert_target_uses_engine_mxcsr_option BUILD_DIRECTORY TARGET_PATH)
  set(flags_path "${BUILD_DIRECTORY}/${TARGET_PATH}/flags.make")
  if(NOT EXISTS "${flags_path}")
    message(FATAL_ERROR "Missing generated target flags: ${flags_path}")
  endif()
  file(READ "${flags_path}" flags)
  string(
    REGEX MATCH
    "engine\\.cpp\\.o_OPTIONS = [^\n]*-msse( |\n|$)"
    engine_match
    "${flags}")
  if(NOT engine_match)
    message(
      FATAL_ERROR
      "Expected ${TARGET_PATH} to enable SSE for EffeTune's MXCSR access")
  endif()
  string(
    REGEX MATCH
    "CXX_FLAGS = [^\n]*-msse( |\n|$)"
    target_match
    "${flags}")
  if(target_match)
    message(
      FATAL_ERROR
      "Expected ${TARGET_PATH} to keep SSE scoped to EffeTune's engine source")
  endif()
endfunction()

assert_native_processor("x86_64" "8" "x86_64")
assert_native_processor("AMD64" "8" "amd64")
assert_native_processor("x86_64" "4" "i686")
assert_native_processor("AMD64" "4" "i686")
assert_native_processor("i686" "4" "i686")
assert_native_processor("aarch64" "8" "aarch64")
assert_native_processor("ARM64" "8" "arm64")
assert_native_processor("aarch64" "4" "armv7l")
assert_native_processor("ARM64" "4" "armv7l")
assert_native_processor("armv7l" "4" "armv7l")
assert_native_processor("riscv64" "8" "riscv64")

set(fixture_root "${CMAKE_CURRENT_BINARY_DIR}/native-dsp-i686-fixture")
set(fixture_build "${fixture_root}/build")
set(fixture_toolchain "${fixture_root}/i686-toolchain.cmake")
file(REMOVE_RECURSE "${fixture_root}")
file(MAKE_DIRECTORY "${fixture_root}")
file(
  WRITE
  "${fixture_toolchain}"
  "set(CMAKE_SYSTEM_NAME Linux)\nset(CMAKE_SYSTEM_PROCESSOR i686)\n")
execute_process(
  COMMAND
    "${CMAKE_COMMAND}"
    -S "${CMAKE_CURRENT_LIST_DIR}/.."
    -B "${fixture_build}"
    -G "Unix Makefiles"
    "-DCMAKE_TOOLCHAIN_FILE=${fixture_toolchain}"
    -DCMAKE_BUILD_TYPE=Release
    -DBUILD_TESTING=ON
    -DPIPETUNE_BUILD_VERSION=0.0.0
  RESULT_VARIABLE fixture_result
  OUTPUT_VARIABLE fixture_output
  ERROR_VARIABLE fixture_error)
if(NOT fixture_result EQUAL 0)
  message(
    FATAL_ERROR
    "Could not configure the i686 DSP fixture:\n${fixture_output}\n${fixture_error}")
endif()

assert_target_scopes_tube_psabi_option(
  "${fixture_build}"
  "effetune-dsp/CMakeFiles/effetune_dsp_core.dir")
assert_target_uses_engine_mxcsr_option(
  "${fixture_build}"
  "effetune-dsp/CMakeFiles/effetune_dsp_core.dir")
foreach(
  target_name
  IN ITEMS
    pipetune_effetune_dsp_scalar_shared
    pipetune_effetune_dsp_simd_shared
    pipetune_effetune_dsp_x86_64_v3_shared)
  assert_target_scopes_tube_psabi_option(
    "${fixture_build}"
    "CMakeFiles/${target_name}.dir")
  assert_target_uses_engine_mxcsr_option(
    "${fixture_build}"
    "CMakeFiles/${target_name}.dir")
endforeach()

file(REMOVE_RECURSE "${fixture_root}")
