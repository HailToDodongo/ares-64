target_sources(
  desktop-ui
  PRIVATE desktop-ui.hpp game-browser/game-browser.hpp input/hotkeys.cpp input/input.hpp presentation/presentation.hpp
)

target_sources(desktop-ui PRIVATE game-browser/game-browser.hpp)

target_sources(desktop-ui PRIVATE input/hotkeys.cpp input/input.hpp)

target_sources(desktop-ui PRIVATE presentation/presentation.hpp)

target_sources(
  desktop-ui
  PRIVATE
    program/drivers.cpp
    program/load.cpp
    program/platform.cpp
    program/program.hpp
    program/rewind.cpp
    program/states.cpp
    program/status.cpp
    program/utility.cpp
)

target_sources(desktop-ui PRIVATE resource/resource.cpp resource/resource.hpp)

target_sources(
  desktop-ui
  PRIVATE
    settings/hotkeys.cpp
    settings/input.cpp
    settings/settings.hpp
)

target_sources(
  desktop-ui
  PRIVATE
    tools/cheats.cpp
    tools/graphics.cpp
    tools/manifest.cpp
    tools/memory.cpp
    tools/properties.cpp
    tools/streams.cpp
    tools/tools.hpp
    tools/tracer.cpp
)

target_sources(
  desktop-ui
  PRIVATE
    emulator/arcade.cpp
    emulator/emulator.cpp
    emulator/emulator.hpp
    emulator/emulators.cpp
    emulator/nintendo-64.cpp
    emulator/nintendo-64dd.cpp
)
