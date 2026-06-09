add_executable(
  desktop-ui
  application/application.cpp
  ui/menus.cpp
  ui/viewport.cpp
  ui/filedialog.cpp
  ui/assign.cpp
  ui/settings.cpp
  ui/tools.cpp
  ui/audio-viewer.cpp
  ui/rdp-viewer.cpp
  ui/rsp-viewer.cpp
  ui/log-dump.cpp
  ui/cpu-profiler.cpp
  ui/flame-chart.cpp
  ui/framebuffer-viewer.cpp
  ui/tmem-viewer.cpp
  ui/memory-viewer.cpp
  ui/stubs.cpp
  desktop-ui.cpp
  input/input.cpp
  emulator/emulator.cpp
  program/program.cpp
  settings/settings.cpp
)

target_sources(desktop-ui PRIVATE application/application.hpp cmake/sources.cmake cmake/os-macos.cmake cmake/os-linux.cmake cmake/os-windows.cmake)

include(cmake/sources.cmake)

get_target_property(desktop-ui_SOURCES desktop-ui SOURCES)

set_source_files_properties(desktop-ui ${desktop-ui_SOURCES} PROPERTIES HEADER_FILE_ONLY TRUE)

set_source_files_properties(
  desktop-ui
  application/application.cpp
  ui/menus.cpp
  ui/viewport.cpp
  ui/filedialog.cpp
  ui/assign.cpp
  ui/settings.cpp
  ui/tools.cpp
  ui/audio-viewer.cpp
  ui/rdp-viewer.cpp
  ui/rsp-viewer.cpp
  ui/log-dump.cpp
  ui/cpu-profiler.cpp
  ui/flame-chart.cpp
  ui/framebuffer-viewer.cpp
  ui/tmem-viewer.cpp
  ui/memory-viewer.cpp
  ui/stubs.cpp
  desktop-ui.cpp
  input/input.cpp
  emulator/emulator.cpp
  program/program.cpp
  settings/settings.cpp
  PROPERTIES HEADER_FILE_ONLY FALSE
)

if(ARES_ENABLE_LIBRASHADER)
  find_package(slang_shaders)
endif()

if(OS_WINDOWS)
  include(cmake/os-windows.cmake)
elseif(OS_MACOS)
  include(cmake/os-macos.cmake)
elseif(OS_LINUX OR OS_FREEBSD OR OS_OPENBSD)
  include(cmake/os-linux.cmake)
endif()

target_include_directories(desktop-ui PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(
  desktop-ui
  PRIVATE ares::ruby ares::ares ares::mia ares::nall ares::imgui
)

if(ARES_ENABLE_CHD)
  target_link_libraries(desktop-ui PRIVATE chdr-static)
endif()

set_target_properties(desktop-ui PROPERTIES OUTPUT_NAME ares)

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${desktop-ui_SOURCES})

ares_configure_executable(desktop-ui)

if (APPLE)
  set(RESOURCE_DIR "$<TARGET_FILE_DIR:desktop-ui>/../Resources")
else()
  set(RESOURCE_DIR "$<TARGET_FILE_DIR:desktop-ui>")
endif()

# Copy font and firmware to build output
add_custom_command(TARGET desktop-ui POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_CURRENT_SOURCE_DIR}/resource/Altinn-DINExp.ttf"
    "${RESOURCE_DIR}/Altinn-DINExp.ttf"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_CURRENT_SOURCE_DIR}/resource/GoogleSansCode.ttf"
    "${RESOURCE_DIR}/GoogleSansCode.ttf"
  COMMAND ${CMAKE_COMMAND} -E make_directory
    "${RESOURCE_DIR}/Firmware/Nintendo 64"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_SOURCE_DIR}/mia/Firmware/Nintendo 64/pif.ntsc.rom"
    "${RESOURCE_DIR}/Firmware/Nintendo 64/pif.ntsc.rom"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_SOURCE_DIR}/mia/Firmware/Nintendo 64/pif.pal.rom"
    "${RESOURCE_DIR}/Firmware/Nintendo 64/pif.pal.rom"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_SOURCE_DIR}/mia/Firmware/Nintendo 64/pif.sm5.rom"
    "${RESOURCE_DIR}/Firmware/Nintendo 64/pif.sm5.rom"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_CURRENT_SOURCE_DIR}/ui/rsp-overlays/rspq-libdragon.json"
    "${RESOURCE_DIR}/rspq-libdragon.json"
)
