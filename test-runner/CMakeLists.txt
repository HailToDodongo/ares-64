# ares-test: headless N64 test runner (no SDL/ImGui/ruby, no window, no GPU).
# Built when ARES_BUILD_HEADLESS is ON; Linux/BSD only. Pairs naturally with
# -DARES_ENABLE_VULKAN=OFF (the linux-headless preset) so parallel-rdp is not
# compiled at all, but also builds in a normal tree for local use.

if(NOT (OS_LINUX OR OS_FREEBSD OR OS_OPENBSD))
  message(WARNING "ares-test is only supported on Linux/BSD; skipping")
  return()
endif()

add_executable(ares-test main.cpp runner.cpp js-host.cpp runner.hpp js-host.hpp)

target_include_directories(ares-test PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(ares-test PRIVATE ares::ares ares::mia ares::nall libco qjs)

if(ARES_ENABLE_CHD)
  target_link_libraries(ares-test PRIVATE chdr-static)
endif()

# (deliberately no ares_configure_executable: its rundir/bin copy would not carry
# the Firmware/ directory below, producing a binary that can't find the PIF ROMs)

# PIF firmware next to the binary: mia::locate() checks Path::program() first.
add_custom_command(TARGET ares-test POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory
    "$<TARGET_FILE_DIR:ares-test>/Firmware/Nintendo 64"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_SOURCE_DIR}/mia/Firmware/Nintendo 64/pif.ntsc.rom"
    "${CMAKE_SOURCE_DIR}/mia/Firmware/Nintendo 64/pif.pal.rom"
    "${CMAKE_SOURCE_DIR}/mia/Firmware/Nintendo 64/pif.sm5.rom"
    "$<TARGET_FILE_DIR:ares-test>/Firmware/Nintendo 64"
)
