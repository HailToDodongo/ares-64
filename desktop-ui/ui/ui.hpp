#pragma once

#include <imgui.h>
#include "assign.hpp"

namespace ares::ui {

auto DrawMainMenuBar() -> void;
auto DrawViewport() -> void;
auto DrawSettingsWindow() -> void;
auto DrawToolsWindow() -> void;
auto DrawStatusBar() -> void;

// Refresh functions called from Program::main() in imgui mode
auto RefreshTools() -> void;

extern bool showSettingsWindow;
extern bool showToolsWindow;
extern bool showAboutDialog;

}  // namespace ares::ui
