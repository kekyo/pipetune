cmake_minimum_required(VERSION 3.24)

project(PipeTuneWorkspace VERSION 0.1.0 LANGUAGES C CXX)

include(CTest)

add_subdirectory(pipetune pipetune-build)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/pipetune-gtk/CMakeLists.txt")
  add_subdirectory(pipetune-gtk pipetune-gtk-build)
endif()
