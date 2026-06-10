#include "desktop-ui.hpp"
#include "application/application.hpp"
#include "ui/ui.hpp"


namespace ruby {
  Video video;
  Audio audio;
  Input input;
}

auto locate(const string& name) -> string {
  // First check each path for the presence of the file we are looking for in the following order
  // allowing users to override the default resources if they wish to do so.

  // 1. The application directory
  string location = {Path::program(), name};
  if(inode::exists(location)) return location;

  // 2. The user data directory
  location = {Path::userData(), "ares-imgui/", name};
  if(inode::exists(location)) return location;

  // 3. The shared data directory
#if defined(PLATFORM_LINUX) || defined(PLATFORM_BSD)
  /// Unix-like systems have multiple notions of a 'shared data' directory. First, check for
  /// an install prefix, as would be used by package managers that do not use `/usr/share`.
  /// Secondly, look in `/usr/local/share` to cover software compiled by the user.
  /// Lastly, look in the 'global' shared data directory, `/usr/share`.
  location = {Path::prefixSharedData(), "ares/", name};
  if(inode::exists(location)) return location;
  
  location = {Path::localSharedData(), "ares/", name};
  if(inode::exists(location)) return location;
#endif

  location = {Path::sharedData(), "ares/", name};
  if(inode::exists(location)) return location;

  // 4. The application bundle resource directory (macOS only)
#if defined(PLATFORM_MACOS)
  location = {Path::resources(), name};
  if(inode::exists(location)) return location;
#endif

  // If the file was not found in any of the above locations, we may be intending to create it
#if defined(PLATFORM_WINDOWS)
  // We must return a path to a user writable directory; on Windows, this is the executable directory
  return {Path::program(), name};
#else
  // On other platforms, this is the "user data" directory
  directory::create({Path::userData(), "ares-imgui/"});
  return {Path::userData(), "ares-imgui/", name};
#endif

}

#include <nall/main.hpp>
auto nall::main(Arguments arguments) -> void {
  //force early allocation for better proximity to executable code
  ares::Memory::FixedAllocator::get();

#if defined(PLATFORM_WINDOWS)
  bool createTerminal = arguments.take("--terminal");
  terminal::redirectStdioToTerminal(createTerminal);
#endif

  // Application::setName handled by SDL3 window title
  // Application::setScreenSaver handled by SDL3

  mia::setHomeLocation([]() -> string {
    if(auto location = settings.paths.home) return location;
    return locate("Systems/");
  });

  mia::setSaveLocation([]() -> string {
    return settings.paths.saves;
  });

  if(arguments.take("--fullscreen")) {
    program.startFullScreen = true;
  } else if(arguments.take("--pseudofullscreen")) {
    program.startPseudoFullScreen = true;
  }

  if(arguments.take("--kiosk")) {
    program.kiosk = true;
    program.noFilePrompt = true;
  }

  if(string system; arguments.take("--system", system)) {
    program.startSystem = system;
  }

  if(arguments.take("--no-file-prompt")) {
    program.noFilePrompt = true;
  }

  if(arguments.take("--play-mode")) {
    ares::ui::playMode = true;
  }

  settings.filePath = locate("settings.bml");
  if(string settingsFile; arguments.take("--settings-file", settingsFile)) {
    settings.filePath = settingsFile;
  }

  if(string savestate; arguments.take("--save-state", savestate)) {
    if(savestate.length() == 1 && savestate[0] >= '1' && savestate[0] <= '9') {
      program.startSaveStateSlot = savestate;
    }
  }

  // --dump-log <targets:after[:count]>: headless dump of the N64 RSP/RDP command
  // logs to stdout, exactly as the viewer windows show them. After <after>
  // presented frames, dump <count> frames (default 1) of the selected log(s),
  // then quit. <targets> contains "rsp", "rdp", or both (e.g. "rsp+rdp", "all").
  if(string spec; arguments.take("--dump-log", spec)) {
    auto parts = nall::split(spec, ":");
    if(parts.size() >= 2) {
      auto targets = parts[0];
      ares::ui::logDump.rsp = (bool)targets.ifind("rsp") || targets == "all";
      ares::ui::logDump.rdp = (bool)targets.ifind("rdp") || targets == "all";
      ares::ui::logDump.startFrame = (u32)parts[1].natural();
      ares::ui::logDump.frameCount = parts.size() >= 3 ? max(1u, (u32)parts[2].natural()) : 1u;
    }
    if(!ares::ui::logDump.active()) {
      print("Invalid --dump-log spec: ", spec, "\n");
      print("Expected: --dump-log <rsp|rdp|rsp+rdp>:<after-frames>[:<frame-count>]\n");
      return;
    }
  }

  inputManager.create();
  Emulator::construct();

  settings.load();
  struct CliSettingOverride {
    string path;
    string originalValue;
    string overriddenValue;
  };
  std::vector<CliSettingOverride> cliSettingOverrides;

  if(arguments.find("--setting")) {
    string settingValue;
    while(arguments.take("--setting", settingValue)) {
      auto kv = nall::split(settingValue, "=", 1L);
      if(kv.size() == 2) {
        auto path = kv[0];
        auto node = settings[kv[0]];
        if(node) {
          bool found = false;
          for(auto& override : cliSettingOverrides) {
            if(override.path != path) continue;
            override.overriddenValue = kv[1];
            found = true;
            break;
          }
          if(!found) {
            cliSettingOverrides.push_back({path, node.value(), kv[1]});
          }
          node.setValue(kv[1]);
        } else {
          print("Invalid setting: ", settingValue, "\n");
          return;
        }
      } else {
        print("Invalid setting: ", settingValue, "\n");
        return;
      }
    }
    settings.process(true);
  }

  if(program.noFilePrompt) settings.general.noFilePrompt = true;

  if(arguments.take("--help")) {
    print("\n Usage: ares [OPTIONS]... game(s)\n\n");
    print("Options:\n");
    print("  --help                Displays available options and exit\n");
    print("  --version             Displays the version string of the application\n");
#if defined(PLATFORM_WINDOWS)
    print("  --terminal            Create new terminal window\n");
#endif
    print("  --fullscreen          Start in full screen mode\n");
    print("  --pseudofullscreen    Start in psuedo full screen mode\n");
    print("  --kiosk               Start in minimal UI mode (implies --no-file-prompt)\n");
    print("  --system name         Specify the system name\n");
    print("  --setting name=value  Specify a value for a setting\n");
    print("  --dump-all-settings   Show a list of all existing settings and exit\n");
    print("  --no-file-prompt      Do not prompt to load (optional) additional roms (eg: 64DD)\n");
    print("  --play-mode           Start in play mode: hide all UI, game output only (toggle via hotkey)\n");
    print("  --settings-file path  Specify a settings file override (settings.bml)\n");
    print("  --save-state slot     Specify a save state slot to load (1-9)\n");
    print("  --dump-log spec       Dump the N64 RSP/RDP command log to stdout, then quit.\n");
    print("                        spec = <rsp|rdp|rsp+rdp>:<after-frames>[:<frame-count>]\n");
    print("                        e.g. --dump-log rsp+rdp:120:3\n");
    print("\n");
    print("Available Systems:\n");
    print("  ");
    for(auto& emulator : emulators) {
      print(emulator->name, ", ");
    }
    print("\n\nares version ", ares::Version, "\n");
    return;
  }

  if(arguments.take("--version")) {
    print("\n", ares::Version, "\n");
    return;
  }

  if(arguments.take("--dump-all-settings")) {
    std::function<void(const Markup::Node&, string)> dump;
    dump = [&](const Markup::Node& node, string prefix) -> void {
      for(const auto& setting : node) {
        print(prefix, setting.name(), "\n");
        dump(setting, string(prefix, setting.name(), "/"));
      }
    };
    dump(settings, "");
    return;
  }

  program.startGameLoad.clear();
  std::vector<string> invalidKioskPaths;
  for(auto argument : arguments) {
    if(file::exists(argument) || directory::exists(argument)) {
      program.startGameLoad.push_back(argument);
    } else if(program.kiosk) {
      invalidKioskPaths.push_back(argument);
    }
  }

  if(program.kiosk) {
    if(!invalidKioskPaths.empty()) {
      program.error({"path does not exist: ", invalidKioskPaths.front()});
      return;
    }
    if(program.startGameLoad.empty()) {
      program.error("provide a valid game file or directory.");
      return;
    }
  }

  if(program.startSystem && !program.startGameLoad.empty()) {
    bool foundSystem = false;
    for(auto& emulator : emulators) {
      if(emulator->name == program.startSystem) {
        foundSystem = true;
        break;
      }
    }
    if(!foundSystem) {
      auto text = string{"Unrecognized argument for --system: ", program.startSystem, "\n"
                         "Use --help to list all valid systems supported by ares."};
      program.error(text);
      if(program.kiosk) return;
    }
  }

  if(!AresApp::initialize()) {
    print("Failed to initialize SDL3+ImGui\n");
    return;
  }

  AresApp::videoContext = {AresApp::window, AresApp::gpu};

  program._videoContext = (uintptr)&AresApp::videoContext;
  program.create();

  ares::ui::showAudioViewer = settings.general.showAudioViewer;
  ares::ui::showManifestViewer = settings.general.showManifestViewer;
  ares::ui::showCheatEditor = settings.general.showCheatEditor;
  ares::ui::showTracerViewer = settings.general.showTracerViewer;
  ares::ui::showRdpViewer = settings.general.showRdpViewer;
  ares::ui::showRspViewer = settings.general.showRspViewer;
  ares::ui::showCpuProfiler = settings.general.showCpuProfiler;
  ares::ui::showFlameChart = settings.general.showFlameChart;
  ares::ui::showFramebufferViewer = settings.general.showFramebufferViewer;
  ares::ui::showTmemViewer = settings.general.showTmemViewer;
  ares::ui::showMemoryViewer = settings.general.showMemoryViewer;

  AresApp::onMain = [=] {
    ruby::Input::setKeyboardCaptured(ImGui::GetIO().WantCaptureKeyboard);
    program.main();

    if(ares::ui::playMode) {
      ares::ui::DrawPlayMode();
      return;
    }

    ares::ui::DrawMenuBar();
    ares::ui::DrawViewport();
    ares::ui::DrawSettingsWindow();
    ares::ui::DrawManifestViewer();
    ares::ui::DrawCheatEditor();
    ares::ui::DrawTracerViewer();
    ares::ui::DrawRdpViewer();
    ares::ui::DrawRspViewer();
    ares::ui::DrawCpuProfiler();
    ares::ui::DrawFlameChart();
    ares::ui::DrawFramebufferViewer();
    ares::ui::DrawTmemViewer();
    ares::ui::DrawMemoryViewer();
    ares::ui::DrawAudioViewer();

    if(ares::ui::showAboutDialog) {
      ImGui::OpenPopup("About ares");
      ares::ui::showAboutDialog = false;
    }
    if(ImGui::BeginPopupModal("About ares", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {

      auto link = [](const char* url) {
        const ImU32 col = IM_COL32(96, 165, 250, 255);
        ImGui::PushStyleColor(ImGuiCol_Text, col);
        ImGui::TextUnformatted(url);
        ImGui::PopStyleColor();
        if(ImGui::IsItemHovered()) {
          ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
          ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
          ImGui::GetWindowDrawList()->AddLine(ImVec2(mn.x, mx.y), ImVec2(mx.x, mx.y), col);
        }
        if(ImGui::IsItemClicked()) SDL_OpenURL(url);
      };

      ImGui::TextUnformatted(ares::Name);
      ImGui::Separator();
      ImGui::TextUnformatted(string{"Version: ", ares::Version}.data());
      ImGui::TextUnformatted(ares::Copyright);
      ImGui::Spacing();

      ImGui::TextUnformatted("Upstream project:");
      link("https://github.com/ares-emulator/ares");

      ImGui::SeparatorText("This fork (ares-64)");
      ImGui::TextUnformatted("Added N64 debugging tools, maintained by Max Bebök.");
      link("https://github.com/HailToDodongo/ares-64");

      ImGui::Spacing();
      if(ImGui::Button("Close")) ImGui::CloseCurrentPopup();
      ImGui::EndPopup();
    }
  };

  AresApp::run();

  program.quit();
  ares::ui::SyncWindowVisibility();
  settings.save();
  AresApp::shutdown();
}


#if defined(PLATFORM_WINDOWS) && defined(ARCHITECTURE_AMD64) && !defined(BUILD_LOCAL)

#include <nall/windows/windows.hpp>
#include <intrin.h>

//this code must run before C++ global initializers
//it works with any valid combination of GCC, Clang, or MSVC and MingW or MSVCRT
//ref: https://learn.microsoft.com/en-us/cpp/c-runtime-library/crt-initialization

auto preCppInitializer() -> int {
  int data[4] = {};
  __cpuid(data, 1);
  bool sse42 = data[2] & 1 << 20;
  if(!sse42) FatalAppExitA(0, "This build of ares requires a CPU that supports SSE4.2.");
  return 0;
}

extern "C" {
#if defined(_MSC_VER)
  #pragma comment(linker, "/include:preCppInitializerEntry")
  #pragma section(".CRT$XCT", read)
  __declspec(allocate(".CRT$XCT"))
#else
  __attribute__((section (".CRT$XCT"), used))
#endif
  decltype(&preCppInitializer) preCppInitializerEntry = preCppInitializer;
}

#endif
