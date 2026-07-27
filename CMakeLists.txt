cmake_minimum_required(VERSION 3.24)

project(PipeTuneWorkspace LANGUAGES C CXX)

include(CTest)

add_subdirectory(pipetune pipetune-build)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/pipetune-gtk/CMakeLists.txt")
  add_subdirectory(pipetune-gtk pipetune-gtk-build)
endif()

if(BUILD_TESTING)
  find_program(PIPETUNE_PACKAGE_NODE_EXECUTABLE node REQUIRED)
  find_program(PIPETUNE_PACKAGE_DPKG_DEB_EXECUTABLE dpkg-deb REQUIRED)
  add_test(
    NAME pipetune_package_build_scripts
    COMMAND
      "${PIPETUNE_PACKAGE_NODE_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/test/build-package.mjs"
      "${CMAKE_CURRENT_SOURCE_DIR}"
      "${PIPETUNE_PACKAGE_DPKG_DEB_EXECUTABLE}")
  set_tests_properties(
    pipetune_package_build_scripts PROPERTIES TIMEOUT 30)
endif()
