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
  find_program(PIPETUNE_PACKAGE_MAKE_EXECUTABLE make REQUIRED)
  add_test(
    NAME pipetune_package_build_scripts
    COMMAND
      "${PIPETUNE_PACKAGE_NODE_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/test/build-package.mjs"
      "${CMAKE_CURRENT_SOURCE_DIR}"
      "${PIPETUNE_PACKAGE_DPKG_DEB_EXECUTABLE}")
  set_tests_properties(
    pipetune_package_build_scripts PROPERTIES TIMEOUT 30)
  add_test(
    NAME pipetune_makefile_install
    COMMAND
      "${PIPETUNE_PACKAGE_NODE_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/test/makefile-install.mjs"
      "${CMAKE_CURRENT_SOURCE_DIR}"
      "${PIPETUNE_PACKAGE_MAKE_EXECUTABLE}")
  set_tests_properties(pipetune_makefile_install PROPERTIES TIMEOUT 10)
  add_test(
    NAME pipetune_source_license_headers
    COMMAND
      "${PIPETUNE_PACKAGE_NODE_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/test/source-license-headers.mjs"
      "${CMAKE_CURRENT_SOURCE_DIR}")
  set_tests_properties(pipetune_source_license_headers PROPERTIES TIMEOUT 10)
endif()
