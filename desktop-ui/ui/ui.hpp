#pragma once

#include <imgui.h>
#include "assign.hpp"

namespace ares::ui {

extern ImFont* monoFont;  // monospaced font for numeric columns

auto DrawMainMenuBar() -> void;
auto DrawMenuBar() -> void;
auto DrawViewport() -> void;
auto DrawSettingsWindow() -> void;
auto DrawManifestViewer() -> void;
auto DrawCheatEditor() -> void;
auto DrawTracerViewer() -> void;
auto DrawRdpViewer() -> void;
auto DrawRspViewer() -> void;
auto DrawFramebufferViewer() -> void;
auto DrawTmemViewer() -> void;
auto DrawMemoryViewer() -> void;
auto DrawAudioViewer() -> void;
auto DrawStatusBar() -> void;

// Refresh functions called from Program::main() in imgui mode
auto RefreshTools() -> void;

extern bool showSettingsWindow;
extern bool showManifestViewer;
extern bool showCheatEditor;
extern bool showTracerViewer;
extern bool showRdpViewer;
extern bool showRspViewer;
extern bool showFramebufferViewer;
extern bool showTmemViewer;
extern bool showMemoryViewer;
extern bool showAudioViewer;
extern bool showAboutDialog;

}  // namespace ares::ui
