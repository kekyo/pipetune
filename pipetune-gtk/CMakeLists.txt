cmake_minimum_required(VERSION 3.24)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(GIO REQUIRED IMPORTED_TARGET gio-2.0 gio-unix-2.0)

add_library(
  pipetune_gtk_core STATIC
  src/application-state.cpp
  src/control-client.cpp
  src/startup-config.cpp)
target_include_directories(pipetune_gtk_core PUBLIC src)
target_link_libraries(
  pipetune_gtk_core
  PUBLIC pipetune_control
  PRIVATE PkgConfig::GIO)
if(MSVC)
  target_compile_options(pipetune_gtk_core PRIVATE /W4 /WX)
else()
  target_compile_options(
    pipetune_gtk_core PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

if(BUILD_TESTING)
  add_executable(
    pipetune_gtk_state_config_tests
    test/state-config-test.cpp)
  target_link_libraries(
    pipetune_gtk_state_config_tests PRIVATE pipetune_gtk_core)
  if(MSVC)
    target_compile_options(pipetune_gtk_state_config_tests PRIVATE /W4 /WX)
  else()
    target_compile_options(
      pipetune_gtk_state_config_tests PRIVATE
      -Wall -Wextra -Wpedantic -Werror)
  endif()
  add_test(
    NAME pipetune_gtk_state_config_tests
    COMMAND pipetune_gtk_state_config_tests)

  add_executable(
    pipetune_gtk_control_client_tests
    test/control-client-test.cpp)
  target_link_libraries(
    pipetune_gtk_control_client_tests
    PRIVATE pipetune_gtk_core PkgConfig::GIO Threads::Threads)
  if(MSVC)
    target_compile_options(pipetune_gtk_control_client_tests PRIVATE /W4 /WX)
  else()
    target_compile_options(
      pipetune_gtk_control_client_tests PRIVATE
      -Wall -Wextra -Wpedantic -Werror)
  endif()
  add_test(
    NAME pipetune_gtk_control_client_tests
    COMMAND pipetune_gtk_control_client_tests)
  set_tests_properties(
    pipetune_gtk_control_client_tests PROPERTIES TIMEOUT 10)
endif()
