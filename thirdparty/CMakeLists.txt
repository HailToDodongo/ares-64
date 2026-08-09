add_library(sljit STATIC sljit/sljit_src/sljitLir.c)

target_include_directories(sljit PUBLIC ../thirdparty)
target_compile_definitions(sljit PUBLIC SLJIT_HAVE_CONFIG_PRE=1 SLJIT_HAVE_CONFIG_POST=1)
target_compile_options(sljit PRIVATE $<$<COMPILE_LANG_AND_ID:C,AppleClang,Clang,GNU>:-Wno-conditional-uninitialized>)

option(ARES_ENABLE_CHD "Enable CHD format support via libchdr" ON)

if(ARES_ENABLE_CHD)
  # lzma
  add_subdirectory(libchdr/deps/lzma-25.01 EXCLUDE_FROM_ALL)
  list(APPEND CHDR_INCLUDES lzma)

  # miniz
  add_subdirectory(libchdr/deps/miniz-3.1.1 EXCLUDE_FROM_ALL)
  set_target_properties(
    miniz
    PROPERTIES POSITION_INDEPENDENT_CODE ON FOLDER thirdparty PREFIX ""
  )
  list(APPEND CHDR_LIBS miniz)
  
  # zstd
  option(ZSTD_BUILD_SHARED "BUILD SHARED LIBRARIES" OFF)
  option(ZSTD_BUILD_PROGRAMS "BUILD PROGRAMS" OFF)
  add_subdirectory(libchdr/deps/zstd-1.5.7 EXCLUDE_FROM_ALL)
  list(APPEND CHDR_LIBS zstd)
  #--------------------------------------------------
  # chdr
  #--------------------------------------------------

  set(
    CHDR_SOURCES
    libchdr/src/libchdr_bitstream.c
    libchdr/src/libchdr_cdrom.c
    libchdr/src/libchdr_chd.c
    libchdr/src/libchdr_flac.c
    libchdr/src/libchdr_huffman.c
    libchdr/src/libchdr_codec_cdfl.c
    libchdr/src/libchdr_codec_cdlz.c
    libchdr/src/libchdr_codec_cdzl.c
    libchdr/src/libchdr_codec_cdzs.c
    libchdr/src/libchdr_codec_flac.c
    libchdr/src/libchdr_codec_huff.c
    libchdr/src/libchdr_codec_lzma.c
    libchdr/src/libchdr_codec_zlib.c
    libchdr/src/libchdr_codec_zstd.c
    libchdr/deps/lzma-25.01/src/LzmaDec.c
  )

  list(APPEND CHDR_INCLUDES ${CMAKE_CURRENT_BINARY_DIR}/libchdr/include)

  add_library(chdr-static STATIC ${CHDR_SOURCES})
  target_include_directories(chdr-static PUBLIC ${CHDR_INCLUDES} PUBLIC libchdr/include)
  target_link_libraries(chdr-static PRIVATE ${CHDR_LIBS} ${PLATFORM_LIBS})
  target_compile_options(
    chdr-static
    PRIVATE $<$<CXX_COMPILER_ID:Clang,AppleClang>:-Wno-unreachable-code -Wno-unused-function>
  )
endif()

add_library(
  tzxfile
  STATIC
  TZXFile/TZXAudioGenerator.cpp
  TZXFile/TZXBlock.cpp
  TZXFile/TZXBlockArchiveInfo.cpp
  TZXFile/TZXBlockCustomInfo.cpp
  TZXFile/TZXBlockGroupEnd.cpp
  TZXFile/TZXBlockGroupStart.cpp
  TZXFile/TZXBlockHardwareType.cpp
  TZXFile/TZXBlockKansasCityStandard.cpp
  TZXFile/TZXBlockLoopEnd.cpp
  TZXFile/TZXBlockLoopStart.cpp
  TZXFile/TZXBlockMessage.cpp
  TZXFile/TZXBlockPause.cpp
  TZXFile/TZXBlockPulseSequence.cpp
  TZXFile/TZXBlockPureData.cpp
  TZXFile/TZXBlockPureTone.cpp
  TZXFile/TZXBlockStandardSpeedData.cpp
  TZXFile/TZXBlockStopTheTape48K.cpp
  TZXFile/TZXBlockTextDescription.cpp
  TZXFile/TZXBlockTurboSpeedData.cpp
  TZXFile/TZXFile.cpp
)

target_include_directories(tzxfile PUBLIC ../thirdparty/TZXFile)

add_library(
  ymfm
  STATIC
  ymfm/src/ymfm_adpcm.cpp
  ymfm/src/ymfm_misc.cpp
  ymfm/src/ymfm_opl.cpp
  ymfm/src/ymfm_opm.cpp
  ymfm/src/ymfm_opn.cpp
  ymfm/src/ymfm_opq.cpp
  ymfm/src/ymfm_opz.cpp
  ymfm/src/ymfm_pcm.cpp
  ymfm/src/ymfm_ssg.cpp
)

target_include_directories(ymfm PUBLIC ../thirdparty/ymfm/src)

target_compile_options(ymfm PRIVATE $<$<CXX_COMPILER_ID:Clang,AppleClang>:-Wno-unreachable-code>)

add_library(qon STATIC qon/qon.cpp)

set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
set(SDL_DISABLE_UNINSTALL ON CACHE BOOL "" FORCE)
set(SDL_AUDIO ON CACHE BOOL "" FORCE)
set(SDL_JOYSTICK ON CACHE BOOL "" FORCE)
set(SDL_HAPTIC OFF CACHE BOOL "" FORCE)
set(SDL_HIDAPI OFF CACHE BOOL "" FORCE)
set(SDL_SENSOR OFF CACHE BOOL "" FORCE)
set(SDL_CAMERA OFF CACHE BOOL "" FORCE)
set(SDL_X11_XTEST OFF CACHE BOOL "" FORCE)
add_subdirectory(SDL)

add_library(
  imgui
  STATIC
  imgui/imgui.cpp
  imgui/imgui_draw.cpp
  imgui/imgui_tables.cpp
  imgui/imgui_widgets.cpp
  imgui/backends/imgui_impl_sdl3.cpp
  imgui/backends/imgui_impl_sdlgpu3.cpp
)

target_include_directories(imgui PUBLIC imgui imgui/backends)
target_link_libraries(imgui PUBLIC SDL3::SDL3)

add_library(ares::imgui ALIAS imgui)
set_target_properties(imgui PROPERTIES FOLDER thirdparty PREFIX "")

# quickjs-ng (v0.15.1, same tree as pyrite64/vendored/quickjs-ng): JS runtime for the
# ares-test scripting host. Core interpreter only — no libc, no CLI, no examples.
if(ARES_BUILD_HEADLESS)
  set(QJS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(QJS_BUILD_CLI_STATIC OFF CACHE BOOL "" FORCE)
  set(QJS_BUILD_LIBC OFF CACHE BOOL "" FORCE)
  set(QJS_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
  add_subdirectory(quickjs-ng EXCLUDE_FROM_ALL)
  set_target_properties(qjs PROPERTIES FOLDER thirdparty PREFIX "")
endif()

set_target_properties(ymfm PROPERTIES FOLDER thirdparty PREFIX "")
set_target_properties(tzxfile PROPERTIES FOLDER thirdparty PREFIX "")
set_target_properties(sljit PROPERTIES FOLDER thirdparty PREFIX "")
if(ARES_ENABLE_CHD)
  set_target_properties(chdr-static PROPERTIES FOLDER thirdparty PREFIX "")
  set_target_properties(miniz PROPERTIES FOLDER thirdparty PREFIX "")
  set_target_properties(zstd PROPERTIES FOLDER thirdparty PREFIX "")
endif()
set_target_properties(qon PROPERTIES FOLDER thirdparty PREFIX "")
