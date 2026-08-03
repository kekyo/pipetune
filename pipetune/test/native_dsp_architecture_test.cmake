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
