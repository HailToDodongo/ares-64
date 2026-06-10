#pragma once

// Master switch for the debugging/profiling tools (set by CMake via ares::ares's
// PUBLIC compile definitions). Default on as a safety net. When 0, the desktop UI is
// locked to a debug-free play mode and all tool windows/menus are compiled out.
#ifndef ARES_DEBUG_TOOLS
  #define ARES_DEBUG_TOOLS 1
#endif

#include <imgui.h>
#include <nall/stdint.hpp>
#include "assign.hpp"

namespace ares::ui {

extern ImFont* monoFont;  // monospaced font for numeric columns

// Global DPI/zoom factor actually in use this session
extern float dpiScale;
// The DPI factor auto-detected at startup (framebuffer/window pixel ratio), 
// before any user override is applied. Shown in the settings UI as the "Detected" value.
extern float dpiScaleDetected;

extern bool uiScaleDirty;

extern bool playMode;

// Effective scale = user override (when enabled) else the detected DPI.
auto effectiveUiScale() -> float;
// Rebuild the theme + scale the UI (style metrics, fonts, and the _px literal).
auto applyUiScale(float scale) -> void;

}  // namespace ares::ui

// Pixel literal: scales a hand-tuned pixel size by the current DPI factor so it
// matches the (already-scaled) font and widgets. e.g. `8_px`, `6.0_px`.
inline float operator""_px(long double value) {
  return static_cast<float>(value) * ares::ui::dpiScale;
}
inline float operator""_px(unsigned long long value) {
  return static_cast<float>(value) * ares::ui::dpiScale;
}

namespace ares::ui {

auto DrawMainMenuBar() -> void;
auto DrawMenuBar() -> void;
auto DrawViewport() -> void;
auto DrawPlayMode() -> void;
auto DrawSettingsWindow() -> void;
auto DrawManifestViewer() -> void;
auto DrawCheatEditor() -> void;
auto DrawTracerViewer() -> void;
auto DrawRdpViewer() -> void;
auto DrawRspViewer() -> void;
auto DrawCpuProfiler() -> void;
auto DrawFlameChart() -> void;
auto DrawFramebufferViewer() -> void;
auto DrawTmemViewer() -> void;
auto DrawMemoryViewer() -> void;
auto DrawAudioViewer() -> void;
auto DrawStatusBar() -> void;

// Refresh functions called from Program::main() in imgui mode
auto RefreshTools() -> void;
auto SyncWindowVisibility() -> void;

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
extern bool showCpuProfiler;
extern bool showFlameChart;
extern bool showFramebufferViewer;
extern bool showTmemViewer;
extern bool showMemoryViewer;
extern bool showAudioViewer;
extern bool showAboutDialog;

}  // namespace ares::ui
