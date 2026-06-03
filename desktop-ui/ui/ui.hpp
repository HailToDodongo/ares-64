#pragma once

#include <imgui.h>
#include <nall/stdint.hpp>
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

struct LogDumpState {
  bool rsp = false;          // dump the RSP command log
  bool rdp = false;          // dump the RDP command log
  u32 startFrame = 0;        // number of presented frames to skip first
  u32 frameCount = 1;        // number of presented frames to dump
  u32 seenFrames = 0;        // presented frames observed so far (worker thread)
  u32 dumpedFrames = 0;      // frames already dumped (worker thread)
  auto active() const -> bool { return rsp || rdp; }
};
extern LogDumpState logDump;
// Called per framebuffer swap with the just-committed command counts. Returns
// true once the requested frames have been dumped, signalling ares to quit.
auto LogDumpOnFrame(u32 rspCount, u32 rdpCount) -> bool;

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
