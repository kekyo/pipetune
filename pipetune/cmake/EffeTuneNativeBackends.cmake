include_guard(GLOBAL)

# Resolves the target processor while accounting for a compiler ABI that is
# narrower than the host kernel reported by CMAKE_SYSTEM_PROCESSOR.
function(
  pipetune_resolve_native_processor
  OUTPUT_VARIABLE
  SYSTEM_PROCESSOR
  POINTER_SIZE)
  string(TOLOWER "${SYSTEM_PROCESSOR}" resolved_processor)

  if("${POINTER_SIZE}" STREQUAL "4")
    if(resolved_processor MATCHES "^(x86_64|amd64)$")
      set(resolved_processor "i686")
    elseif(resolved_processor MATCHES "^(aarch64|arm64)$")
      set(resolved_processor "armv7l")
    endif()
  endif()

  set(${OUTPUT_VARIABLE} "${resolved_processor}" PARENT_SCOPE)
endfunction()

# EffeTune's Tube Simulator keeps its GNU f64x2 helpers within one translation
# unit. On 32-bit targets GCC reports their file-local calling convention as a
# psABI warning even though no vector type crosses a library ABI boundary.
function(
  pipetune_resolve_effetune_portability_options
  OUTPUT_VARIABLE
  NATIVE_PROCESSOR)
  set(portability_options)
  if(NATIVE_PROCESSOR MATCHES "^(i[3-6]86|x86|arm|armv[5-8].*)$")
    list(
      APPEND portability_options
      "$<$<COMPILE_LANG_AND_ID:CXX,GNU>:-Wno-error=psabi>")
  endif()
  set(${OUTPUT_VARIABLE} "${portability_options}" PARENT_SCOPE)
endfunction()

function(
  pipetune_configure_effetune_portability_target
  TARGET_NAME
  NATIVE_PROCESSOR)
  pipetune_resolve_effetune_portability_options(
    portability_options
    "${NATIVE_PROCESSOR}")
  if(portability_options)
    target_compile_options(${TARGET_NAME} PRIVATE ${portability_options})
  endif()
endfunction()

# EffeTune accesses MXCSR directly on native x86. GCC's i686 baseline does not
# enable those SSE instructions, so scope the required option to engine.cpp and
# leave the scalar backend's floating-point and vectorization policy unchanged.
function(
  pipetune_configure_effetune_mxcsr_source
  SOURCE_PATH
  NATIVE_PROCESSOR)
  if(NATIVE_PROCESSOR MATCHES "^(i[3-6]86|x86)$")
    set_property(
      SOURCE "${SOURCE_PATH}"
      ${ARGN}
      APPEND
      PROPERTY COMPILE_OPTIONS
               "$<$<COMPILE_LANG_AND_ID:CXX,GNU>:-msse>")
  endif()
endfunction()

function(
  pipetune_configure_native_variant_target
  TARGET_NAME
  VARIANT_NAME
  NATIVE_PROCESSOR)
  target_compile_options(
    ${TARGET_NAME}
    PRIVATE
      $<$<AND:$<CONFIG:Release>,$<COMPILE_LANG_AND_ID:C,GNU>>:-ftree-vectorize>
      $<$<AND:$<CONFIG:Release>,$<COMPILE_LANG_AND_ID:CXX,GNU>>:-ftree-vectorize>)

  if(NATIVE_PROCESSOR MATCHES "^(x86_64|amd64)$")
    # The x86-64 architectural baseline already includes SSE2.
  elseif(NATIVE_PROCESSOR MATCHES "^(i[3-6]86|x86)$")
    target_compile_options(${TARGET_NAME} PRIVATE -msse2 -mfpmath=sse)
  elseif(NATIVE_PROCESSOR MATCHES "^(aarch64|arm64)$")
    target_compile_definitions(${TARGET_NAME} PRIVATE PFFFT_ENABLE_NEON=1)
  elseif(NATIVE_PROCESSOR MATCHES "^(arm|armv[5-8].*)$")
    target_compile_definitions(${TARGET_NAME} PRIVATE PFFFT_ENABLE_NEON=1)
    target_compile_options(${TARGET_NAME} PRIVATE -mfpu=neon)
  elseif(NATIVE_PROCESSOR MATCHES "^riscv64")
    target_compile_options(
      ${TARGET_NAME} PRIVATE -march=rv64gcv -mabi=lp64d)
  else()
    message(
      FATAL_ERROR
      "Unsupported native SIMD backend architecture: ${NATIVE_PROCESSOR}")
  endif()

  if(VARIANT_NAME STREQUAL "X86_64_V3")
    if(NOT NATIVE_PROCESSOR MATCHES "^(x86_64|amd64|i[3-6]86|x86)$")
      message(FATAL_ERROR "x86-64-v3 backend requested for a non-x86 target")
    endif()
    target_compile_options(${TARGET_NAME} PRIVATE -march=x86-64-v3)
  elseif(VARIANT_NAME STREQUAL "X86_64_V4")
    if(NOT NATIVE_PROCESSOR MATCHES "^(x86_64|amd64)$")
      message(FATAL_ERROR "x86-64-v4 backend requested for a non-amd64 target")
    endif()
    target_compile_options(${TARGET_NAME} PRIVATE -march=x86-64-v4)
  elseif(VARIANT_NAME STREQUAL "ARM64_SVE")
    if(NOT NATIVE_PROCESSOR MATCHES "^(aarch64|arm64)$")
      message(FATAL_ERROR "Arm64 SVE backend requested for a non-arm64 target")
    endif()
    target_compile_options(
      ${TARGET_NAME} PRIVATE -march=armv8-a+sve -msve-vector-bits=scalable)
  elseif(NOT VARIANT_NAME STREQUAL "SIMD_BASELINE")
    message(FATAL_ERROR "Unknown native SIMD backend variant: ${VARIANT_NAME}")
  endif()
endfunction()

function(
  pipetune_add_native_pffft
  TARGET_NAME
  VARIANT_NAME
  NATIVE_PROCESSOR
  EFFETUNE_DSP_DIR)
  add_library(
    ${TARGET_NAME} STATIC
    "${EFFETUNE_DSP_DIR}/vendor/pffft/src/pffft.c"
    "${EFFETUNE_DSP_DIR}/vendor/pffft/src/pffft_common.c")
  target_include_directories(
    ${TARGET_NAME}
    PUBLIC "${EFFETUNE_DSP_DIR}/vendor/pffft/include/pffft"
    PRIVATE "${EFFETUNE_DSP_DIR}/vendor/pffft/src")
  target_compile_definitions(${TARGET_NAME} PUBLIC PFFFT_STATIC_DEFINE=1)
  if(VARIANT_NAME STREQUAL "SCALAR")
    target_compile_definitions(${TARGET_NAME} PRIVATE PFFFT_SIMD_DISABLE=1)
  else()
    pipetune_configure_native_variant_target(
      ${TARGET_NAME} ${VARIANT_NAME} ${NATIVE_PROCESSOR})
  endif()
  set_target_properties(
    ${TARGET_NAME}
    PROPERTIES
      C_STANDARD 11
      C_STANDARD_REQUIRED ON
      C_EXTENSIONS OFF
      C_VISIBILITY_PRESET hidden
      POSITION_INDEPENDENT_CODE ON)
  target_link_libraries(${TARGET_NAME} PUBLIC m)
endfunction()

function(
  pipetune_add_native_shared_backend
  TARGET_NAME
  OUTPUT_NAME
  PFFFT_TARGET
  VARIANT_NAME
  VARIANT_ID
  NATIVE_PROCESSOR
  EFFETUNE_DSP_DIR
  EFFETUNE_ABI_SOURCE
  OUTPUT_DIRECTORY)
  set(
    effetune_production_sources
    "${EFFETUNE_DSP_DIR}/core/allocation_guard.cpp"
    "${EFFETUNE_ABI_SOURCE}"
    "${EFFETUNE_DSP_DIR}/core/arena.cpp"
    "${EFFETUNE_DSP_DIR}/core/design_fft.cpp"
    "${EFFETUNE_DSP_DIR}/core/engine.cpp"
    "${EFFETUNE_DSP_DIR}/core/graph.cpp"
    "${EFFETUNE_DSP_DIR}/core/partitioned_convolver.cpp"
    "${EFFETUNE_DSP_DIR}/core/registry.cpp"
    "${EFFETUNE_DSP_DIR}/core/telemetry.cpp")
  file(
    GLOB_RECURSE effetune_plugin_sources
    CONFIGURE_DEPENDS
    "${EFFETUNE_DSP_DIR}/plugins/*/kernel.cpp")
  list(APPEND effetune_production_sources ${effetune_plugin_sources})
  list(
    APPEND effetune_production_sources
    "${EFFETUNE_DSP_DIR}/plugins/lofi/gsm_full_rate_simulator/codec.cpp")

  set(backend_abi_source
      "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../src/effetune_backend_abi.c")
  set(backend_asset_bridge_source
      "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../src/effetune_backend_asset_bridge.cpp")
  set(backend_export_map
      "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../src/effetune_backend_exports.map")
  add_library(
    ${TARGET_NAME} SHARED
    ${effetune_production_sources}
    "${backend_abi_source}"
    "${backend_asset_bridge_source}")
  add_dependencies(
    ${TARGET_NAME}
    pipetune_effetune_backend_abi_source)
  target_include_directories(
    ${TARGET_NAME}
    PUBLIC "${EFFETUNE_DSP_DIR}/include"
    PRIVATE
      "${EFFETUNE_DSP_DIR}/bindings/generated"
      "${EFFETUNE_DSP_DIR}/core"
      "${EFFETUNE_DSP_DIR}/generated/cpp"
      "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../src")
  target_link_libraries(${TARGET_NAME} PRIVATE ${PFFFT_TARGET})
  target_compile_definitions(
    ${TARGET_NAME}
    PRIVATE
      PIPETUNE_EFFETUNE_BACKEND_VARIANT=${VARIANT_ID}
      $<$<CONFIG:Debug>:ET_DEBUG_BUILD=1>)
  if(NOT VARIANT_NAME STREQUAL "SCALAR")
    target_compile_definitions(${TARGET_NAME} PRIVATE ET_SIMD=1)
  endif()
  target_compile_options(
    ${TARGET_NAME}
    PRIVATE
      -Wall
      -Wextra
      -Wpedantic
      -Werror
      $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
      $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>)
  pipetune_configure_effetune_portability_target(
    ${TARGET_NAME}
    "${NATIVE_PROCESSOR}")
  if(NOT VARIANT_NAME STREQUAL "SCALAR")
    pipetune_configure_native_variant_target(
      ${TARGET_NAME} ${VARIANT_NAME} ${NATIVE_PROCESSOR})
  endif()
  set_target_properties(
    ${TARGET_NAME}
    PROPERTIES
      C_STANDARD 11
      C_STANDARD_REQUIRED ON
      C_EXTENSIONS OFF
      CXX_STANDARD 20
      CXX_STANDARD_REQUIRED ON
      CXX_EXTENSIONS OFF
      C_VISIBILITY_PRESET hidden
      CXX_VISIBILITY_PRESET hidden
      VISIBILITY_INLINES_HIDDEN YES
      POSITION_INDEPENDENT_CODE ON
      OUTPUT_NAME "${OUTPUT_NAME}"
      LIBRARY_OUTPUT_DIRECTORY "${OUTPUT_DIRECTORY}")
  set_property(
    TARGET ${TARGET_NAME}
    APPEND
    PROPERTY LINK_DEPENDS "${backend_export_map}")
  target_link_options(
    ${TARGET_NAME}
    PRIVATE
      "LINKER:--no-undefined"
      "LINKER:--version-script=${backend_export_map}")
endfunction()

# Copies EffeTune's file-local engine registry ABI into the build tree and
# applies the smallest PipeTune integration hook as a normal unified diff.
function(
  pipetune_prepare_effetune_backend_abi
  EFFETUNE_DSP_DIR
  OUTPUT_VARIABLE)
  find_program(PIPETUNE_PATCH_EXECUTABLE patch REQUIRED)
  set(source_abi "${EFFETUNE_DSP_DIR}/core/abi.cpp")
  set(abi_patch
      "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../patches/effetune/abi-engine-access.patch")
  set(generated_directory
      "${CMAKE_CURRENT_BINARY_DIR}/generated/effetune-backend/core")
  set(generated_abi "${generated_directory}/abi.cpp")
  set(working_abi "${generated_directory}/abi.cpp.work")
  add_custom_command(
    OUTPUT "${generated_abi}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${generated_directory}"
    COMMAND "${CMAKE_COMMAND}" -E copy "${source_abi}" "${working_abi}"
    COMMAND
      "${PIPETUNE_PATCH_EXECUTABLE}"
      --batch
      --forward
      --fuzz=0
      --reject-file=-
      --input
      "${abi_patch}"
      "${working_abi}"
    COMMAND
      "${CMAKE_COMMAND}" -E copy_if_different
      "${working_abi}"
      "${generated_abi}"
    DEPENDS
      "${source_abi}"
      "${abi_patch}"
      "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../src/effetune_backend_engine_access.inc"
      "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../src/effetune_backend_engine_access.h"
    BYPRODUCTS "${working_abi}"
    COMMENT "Applying PipeTune's native EffeTune ABI bridge patch"
    VERBATIM)
  add_custom_target(
    pipetune_effetune_backend_abi_source
    DEPENDS "${generated_abi}")
  set_source_files_properties("${generated_abi}" PROPERTIES GENERATED TRUE)
  set(${OUTPUT_VARIABLE} "${generated_abi}" PARENT_SCOPE)
endfunction()

# Builds PipeTune-owned native shared artifacts from an unmodified EffeTune
# source tree and returns the architecture-applicable target list.
function(
  pipetune_add_effetune_native_backends
  EFFETUNE_DSP_DIR
  OUTPUT_DIRECTORY
  OUTPUT_VARIABLE)
  if(EMSCRIPTEN OR MSVC OR NOT UNIX)
    message(
      FATAL_ERROR
      "Native shared DSP backends require a non-Emscripten Unix GCC toolchain")
  endif()

  pipetune_resolve_native_processor(
    native_processor
    "${CMAKE_SYSTEM_PROCESSOR}"
    "${CMAKE_SIZEOF_VOID_P}")
  pipetune_configure_effetune_mxcsr_source(
    "${EFFETUNE_DSP_DIR}/core/engine.cpp"
    "${native_processor}")
  pipetune_prepare_effetune_backend_abi(
    "${EFFETUNE_DSP_DIR}"
    generated_abi_source)

  set(registry_source "${EFFETUNE_DSP_DIR}/core/registry.cpp")
  set_property(
    SOURCE "${registry_source}"
    APPEND
    PROPERTY OBJECT_DEPENDS "${EFFETUNE_DSP_DIR}/registry.inc")

  # EffeTune applies this source-specific policy in its own DSP directory.
  # Source properties are directory-scoped, so mirror it for the PipeTune-owned
  # shared backends to preserve the official golden outputs.
  set_property(
    SOURCE
      "${EFFETUNE_DSP_DIR}/plugins/dynamics/auto_leveler/kernel.cpp"
      "${EFFETUNE_DSP_DIR}/plugins/lofi/bluetooth_sbc_simulator/kernel.cpp"
      "${EFFETUNE_DSP_DIR}/plugins/lofi/cassette_artifacts/kernel.cpp"
      "${EFFETUNE_DSP_DIR}/plugins/lofi/tape_artifacts/kernel.cpp"
      "${EFFETUNE_DSP_DIR}/plugins/lofi/vinyl_artifacts/kernel.cpp"
    APPEND
    PROPERTY COMPILE_OPTIONS
             "$<$<COMPILE_LANG_AND_ID:CXX,AppleClang,Clang,GNU>:-ffp-contract=off>")

  pipetune_add_native_pffft(
    pipetune_effetune_pffft_scalar
    SCALAR
    "${native_processor}"
    "${EFFETUNE_DSP_DIR}")
  pipetune_add_native_pffft(
    pipetune_effetune_pffft_simd
    SIMD_BASELINE
    "${native_processor}"
    "${EFFETUNE_DSP_DIR}")
  pipetune_add_native_shared_backend(
    pipetune_effetune_dsp_scalar_shared
    "effetune-dsp-scalar"
    pipetune_effetune_pffft_scalar
    SCALAR
    PIPETUNE_EFFETUNE_BACKEND_VARIANT_SCALAR
    "${native_processor}"
    "${EFFETUNE_DSP_DIR}"
    "${generated_abi_source}"
    "${OUTPUT_DIRECTORY}")
  pipetune_add_native_shared_backend(
    pipetune_effetune_dsp_simd_shared
    "effetune-dsp-simd"
    pipetune_effetune_pffft_simd
    SIMD_BASELINE
    PIPETUNE_EFFETUNE_BACKEND_VARIANT_SIMD_BASELINE
    "${native_processor}"
    "${EFFETUNE_DSP_DIR}"
    "${generated_abi_source}"
    "${OUTPUT_DIRECTORY}")

  set(
    backend_targets
    pipetune_effetune_dsp_scalar_shared
    pipetune_effetune_dsp_simd_shared)

  if(native_processor MATCHES "^(x86_64|amd64|i[3-6]86|x86)$")
    pipetune_add_native_pffft(
      pipetune_effetune_pffft_x86_64_v3
      X86_64_V3
      "${native_processor}"
      "${EFFETUNE_DSP_DIR}")
    pipetune_add_native_shared_backend(
      pipetune_effetune_dsp_x86_64_v3_shared
      "effetune-dsp-simd-x86-64-v3"
      pipetune_effetune_pffft_x86_64_v3
      X86_64_V3
      PIPETUNE_EFFETUNE_BACKEND_VARIANT_X86_64_V3
      "${native_processor}"
      "${EFFETUNE_DSP_DIR}"
      "${generated_abi_source}"
      "${OUTPUT_DIRECTORY}")
    list(APPEND backend_targets pipetune_effetune_dsp_x86_64_v3_shared)
  endif()

  if(native_processor MATCHES "^(x86_64|amd64)$")
    pipetune_add_native_pffft(
      pipetune_effetune_pffft_x86_64_v4
      X86_64_V4
      "${native_processor}"
      "${EFFETUNE_DSP_DIR}")
    pipetune_add_native_shared_backend(
      pipetune_effetune_dsp_x86_64_v4_shared
      "effetune-dsp-simd-x86-64-v4"
      pipetune_effetune_pffft_x86_64_v4
      X86_64_V4
      PIPETUNE_EFFETUNE_BACKEND_VARIANT_X86_64_V4
      "${native_processor}"
      "${EFFETUNE_DSP_DIR}"
      "${generated_abi_source}"
      "${OUTPUT_DIRECTORY}")
    list(APPEND backend_targets pipetune_effetune_dsp_x86_64_v4_shared)
  endif()

  if(native_processor MATCHES "^(aarch64|arm64)$")
    pipetune_add_native_pffft(
      pipetune_effetune_pffft_arm64_sve
      ARM64_SVE
      "${native_processor}"
      "${EFFETUNE_DSP_DIR}")
    pipetune_add_native_shared_backend(
      pipetune_effetune_dsp_arm64_sve_shared
      "effetune-dsp-simd-arm64-sve"
      pipetune_effetune_pffft_arm64_sve
      ARM64_SVE
      PIPETUNE_EFFETUNE_BACKEND_VARIANT_ARM64_SVE
      "${native_processor}"
      "${EFFETUNE_DSP_DIR}"
      "${generated_abi_source}"
      "${OUTPUT_DIRECTORY}")
    list(APPEND backend_targets pipetune_effetune_dsp_arm64_sve_shared)
  endif()

  set(${OUTPUT_VARIABLE} ${backend_targets} PARENT_SCOPE)
endfunction()
