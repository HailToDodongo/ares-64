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

if(ARES_BUILD_OPTIONAL_TARGETS)
  add_executable(mia-ui mia.cpp)
  target_include_directories(mia-ui PRIVATE ${CMAKE_SOURCE_DIR})
  target_link_libraries(
    mia-ui PRIVATE ares::hiro ares::ares ares::nall tzxfile
    "$<$<PLATFORM_ID:Darwin>:$<LINK_LIBRARY:FRAMEWORK,Carbon.framework>>"
    "$<$<PLATFORM_ID:Darwin>:$<LINK_LIBRARY:FRAMEWORK,IOKit.framework>>"
    "$<$<PLATFORM_ID:Darwin>:$<LINK_LIBRARY:FRAMEWORK,Security.framework>>"
    "$<$<PLATFORM_ID:Darwin>:mia>"
  )
  if(WIN32)
    target_sources(mia-ui PRIVATE resource/mia.rc resource/mia.Manifest)
  endif()
  set_target_properties(mia-ui PROPERTIES OUTPUT_NAME mia MACOSX_BUNDLE TRUE
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_BINARY_DIR}/mia.plist"
    XCODE_EMBED_FRAMEWORKS_REMOVE_HEADERS_ON_COPY YES
    XCODE_EMBED_FRAMEWORKS_CODE_SIGN_ON_COPY YES
    XCODE_EMBED_PLUGINS_REMOVE_HEADERS_ON_COPY YES
    XCODE_EMBED_PLUGINS_CODE_SIGN_ON_COPY YES
  )
  if(OS_MACOS)
    set_target_xcode_properties(mia-ui PROPERTIES PRODUCT_BUNDLE_IDENTIFIER dev.ares.mia
      PRODUCT_NAME mia ASSETCATALOG_COMPILER_APPICON_NAME AppIcon
      MARKETING_VERSION ${ARES_VERSION} GENERATE_INFOPLIST_FILE YES
      CLANG_ENABLE_OBJC_ARC YES INFOPLIST_KEY_CFBundleDisplayName "mia"
      INFOPLIST_KEY_NSHumanReadableCopyright "(c) 2004-${CURRENT_YEAR} ares team, Near et. al."
    )
  endif()
  ares_configure_executable(mia-ui)
  target_enable_subproject(mia-ui "mia (manifest generator) standalone frontend")
  set_target_properties(mia-ui PROPERTIES FOLDER tools PREFIX "")
else()
  target_disable_subproject(mia-ui "mia (manifest generator) standalone frontend")
endif()

get_target_property(mia_SOURCES mia SOURCES)
set_source_files_properties(mia ${mia_SOURCES} PROPERTIES HEADER_FILE_ONLY TRUE)
set_source_files_properties(mia mia.cpp PROPERTIES HEADER_FILE_ONLY FALSE)
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${mia_SOURCES})
