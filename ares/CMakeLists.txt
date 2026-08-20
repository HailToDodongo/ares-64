add_library(ares STATIC ares/ares.cpp ares/memory/fixed-allocator.cpp)

configure_file(ares/ares.cpp.in ares/ares.cpp @ONLY)

add_library(ares::ares ALIAS ares)

option(ARES_PROFILE_ACCURACY "Build to prefer emulation accuracy over performance in certain specific areas" OFF)
option(ARES_PRECOMPILE_HEADERS "Precompile ares/ares.hpp header for performance" OFF)
option(
  ARES_ENABLE_DEBUG_TOOLS
  "\
Build the N64 debugging/profiling tools (CPU profiler, flame chart, RSP/RDP command viewers, framebuffer heatmap) plus \
the GDB server and instruction tracer. When OFF, every per-instruction/per-access hook is compiled out and the desktop \
UI is locked to a debug-free play mode.
"
  ON
)
if(ARES_BUILD_HEADLESS)
  # ares-test reads the last presented frame directly, which is only well-defined
  # if the screen delivers frames on the emulation thread. Note this applies to
  # every consumer of the ares library in this build tree, which is why the
  # headless runner has its own preset/build directory.
  target_compile_definitions(ares PUBLIC ARES_VIDEO_SYNCHRONOUS)
endif()

option(
  ARES_ENABLE_VULKAN
  "\
Build the paraLLEl-RDP Vulkan renderer for the N64 core. When OFF, the ~40 parallel-rdp/Granite translation units are \
not compiled at all and the angrylion CPU renderer (or the 'none' renderer) is used instead — intended for headless/CI \
builds that cannot assume a GPU.
"
  ON
)
option(
  ARES_UNITY_CORES
  "\
Build each core as one translation unit. Decreases cold build times but increases incremental build times. Slightly \
increases performance.
"
  OFF
)

if(ARES_PRECOMPILE_HEADERS)
  target_precompile_headers(ares PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:ares/ares.hpp>")
endif()

target_link_libraries(ares PRIVATE libco ares::nall::headers $<$<BOOL:${ARES_ENABLE_CHD}>:chdr-static> PUBLIC sljit)

if(ARES_PROFILE_ACCURACY)
  target_compile_definitions(ares PUBLIC PROFILE_ACCURACY)
else()
  target_compile_definitions(ares PUBLIC PROFILE_PERFORMANCE)
endif()

# Single switch for all debugging/profiling tooling. PUBLIC so it also reaches
# desktop-ui (which links ares::ares). 1 = full tools, 0 = lean play build.
target_compile_definitions(ares PUBLIC ARES_DEBUG_TOOLS=$<IF:$<BOOL:${ARES_ENABLE_DEBUG_TOOLS}>,1,0>)

target_sources(
  ares
  PRIVATE ares/ares.cpp ares/ares.cpp.in ares/ares.hpp ares/inline.hpp ares/platform.hpp ares/random.hpp ares/types.hpp
)

target_sources(
  ares
  PRIVATE
    ares/debug/debug.cpp
    ares/debug/debug.hpp
    ares/memory/fixed-allocator.cpp
    ares/memory/fixed-allocator.hpp
    ares/memory/memory.hpp
    ares/memory/readable.hpp
    ares/memory/writable.hpp
)

target_sources(
  ares
  PRIVATE ares/scheduler/scheduler.cpp ares/scheduler/scheduler.hpp ares/scheduler/thread.cpp ares/scheduler/thread.hpp
)

target_sources(
  ares
  PRIVATE
    ares/node/attribute.hpp
    ares/node/class.hpp
    ares/node/node.cpp
    ares/node/node.hpp
    ares/node/object.hpp
    ares/node/peripheral.hpp
    ares/node/port.hpp
    ares/node/system.hpp
)

target_sources(ares PRIVATE ares/node/audio/audio.hpp ares/node/audio/stream.cpp ares/node/audio/stream.hpp)

target_sources(ares PRIVATE ares/node/component/component.hpp ares/node/component/real-time-clock.hpp)

target_sources(
  ares
  PRIVATE
    ares/node/input/axis.hpp
    ares/node/input/button.hpp
    ares/node/input/input.hpp
    ares/node/input/rumble.hpp
    ares/node/input/trigger.hpp
)

target_sources(
  ares
  PRIVATE
    ares/node/setting/boolean.hpp
    ares/node/setting/integer.hpp
    ares/node/setting/natural.hpp
    ares/node/setting/real.hpp
    ares/node/setting/setting.hpp
    ares/node/setting/string.hpp
)

target_sources(
  ares
  PRIVATE
    ares/node/video/screen.cpp
    ares/node/video/screen.hpp
    ares/node/video/sprite.cpp
    ares/node/video/sprite.hpp
    ares/node/video/video.hpp
)

target_sources(
  ares
  PRIVATE
    ares/node/debugger/debugger.hpp
    ares/node/debugger/graphics.hpp
    ares/node/debugger/memory.hpp
    ares/node/debugger/properties.hpp
    ares/node/debugger/tracer/tracer.hpp
    ares/node/debugger/tracer/instruction.hpp
    ares/node/debugger/tracer/notification.hpp
)

if(OS_WINDOWS)
  target_link_libraries(
    ares
    PRIVATE uuid kernel32 user32 gdi32 comctl32 comdlg32 shell32
  )
elseif(OS_MACOS)
elseif(OS_LINUX OR OS_FREEBSD OR OS_OPENBSD)
  target_link_options(ares PRIVATE LINKER:-export-dynamic)
endif()

macro(ares_components)
  list(APPEND ares.components ${ARGV})
  set(ares.components ${ares.components} PARENT_SCOPE)
endmacro()

macro(ares_add_sources)
  set(options "")
  set(oneValueArgs CORE)
  set(multiValueArgs PRIMARY INCLUDED UNITY)
  cmake_parse_arguments(AAS "" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

  target_sources(ares PRIVATE ${AAS_INCLUDED} ${AAS_PRIMARY})

  if(ARES_UNITY_CORES)
    list(APPEND AAS_INCLUDED ${AAS_PRIMARY})
    target_sources(ares PRIVATE ${AAS_UNITY})
  endif()

  foreach(arg IN ITEMS ${AAS_INCLUDED})
    list(APPEND ARES_HEADER_SOURCES "${AAS_CORE}/${arg}")
  endforeach()
  set(ARES_HEADER_SOURCES ${ARES_HEADER_SOURCES} PARENT_SCOPE)
endmacro()

list(TRANSFORM ARES_CORES STRIP)

if(n64 IN_LIST ARES_CORES)
  enable_core("Nintendo 64")
  add_subdirectory(n64)
else()
  disable_core("Nintendo 64")
endif()


foreach(c IN LISTS ARES_CORES)
  # target_enable_feature(ares "${c} core" CORE_$<UPPER_CASE:${c}>)
  target_compile_definitions(ares PUBLIC CORE_$<UPPER_CASE:${c}>)
endforeach()

get_target_property(ares_SOURCES ares SOURCES)

set_source_files_properties(
  ares
  ares/debug/debug.cpp
  ares/node/audio/stream.cpp
  ares/node/node.cpp
  ares/node/video/screen.cpp
  ares/node/video/sprite.cpp
  ares/scheduler/thread.cpp
  ares/scheduler/scheduler.cpp
  PROPERTIES HEADER_FILE_ONLY TRUE
)

set_source_files_properties(
  n64/vulkan/parallel-rdp/vulkan/shader.cpp
  n64/vulkan/parallel-rdp/vulkan/descriptor_set.cpp
  n64/vulkan/parallel-rdp/vulkan/device.cpp
  n64/vulkan/parallel-rdp/vulkan/command_buffer.cpp
  n64/vulkan/parallel-rdp/vulkan/render_pass.cpp
  n64/vulkan/parallel-rdp/vulkan/context.cpp
  n64/vulkan/parallel-rdp/vulkan/memory_allocator.cpp
  n64/vulkan/parallel-rdp/vulkan/semaphore.cpp
  n64/vulkan/parallel-rdp/util/logging.cpp
  n64/vulkan/parallel-rdp/util/environment.cpp
  n64/vulkan/parallel-rdp/util/thread_name.cpp
  n64/vulkan/parallel-rdp/util/timer.cpp
  PROPERTIES SKIP_PRECOMPILE_HEADERS ON
)

set_source_files_properties(ares ${ARES_HEADER_SOURCES} PROPERTIES HEADER_FILE_ONLY TRUE)

set_source_files_properties(ares ares/ares.cpp ares/memory/fixed-allocator.cpp PROPERTIES HEADER_FILE_ONLY FALSE)

target_include_directories(ares INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(ares PRIVATE ${CMAKE_SOURCE_DIR}/thirdparty/json)

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${ares_SOURCES})
