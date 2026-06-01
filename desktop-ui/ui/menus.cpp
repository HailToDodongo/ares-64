#include "ui.hpp"

#include "../desktop-ui.hpp"
#include "../application/application.hpp"
#include <n64/n64.hpp>
#include <chrono>
#include <cstdio>

namespace ares::ui {

bool showSettingsWindow = false;
bool showAboutDialog = false;

static void DrawFileMenu() {
  if(!ImGui::BeginMenu("File")) return;

  // Recent Games
  if(ImGui::BeginMenu("Recent Games")) {
    u32 count = 0;
    for(u32 i : range(9)) {
      if(settings.recent.game[i].length() > 0) count++;
    }
    if(count > 0) {
      for(u32 i : range(9)) {
        auto entry = settings.recent.game[i];
        if(!entry.length()) continue;
        auto parts = nall::split(entry, ";", 1L);
        parts.resize(2);
        auto system = parts[0];
        auto location = parts[1];
        auto label = string{Location::base(location).trimRight("/"), " (", system, ")"};
        if(ImGui::MenuItem(label.data())) {
          Program::Guard guard;
          if(inode::exists(location)) {
            for(auto& emulator : emulators) {
              if(emulator->name == system) {
                program.load(emulator, location);
                break;
              }
            }
          }
        }
      }
      ImGui::Separator();
      if(ImGui::MenuItem("Clear History")) {
        for(u32 i : range(9)) settings.recent.game[i] = {};
      }
    } else {
      ImGui::TextDisabled("No recent games");
    }
    ImGui::EndMenu();
  }

  ImGui::Separator();

  // System groups, collect unique groups, show Arcade first
  std::vector<string> groups;
  for(auto& emulator : emulators) {
    if(!emulator->configuration.visible) continue;
    auto g = emulator->group();
    if(std::ranges::find(groups, g) == groups.end()) groups.push_back(g);
  }
  std::ranges::sort(groups, [](auto& a, auto& b) {
    if(a == "Arcade") return true;
    if(b == "Arcade") return false;
    return a < b;
  });

  for(auto& group : groups) {
    if(ImGui::BeginMenu(group.data())) {
      for(auto& emu : emulators) {
        if(!emu->configuration.visible) continue;
        if(emu->group() != group) continue;
        if(ImGui::MenuItem(emu->name.data())) {
          program.load(emu);
        }
      }
      ImGui::EndMenu();
    }
  }

  ImGui::Separator();
  if(ImGui::MenuItem("Quit")) {
    AresApp::quit();
  }

  ImGui::EndMenu();
}

static void DrawSystemMenu() {
  if(!emulator) return;

  // Direct menu-bar entry (no submenu): reset the running system.
  if(ImGui::MenuItem("Reset")) {
    Program::Guard guard;
    emulator->root->power();
  }
}

static void DrawSettingsMenu() {
  if(!ImGui::BeginMenu("Settings")) return;

  // Output submenu
  if(ImGui::BeginMenu("Output")) {
    if(ImGui::MenuItem("Scale: Best Fit", nullptr, settings.video.output == "Scale")) {
      settings.video.output = "Scale";
    }
    if(ImGui::MenuItem("Scale: Integer (auto)", nullptr, settings.video.output == "Integer")) {
      settings.video.output = "Integer";
    }
    if(ImGui::MenuItem("Scale: Stretch to Fill", nullptr, settings.video.output == "Stretch")) {
      settings.video.output = "Stretch";
    }
    ImGui::Separator();
    if(ImGui::MenuItem("Aspect: No correction", nullptr, settings.video.aspectCorrection == "None")) {
      settings.video.aspectCorrection = "None";
    }
    if(ImGui::MenuItem("Aspect: Standard", nullptr, settings.video.aspectCorrection == "Standard")) {
      settings.video.aspectCorrection = "Standard";
    }
    if(ImGui::MenuItem("Aspect: Anamorphic (16:9)", nullptr, settings.video.aspectCorrection == "Anamorphic")) {
      settings.video.aspectCorrection = "Anamorphic";
    }
    ImGui::Separator();
    bool adaptive = settings.video.adaptiveSizing;
    if(ImGui::MenuItem("Auto resize to content", nullptr, &adaptive)) {
      settings.video.adaptiveSizing = adaptive;
    }
    bool center = settings.video.autoCentering;
    if(ImGui::MenuItem("Auto center", nullptr, &center)) {
      settings.video.autoCentering = center;
    }
    ImGui::EndMenu();
  }


  // Boot Options submenu
  if(ImGui::BeginMenu("Boot Options")) {
    bool fastBoot = settings.boot.fast;
    if(ImGui::MenuItem("Fast Boot", nullptr, &fastBoot)) {
      settings.boot.fast = fastBoot;
    }
    bool tracer = settings.boot.debugger;
    if(ImGui::MenuItem("Launch Tracer", nullptr, &tracer)) {
      settings.boot.debugger = tracer;
    }
    bool gdb = settings.boot.awaitGDBClient;
    if(ImGui::MenuItem("Await GDB Client", nullptr, &gdb)) {
      settings.boot.awaitGDBClient = gdb;
    }
    ImGui::Separator();
    if(ImGui::BeginMenu("Region Preference")) {
      const char* regions[] = {"NTSC-U->NTSC-J->PAL", "NTSC-U->PAL->NTSC-J",
                               "NTSC-J->NTSC-U->PAL", "NTSC-J->PAL->NTSC-U",
                               "PAL->NTSC-U->NTSC-J", "PAL->NTSC-J->NTSC-U"};
      for(auto& r : regions) {
        if(ImGui::MenuItem(r, nullptr, settings.boot.prefer == r)) {
          settings.boot.prefer = r;
        }
      }
      ImGui::EndMenu();
    }
    ImGui::EndMenu();
  }

  ImGui::Separator();

  bool mute = settings.audio.mute;
  if(ImGui::MenuItem("Mute Audio", nullptr, &mute)) {
    settings.audio.mute = mute;
  }

  ImGui::Separator();
  if(ImGui::MenuItem("Toggle Fullscreen", "F11")) {
    bool fs = SDL_GetWindowFlags(AresApp::window) & SDL_WINDOW_FULLSCREEN;
    SDL_SetWindowFullscreen(AresApp::window, !fs);
  }
  ImGui::Separator();

  if(ImGui::MenuItem("Video" "...")) showSettingsWindow = true;
  if(ImGui::MenuItem("Audio" "...")) showSettingsWindow = true;
  if(ImGui::MenuItem("Input" "...")) showSettingsWindow = true;
  if(ImGui::MenuItem("Hotkeys" "...")) showSettingsWindow = true;

  ImGui::EndMenu();
}

static void DrawToolsMenu() {
  if(!emulator) return;
  if(!ImGui::BeginMenu("Tools")) return;

  // Save State slots
  if(ImGui::BeginMenu("Save State")) {
    for(u32 slot : range(9)) {
      auto label = string{"Slot ", 1 + slot};
      if(ImGui::MenuItem(label.data())) {
        Program::Guard guard;
        program.stateSave(1 + slot);
      }
    }
    ImGui::EndMenu();
  }

  // Load State slots
  if(ImGui::BeginMenu("Load State")) {
    for(u32 slot : range(9)) {
      auto label = string{"Slot ", 1 + slot};
      if(ImGui::MenuItem(label.data())) {
        Program::Guard guard;
        program.stateLoad(1 + slot);
      }
    }
    ImGui::EndMenu();
  }

  if(ImGui::MenuItem("Undo Last Save State")) {
    Program::Guard guard;
    program.undoStateSave();
  }
  if(ImGui::MenuItem("Undo Last Load State")) {
    Program::Guard guard;
    program.undoStateLoad();
  }

  ImGui::Separator();

  if(ImGui::MenuItem("Capture Screenshot")) {
    Program::Guard guard;
    program.requestScreenshot = true;
  }

  bool paused = program.paused;
  if(ImGui::MenuItem("Pause Emulation", nullptr, &paused)) {
    auto& rspCap = ares::Nintendo64::rsp.capture;
    auto& rdpCap = ares::Nintendo64::rdp.capture;
    if(program.paused || rspCap.stepMode.load() || rdpCap.stepMode.load()) {
      // Unpausing: break spin-waits FIRST, then Guard
      rspCap.stepMode.store(false, std::memory_order_release);
      rspCap.stepPending.store(true, std::memory_order_release);
      rdpCap.stepMode.store(false, std::memory_order_release);
      rdpCap.stepPending.store(true, std::memory_order_release);
      Program::Guard guard;
      program.pause(false);
    } else if(program.stepType == Program::StepType::Frame) {
      Program::Guard guard;
      program.pause(true);
    } else {
      rspCap.stepMode.store(program.stepType == Program::StepType::RSP, std::memory_order_release);
      rdpCap.stepMode.store(program.stepType == Program::StepType::RDP, std::memory_order_release);
    }
  }

  ImGui::Separator();

  if(ImGui::MenuItem("Step >")) {
    switch(program.stepType) {
    case Program::StepType::Frame:
      program.requestFrameAdvance = true;
      break;
    case Program::StepType::RSP:
      program.stepSequence++;
      ares::Nintendo64::rsp.capture.stepPending.store(true, std::memory_order_release);
      break;
    case Program::StepType::RDP:
      program.stepSequence++;
      ares::Nintendo64::rdp.capture.stepPending.store(true, std::memory_order_release);
      break;
    default:
      program.requestFrameAdvance = true;
      break;
    }
  }

  ImGui::Separator();

  // Tool panel openers
  if(ImGui::MenuItem("Manifest Viewer")) showManifestViewer = true;
  if(ImGui::MenuItem("Cheat Editor")) showCheatEditor = true;
  if(ImGui::MenuItem("Trace Logger")) showTracerViewer = true;
  if(ImGui::MenuItem("RDP Commands")) showRdpViewer = true;
  if(ImGui::MenuItem("RSP Commands")) showRspViewer = true;
  if(ImGui::MenuItem("Framebuffer")) showFramebufferViewer = true;
  if(ImGui::MenuItem("TMEM")) showTmemViewer = true;
  if(ImGui::MenuItem("Memory Editor")) showMemoryViewer = true;
  if(ImGui::MenuItem("Audio Viewer")) showAudioViewer = true;

  ImGui::EndMenu();
}

static void DrawHelpMenu() {
  if(!ImGui::BeginMenu("Help")) return;
  if(ImGui::MenuItem("About" "...")) {
    showAboutDialog = true;
  }
  ImGui::EndMenu();
}

static auto drawStepTypeCombo() -> void {
  static const char* stepNames[] = {"Frame", "RSP", "RDP"};
  int st = (int)program.stepType - 1;
  if(st < 0) st = 0;

  ImGui::Text("Step:");
  ImGui::SameLine();

  ImGui::SetNextItemWidth(80);
  if(ImGui::Combo("##steptype_bar", &st, stepNames, 3)) {
    program.stepType = (Program::StepType)(st + 1);
  }
}

// Live RDP renderer selector, shown only for N64-based systems. Writes the choice to
// settings (persisted across restarts) and requests a hot-swap that the emu worker
// applies at the next frame boundary.
static auto drawRendererCombo() -> void {
  if(!ares::Nintendo64::system.node) return;
  static const char* names[] = {"paraLLEl", "angrylion"};
  int idx = settings.video.renderer == "angrylion" ? 1 : 0;

  ImGui::Text("RDP:");
  ImGui::SameLine();

  ImGui::SetNextItemWidth(110);
  if(ImGui::Combo("##rdp_bar", &idx, names, 2)) {
    settings.video.renderer = names[idx];
    using Renderer = ares::Nintendo64::System::Renderer;
    ares::Nintendo64::system.requestRenderer(idx == 1 ? Renderer::Angrylion : Renderer::ParallelRDP);
  }
}

// Width occupied by drawRendererCombo (0 when hidden), used for right-aligned layout.
static auto rendererComboWidth() -> float {
  if(!ares::Nintendo64::system.node) return 0.0f;
  return ImGui::CalcTextSize("RDP:").x + ImGui::GetStyle().ItemSpacing.x + 110 + ImGui::GetStyle().ItemSpacing.x;
}

auto DrawMainMenuBar() -> void {
  if(!ImGui::BeginMainMenuBar()) return;

  DrawFileMenu();
  if(emulator) DrawSystemMenu();
  DrawSettingsMenu();
  if(emulator) DrawToolsMenu();
  DrawHelpMenu();

  if(emulator) {
    // Right-align, from the VPS counter leftward: [RDP combo] [Step combo] [VPS].
    auto vps = program.vblanksPerSecond.load();
    char buf[32];
    snprintf(buf, sizeof(buf), "%u VPS", (u32)vps);
    float vpsW = ImGui::CalcTextSize(buf).x;
    float comboW = ImGui::CalcTextSize("Step:").x + ImGui::GetStyle().ItemSpacing.x + 80;
    float rdpW = rendererComboWidth();
    float pad = ImGui::GetStyle().ItemSpacing.x;
    ImGui::SameLine(ImGui::GetWindowWidth() - vpsW - comboW - pad - rdpW - 16);
    drawRendererCombo();

    ImGui::SameLine(ImGui::GetWindowWidth() - vpsW - comboW - pad - 16);
    drawStepTypeCombo();

    ImGui::SameLine(ImGui::GetWindowWidth() - vpsW - 16);
    ImGui::TextUnformatted(buf);
  }

  ImGui::EndMainMenuBar();
}

// Menu bar for use inside a dockable window (BeginMenuBar vs BeginMainMenuBar)
auto DrawMenuBar() -> void {
  if(!ImGui::BeginMenuBar()) return;

  DrawFileMenu();
  if(emulator) DrawSystemMenu();
  DrawSettingsMenu();
  if(emulator) DrawToolsMenu();
  DrawHelpMenu();

  // VPS counter on the right, with the Step and RDP-renderer combos just left of it.
  if(emulator) {
    auto vps = program.vblanksPerSecond.load();
    char buf[32];
    snprintf(buf, sizeof(buf), "%u VPS", (u32)vps);
    float vpsW = ImGui::CalcTextSize(buf).x;
    float comboW = ImGui::CalcTextSize("Step:").x + ImGui::GetStyle().ItemSpacing.x + 80;
    float rdpW = rendererComboWidth();
    float pad = ImGui::GetStyle().ItemSpacing.x;
    ImGui::SameLine(ImGui::GetWindowWidth() - vpsW - comboW - pad - rdpW - 16);
    drawRendererCombo();

    ImGui::SameLine(ImGui::GetWindowWidth() - vpsW - comboW - pad - 16);
    drawStepTypeCombo();

    ImGui::SameLine(ImGui::GetWindowWidth() - vpsW - 16);
    ImGui::TextUnformatted(buf);

    // Log VPS every 2 seconds
    /*static auto lastLog = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    if(now - lastLog > std::chrono::seconds(2)) {
      auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
      fprintf(stderr, "[%lld.%02lld] VPS: %u\n",
              (long long)(ms / 1000), (long long)(ms % 1000) / 10, (u32)vps);
      lastLog = now;
    }*/
  }

  ImGui::EndMenuBar();
}

}  // namespace ares::ui
