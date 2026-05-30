cmake_minimum_required(VERSION 3.28...4.1)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(ares VERSION ${ARES_VERSION_CANONICAL})

include(compilerconfig)
include(defaults)
include(helpers)

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

option(ARES_BUILD_OPTIONAL_TARGETS "Include supplemental tools and tests" OFF)

if(ARES_BUILD_OPTIONAL_TARGETS)
  add_subdirectory(tools/mame2bml)
else()
  target_disable_subproject(mame2bml "mame2bml (MAME manifest converter)")
endif()

add_subdirectory(tools/sourcery)

message_configuration()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.github/CMakeLists.txt")
  add_subdirectory(.github)
endif()
add_subdirectory(cmake)
return()
