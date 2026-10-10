include(ExternalProject)
include(ProcessorCount)
find_package(Git REQUIRED)
find_program(PIPETUNE_FFMPEG_MAKE make REQUIRED)
find_program(PIPETUNE_FFMPEG_XZ xz REQUIRED)

set(PIPETUNE_FFMPEG_SOURCE "${PIPETUNE_DEPS_DIR}/ffmpeg")
set(PIPETUNE_FFMPEG_COMMIT "f1e3a2bf7a2f2cde936d1ed97f09a26853d20125")
if(NOT EXISTS "${PIPETUNE_FFMPEG_SOURCE}/configure")
  message(FATAL_ERROR "FFmpeg is unavailable; initialize the workspace submodules")
endif()
execute_process(
  COMMAND "${GIT_EXECUTABLE}" -c "safe.directory=${PIPETUNE_FFMPEG_SOURCE}"
    -C "${PIPETUNE_FFMPEG_SOURCE}" rev-parse HEAD
  OUTPUT_VARIABLE ffmpeg_commit OUTPUT_STRIP_TRAILING_WHITESPACE
  COMMAND_ERROR_IS_FATAL ANY)
if(NOT ffmpeg_commit STREQUAL PIPETUNE_FFMPEG_COMMIT)
  message(FATAL_ERROR "Initialize the pinned FFmpeg submodule before building PipeTune")
endif()

set(PIPETUNE_FFMPEG_PREFIX "${CMAKE_CURRENT_BINARY_DIR}/ffmpeg/install")
set(PIPETUNE_FFMPEG_BUILD "${CMAKE_CURRENT_BINARY_DIR}/ffmpeg/build")
file(MAKE_DIRECTORY "${PIPETUNE_FFMPEG_PREFIX}/include")
ProcessorCount(ffmpeg_processors)
if(ffmpeg_processors LESS 1)
  set(ffmpeg_processors 1)
endif()
if(DEFINED ENV{PIPETUNE_MAKE_JOBS})
  set(ffmpeg_processors "$ENV{PIPETUNE_MAKE_JOBS}")
endif()
set(PIPETUNE_FFMPEG_JOBS "${ffmpeg_processors}" CACHE STRING "FFmpeg parallel build jobs")
if(NOT PIPETUNE_FFMPEG_JOBS MATCHES "^[1-9][0-9]*$")
  message(FATAL_ERROR "PIPETUNE_FFMPEG_JOBS must be a positive integer")
endif()

set(ffmpeg_arch "${CMAKE_SYSTEM_PROCESSOR}")
if(ffmpeg_arch MATCHES "^(x86_64|amd64|AMD64|i[3-6]86|x86)$")
  find_program(PIPETUNE_FFMPEG_NASM nasm REQUIRED)
  if(CMAKE_SIZEOF_VOID_P EQUAL 4)
    set(ffmpeg_arch x86_32)
  else()
    set(ffmpeg_arch x86_64)
  endif()
endif()

set(ffmpeg_byproducts)
foreach(component avformat avcodec avutil swresample)
  list(APPEND ffmpeg_byproducts "${PIPETUNE_FFMPEG_PREFIX}/lib/lib${component}-pipetune.so")
endforeach()
ExternalProject_Add(pipetune_ffmpeg_build
  PREFIX "${CMAKE_CURRENT_BINARY_DIR}/ffmpeg/steps"
  SOURCE_DIR "${PIPETUNE_FFMPEG_SOURCE}"
  BINARY_DIR "${PIPETUNE_FFMPEG_BUILD}"
  DOWNLOAD_COMMAND "" UPDATE_COMMAND "" PATCH_COMMAND ""
  CONFIGURE_COMMAND "${CMAKE_COMMAND}"
    "-DPIPETUNE_FFMPEG_SOURCE=${PIPETUNE_FFMPEG_SOURCE}"
    "-DPIPETUNE_FFMPEG_PREFIX=${PIPETUNE_FFMPEG_PREFIX}"
    "-DPIPETUNE_FFMPEG_CC=${CMAKE_C_COMPILER}"
    "-DPIPETUNE_FFMPEG_ARCH=${ffmpeg_arch}"
    "-DPIPETUNE_FFMPEG_CFLAGS=${CMAKE_C_FLAGS}"
    -P "${CMAKE_CURRENT_LIST_DIR}/ConfigureFFmpeg.cmake"
  BUILD_COMMAND "${PIPETUNE_FFMPEG_MAKE}" "-j${PIPETUNE_FFMPEG_JOBS}"
    COMMAND "${PIPETUNE_FFMPEG_MAKE}" install-libs install-headers
  BUILD_BYPRODUCTS ${ffmpeg_byproducts}
  INSTALL_COMMAND ""
  LOG_CONFIGURE ON LOG_BUILD ON
  LOG_OUTPUT_ON_FAILURE ON)
ExternalProject_Add_StepDependencies(pipetune_ffmpeg_build configure
  "${CMAKE_CURRENT_LIST_FILE}"
  "${CMAKE_CURRENT_LIST_DIR}/ConfigureFFmpeg.cmake"
  "${PIPETUNE_FFMPEG_SOURCE}/configure")

add_library(pipetune_ffmpeg INTERFACE)
foreach(component avformat avcodec avutil swresample)
  add_library(pipetune_ffmpeg_${component} SHARED IMPORTED GLOBAL)
  set_target_properties(pipetune_ffmpeg_${component} PROPERTIES
    IMPORTED_LOCATION "${PIPETUNE_FFMPEG_PREFIX}/lib/lib${component}-pipetune.so"
    INTERFACE_INCLUDE_DIRECTORIES "${PIPETUNE_FFMPEG_PREFIX}/include")
  add_dependencies(pipetune_ffmpeg_${component} pipetune_ffmpeg_build)
  target_link_libraries(pipetune_ffmpeg INTERFACE pipetune_ffmpeg_${component})
endforeach()

# Executables keep a relocatable search path for user-replaceable LGPL libraries.
function(pipetune_configure_ffmpeg_runtime target)
  file(RELATIVE_PATH library_relative "${CMAKE_INSTALL_FULL_BINDIR}"
    "${CMAKE_INSTALL_FULL_LIBDIR}/pipetune")
  set_property(TARGET ${target} APPEND PROPERTY
    INSTALL_RPATH "$ORIGIN/${library_relative}")
endfunction()

install(DIRECTORY "${PIPETUNE_FFMPEG_PREFIX}/lib/"
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/pipetune"
  FILES_MATCHING PATTERN "lib*-pipetune.so*")

set(ffmpeg_archive "${CMAKE_CURRENT_BINARY_DIR}/ffmpeg/ffmpeg-source.tar.xz")
add_custom_command(OUTPUT "${ffmpeg_archive}"
  COMMAND "${CMAKE_COMMAND}"
    "-DGIT_EXECUTABLE=${GIT_EXECUTABLE}"
    "-DXZ_EXECUTABLE=${PIPETUNE_FFMPEG_XZ}"
    "-DFFMPEG_SOURCE=${PIPETUNE_FFMPEG_SOURCE}"
    "-DFFMPEG_COMMIT=${PIPETUNE_FFMPEG_COMMIT}"
    "-DFFMPEG_ARCHIVE=${ffmpeg_archive}"
    -P "${CMAKE_CURRENT_LIST_DIR}/ArchiveFFmpeg.cmake"
  DEPENDS "${CMAKE_CURRENT_LIST_DIR}/ArchiveFFmpeg.cmake"
    "${CMAKE_CURRENT_LIST_FILE}"
  VERBATIM)
add_custom_target(pipetune_ffmpeg_source DEPENDS "${ffmpeg_archive}")
add_dependencies(pipetune_ffmpeg_build pipetune_ffmpeg_source)
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/ffmpeg/build-info.txt" CONTENT
"upstream=https://github.com/FFmpeg/FFmpeg.git
tag=n6.1.6
commit=${PIPETUNE_FFMPEG_COMMIT}
license=LGPL-2.1-or-later
compiler=${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION}
arch=${ffmpeg_arch}
cflags=${CMAKE_C_FLAGS}
configuration=ConfigureFFmpeg.cmake
source_changes=none
")
set(ffmpeg_documentation "${CMAKE_INSTALL_DATADIR}/doc/pipetune/ffmpeg")
install(FILES "${ffmpeg_archive}"
  "${CMAKE_CURRENT_BINARY_DIR}/ffmpeg/build-info.txt"
  "${CMAKE_CURRENT_LIST_DIR}/ConfigureFFmpeg.cmake"
  "${PIPETUNE_FFMPEG_SOURCE}/COPYING.LGPLv2.1"
  "${PIPETUNE_FFMPEG_SOURCE}/LICENSE.md"
  DESTINATION "${ffmpeg_documentation}")
install(FILES "${PIPETUNE_DEPS_DIR}/README.md"
  DESTINATION "${ffmpeg_documentation}" RENAME REBUILD.md)

if(BUILD_TESTING)
  add_executable(pipetune_ffmpeg_dependency_tests
    "${CMAKE_CURRENT_LIST_DIR}/../test/ffmpeg_dependency_test.c")
  target_link_libraries(pipetune_ffmpeg_dependency_tests PRIVATE pipetune_ffmpeg)
  set_target_properties(pipetune_ffmpeg_dependency_tests PROPERTIES BUILD_RPATH_USE_ORIGIN TRUE)
  target_compile_options(pipetune_ffmpeg_dependency_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
  add_test(NAME pipetune_ffmpeg_dependencies COMMAND pipetune_ffmpeg_dependency_tests)
  add_test(NAME pipetune_ffmpeg_relocation COMMAND bash
    "${CMAKE_CURRENT_LIST_DIR}/../test/ffmpeg-runtime-test.sh"
    "$<TARGET_FILE:pipetune_ffmpeg_dependency_tests>" "${PIPETUNE_FFMPEG_PREFIX}/lib")
endif()
