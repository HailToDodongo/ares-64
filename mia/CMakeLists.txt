add_library(mia STATIC mia.cpp mia.hpp)
add_library(ares::mia ALIAS mia)

if(ARES_ENABLE_CHD)
  target_compile_definitions(mia PUBLIC ARES_ENABLE_CHD)
  target_enable_feature(mia "CHD format support via libchdr")
else()
  target_disable_feature(mia "CHD format support via libchdr")
endif()

target_sources(
  mia
  PRIVATE
    medium/medium.cpp
    medium/medium.hpp
    medium/nintendo-64.cpp
    medium/nintendo-64dd.cpp
)

target_sources(mia PRIVATE pak/pak.cpp pak/pak.hpp)

target_sources(
  mia
  PRIVATE
    program/game-importer.cpp
    program/game-importer.hpp
    program/game-manager.cpp
    program/game-manager.hpp
    program/home.cpp
    program/home.hpp
    program/program.cpp
    program/program.hpp
    program/system-selection.cpp
    program/system-selection.hpp
)

target_sources(mia PRIVATE settings/settings.cpp settings/settings.hpp)

target_sources(
  mia
  PRIVATE
    system/nintendo-64.cpp
    system/nintendo-64dd.cpp
    system/system.cpp
    system/system.hpp
)

target_compile_definitions(mia PUBLIC MIA_LIBRARY)

target_include_directories(mia PRIVATE ${CMAKE_SOURCE_DIR})
target_include_directories(mia PRIVATE ${CMAKE_SOURCE_DIR}/thirdparty)
target_link_libraries(mia PRIVATE ares::ares ares::nall::headers tzxfile $<$<BOOL:${ARES_ENABLE_CHD}>:chdr-static>)


target_disable_subproject(mia-ui "mia (manifest generator) standalone frontend")

get_target_property(mia_SOURCES mia SOURCES)
set_source_files_properties(mia ${mia_SOURCES} PROPERTIES HEADER_FILE_ONLY TRUE)
set_source_files_properties(mia mia.cpp PROPERTIES HEADER_FILE_ONLY FALSE)
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${mia_SOURCES})
