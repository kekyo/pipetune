cmake_minimum_required(VERSION 3.24)

file(
  STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION"
  PIPETUNE_PROJECT_VERSION
  LIMIT_COUNT 1)
if(NOT PIPETUNE_PROJECT_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
  message(FATAL_ERROR "VERSION must contain a semantic version")
endif()

project(
  PipeTuneWorkspace
  VERSION "${PIPETUNE_PROJECT_VERSION}"
  LANGUAGES C CXX)

include(CTest)

add_subdirectory(pipetune pipetune-build)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/pipetune-gtk/CMakeLists.txt")
  add_subdirectory(pipetune-gtk pipetune-gtk-build)
endif()
