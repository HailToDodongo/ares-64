#include "ui.hpp"

#include "../desktop-ui.hpp"
#include "../application/application.hpp"
#include <chrono>
#include <cstdio>

namespace ares::ui {

bool showSettingsWindow = false;
bool showToolsWindow = false;
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
  if(!ImGui::BeginMenu(emulator->name.data())) return;

  // Core-specific menu items are built by the emulator
  // TODO: implement emulator->load() callback
  ImGui::Separator();
  if(ImGui::MenuItem("Reset")) {
    Program::Guard guard;
    emulator->root->power();
  }
  if(ImGui::MenuItem("Unload")) {
    Program::Guard guard;
    program.unload();
  }

  ImGui::EndMenu();
}

static void DrawSettingsMenu() {
  if(!ImGui::BeginMenu("Settings")) return;

  // Window Size submenu
  if(ImGui::BeginMenu("Window Size")) {
    struct SizePreset { const char* label; u32 w; u32 h; };
    static const SizePreset presets[] = {
      {"640x480",   640,  480},
      {"960x720",   960,  720},
      {"1280x960",  1280, 960},
      {"1600x1200", 1600, 1200},
      {"1920x1440", 1920, 1440},
      {"2560x1920", 2560, 1920},
      {"3200x2400", 3200, 2400},
      {"3840x2880", 3840, 2880},
    };
    for(auto& p : presets) {
      if(ImGui::MenuItem(p.label)) {
        SDL_SetWindowSize(AresApp::window, p.w, p.h);
	        SDL_SetWindowPosition(AresApp::window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
      }
    }
    ImGui::EndMenu();
  }

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

  // Shader submenu
  if(ImGui::BeginMenu("Shader")) {
    if(ImGui::MenuItem("None", nullptr, settings.video.shader == "None")) {
      ruby::video.setShader("None");
      settings.video.shader = "None";
    }
    // Shaders are loaded dynamically; for now show None + placeholder
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
    Program::Guard guard;
    program.pause(!program.paused);
  }

  if(ImGui::MenuItem("Frame Advance")) {
    Program::Guard guard;
    if(!program.paused) program.pause(true);
    program.requestFrameAdvance = true;
  }

  if(ImGui::MenuItem("Reload Game")) {
    Program::Guard guard;
    program.load(emulator, emulator->game->location);
  }

  ImGui::Separator();

  // Tool panel openers
  if(ImGui::MenuItem("Manifest")) showToolsWindow = true;
  if(ImGui::MenuItem("Cheats")) showToolsWindow = true;
  if(ImGui::MenuItem("Memory")) showToolsWindow = true;
  if(ImGui::MenuItem("Graphics")) showToolsWindow = true;
  if(ImGui::MenuItem("Streams")) showToolsWindow = true;
  if(ImGui::MenuItem("Properties")) showToolsWindow = true;
  if(ImGui::MenuItem("Tracer")) showToolsWindow = true;
  if(ImGui::MenuItem("Tape")) showToolsWindow = true;

  ImGui::EndMenu();
}

static void DrawHelpMenu() {
  if(!ImGui::BeginMenu("Help")) return;
  if(ImGui::MenuItem("About" "...")) {
    showAboutDialog = true;
  }
  ImGui::EndMenu();
}

auto DrawMainMenuBar() -> void {
  if(!ImGui::BeginMainMenuBar()) return;

  DrawFileMenu();
  if(emulator) DrawSystemMenu();
  DrawSettingsMenu();
  if(emulator) DrawToolsMenu();
  DrawHelpMenu();

  // VPS counter on the right side of the menu bar
  if(emulator) {
    auto vps = program.vblanksPerSecond.load();
    char buf[32];
    snprintf(buf, sizeof(buf), "%u VPS", (u32)vps);
    auto textSize = ImGui::CalcTextSize(buf);
    ImGui::SameLine(ImGui::GetWindowWidth() - textSize.x - 16);
    ImGui::TextUnformatted(buf);

    // Log VPS every 2 seconds
    static auto lastLog = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    if(now - lastLog > std::chrono::seconds(2)) {
      auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
      fprintf(stderr, "[%lld.%02lld] VPS: %u\n",
              (long long)(ms / 1000), (long long)(ms % 1000) / 10, (u32)vps);
      lastLog = now;
    }
  }

  ImGui::EndMainMenuBar();
}

}  // namespace ares::ui
