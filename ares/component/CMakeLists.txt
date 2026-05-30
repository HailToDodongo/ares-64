list(REMOVE_DUPLICATES ares.components)

set(ARES_COMPONENTS_LIST "${ares.components}" CACHE STRING "Enabled shared hardware components" FORCE)

mark_as_advanced(ARES_COMPONENTS_LIST)

ares_add_sources(INCLUDED CMakeLists.txt)

macro(component_sources)
  set(options "")
  set(oneValueArgs COMPONENT PRIMARY)
  set(multiValueArgs INCLUDED)

  cmake_parse_arguments(CPS "" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

  if(${CPS_COMPONENT} IN_LIST ares.components)
    target_sources(ares PRIVATE ${CPS_PRIMARY})
    foreach(arg IN ITEMS ${CPS_INCLUDED})
      list(APPEND ARES_HEADER_SOURCES "component/${arg}")
      target_sources(ares PRIVATE ${arg})
    endforeach()
  endif()
  set(ARES_HEADER_SOURCES ${ARES_HEADER_SOURCES} PARENT_SCOPE)
endmacro()

component_sources(
  COMPONENT
    sm5k
  PRIMARY
    processor/sm5k/sm5k.cpp
  INCLUDED
    processor/sm5k/disassembler.cpp
    processor/sm5k/instruction.cpp
    processor/sm5k/instructions.cpp
    processor/sm5k/memory.cpp
    processor/sm5k/serialization.cpp
    processor/sm5k/sm5k.hpp
    processor/sm5k/timer.cpp
)
