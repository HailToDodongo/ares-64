#pragma once

#include <imgui.h>
#include "assign.hpp"

namespace ares::ui {

auto DrawMainMenuBar() -> void;
auto DrawMenuBar() -> void;
auto DrawViewport() -> void;
auto DrawSettingsWindow() -> void;
auto DrawManifestViewer() -> void;
auto DrawCheatEditor() -> void;
auto DrawTracerViewer() -> void;
auto DrawRdpViewer() -> void;
auto DrawFramebufferViewer() -> void;
auto DrawAudioViewer() -> void;
auto DrawStatusBar() -> void;

// Refresh functions called from Program::main() in imgui mode
auto RefreshTools() -> void;

extern bool showSettingsWindow;
extern bool showManifestViewer;
extern bool showCheatEditor;
extern bool showTracerViewer;
extern bool showRdpViewer;
extern bool showFramebufferViewer;
extern bool showAudioViewer;
extern bool showAboutDialog;

}  // namespace ares::ui
