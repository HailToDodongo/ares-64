cmake_minimum_required(VERSION 3.28...4.1)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(ares VERSION ${ARES_VERSION_CANONICAL})

include(compilerconfig)
include(defaults)
include(helpers)

option(ARES_BUILD_HEADLESS "Build the ares-test headless test runner (Linux/BSD only)" OFF)

add_subdirectory(thirdparty)

add_subdirectory(nall/nall)
add_subdirectory(libco)
add_subdirectory(ruby)

# gersemi: off
set(
  ARES_CORES
  n64
  CACHE STRING "List of systems to be built by the project"
)
# gersemi: on

add_subdirectory(ares)
add_subdirectory(mia)
add_subdirectory(desktop-ui)

if(ARES_BUILD_HEADLESS)
  add_subdirectory(test-runner)
endif()


message_configuration()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.github/CMakeLists.txt")
  add_subdirectory(.github)
endif()
add_subdirectory(cmake)
return()
