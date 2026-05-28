#include "ui.hpp"

#include "../desktop-ui.hpp"
#include "../application/application.hpp"

namespace ares::ui {

static int activePanel = 0;
static const char* panelNames[] = {
  "Video", "Audio", "Paths",
  "Options", "Debug", "Emulators", "Firmware",
  "Input", "Hotkeys", "Import/Export"
};
static constexpr int panelCount = sizeof(panelNames) / sizeof(panelNames[0]);

// --- Video panel ---

static void DrawVideoPanel() {
  ImGui::SeparatorText("Color Adjustment");
  float lum = settings.video.luminance;
  if(ImGui::SliderFloat("Luminance", &lum, 0.0f, 2.0f, "%.2f")) settings.video.luminance = lum;
  float sat = settings.video.saturation;
  if(ImGui::SliderFloat("Saturation", &sat, 0.0f, 2.0f, "%.2f")) settings.video.saturation = sat;
  float gam = settings.video.gamma;
  if(ImGui::SliderFloat("Gamma", &gam, 0.0f, 2.0f, "%.2f")) settings.video.gamma = gam;

  ImGui::SeparatorText("Emulator Settings");
  ImGui::Checkbox("Color Bleed", &settings.video.colorBleed);
  ImGui::Checkbox("Color Emulation", &settings.video.colorEmulation);
  ImGui::Checkbox("Deep Black Boost", &settings.video.deepBlackBoost);
  ImGui::Checkbox("Interframe Blending", &settings.video.interframeBlending);
  ImGui::Checkbox("Overscan", &settings.video.overscan);
  ImGui::Checkbox("Pixel Accuracy", &settings.video.pixelAccuracy);

  ImGui::SeparatorText("Output");
  const char* qualities[] = {"SD", "HD", "UHD"};
  int qualityIdx = 0;
  if(settings.video.quality == "HD") qualityIdx = 1;
  if(settings.video.quality == "UHD") qualityIdx = 2;
  if(ImGui::Combo("Quality", &qualityIdx, qualities, 3)) {
    settings.video.quality = qualities[qualityIdx];
  }
  ImGui::Checkbox("Supersampling", &settings.video.supersampling);
  ImGui::Checkbox("Disable VI Processing", &settings.video.disableVideoInterfaceProcessing);
  ImGui::Checkbox("Weave Deinterlacing", &settings.video.weaveDeinterlacing);
}

// --- Audio panel ---

static void DrawAudioPanel() {
  ImGui::SeparatorText("Volume");
  float vol = settings.audio.volume;
  if(ImGui::SliderFloat("Volume", &vol, 0.0f, 2.0f, "%.2f")) settings.audio.volume = vol;
  float bal = settings.audio.balance;
  if(ImGui::SliderFloat("Balance", &bal, -1.0f, 1.0f, "%.2f")) settings.audio.balance = bal;

  ImGui::SeparatorText("Device");
  ImGui::Checkbox("Exclusive Mode", &settings.audio.exclusive);
  ImGui::Checkbox("Blocking", &settings.audio.blocking);
  ImGui::Checkbox("Dynamic Rate", &settings.audio.dynamic);
}

// --- Paths panel ---

static void DrawPathsPanel() {
  ImGui::SeparatorText("Paths");
  static char home[1024] = {};
  static char firmware[1024] = {};
  static char saves[1024] = {};
  static bool pathsInit = false;
  if(!pathsInit) {
    memory::copy(home, settings.paths.home.data(), min(1023u, settings.paths.home.length()));
    memory::copy(firmware, settings.paths.firmware.data(), min(1023u, settings.paths.firmware.length()));
    memory::copy(saves, settings.paths.saves.data(), min(1023u, settings.paths.saves.length()));
    pathsInit = true;
  }

  if(ImGui::InputText("Home", home, sizeof(home))) settings.paths.home = home;
  if(ImGui::InputText("Firmware", firmware, sizeof(firmware))) settings.paths.firmware = firmware;
  if(ImGui::InputText("Saves", saves, sizeof(saves))) settings.paths.saves = saves;
}

// --- Options panel ---

static void DrawOptionsPanel() {
  ImGui::SeparatorText("Emulator Options");

  if(ImGui::Checkbox("Rewind", &settings.general.rewind)) {
    program.rewindReset();
  }
  ImGui::SameLine();
  ImGui::TextDisabled("(?)");
  if(ImGui::IsItemHovered()) ImGui::SetTooltip("Allows you to reverse time via the rewind hotkey");

  bool runAhead = settings.general.runAhead;
  if(ImGui::Checkbox("Run-Ahead", &runAhead)) {
    settings.general.runAhead = runAhead;
    program.runAheadUpdate();
  }
  ImGui::SameLine();
  ImGui::TextDisabled("(?)");
  if(ImGui::IsItemHovered()) ImGui::SetTooltip("Removes one frame of input lag, but doubles system requirements");

  ImGui::Checkbox("Auto-Save Memory Periodically", &settings.general.autoSaveMemory);
  ImGui::SameLine();
  ImGui::TextDisabled("(?)");
  if(ImGui::IsItemHovered()) ImGui::SetTooltip("Helps safeguard game saves from being lost");

  ImGui::Checkbox("Homebrew Development Mode", &settings.general.homebrewMode);
  ImGui::Checkbox("Force Interpreter", &settings.general.forceInterpreter);
  ImGui::Checkbox("Disable requests for loading additional media", &settings.general.noFilePrompt);

  if(ImGui::CollapsingHeader("Nintendo 64 Settings")) {
    ImGui::Checkbox("4MB Expansion Pak", &settings.nintendo64.expansionPak);

    const char* pakSizes[] = {"32KiB (Default)", "128KiB (Datel 1Meg)", "512KiB (Datel 4Meg)", "1984KiB (Maximum)"};
    int pakIdx = 0;
    if(settings.nintendo64.controllerPakBankString == "128KiB (Datel 1Meg)") pakIdx = 1;
    else if(settings.nintendo64.controllerPakBankString == "512KiB (Datel 4Meg)") pakIdx = 2;
    else if(settings.nintendo64.controllerPakBankString == "1984KiB (Maximum)") pakIdx = 3;

    if(ImGui::Combo("Controller Pak Size", &pakIdx, pakSizes, 4)) {
      settings.nintendo64.controllerPakBankString = pakSizes[pakIdx];
      if(pakIdx == 0) settings.nintendo64.controllerPakBankCount = 1;
      else if(pakIdx == 1) settings.nintendo64.controllerPakBankCount = 4;
      else if(pakIdx == 2) settings.nintendo64.controllerPakBankCount = 16;
      else settings.nintendo64.controllerPakBankCount = 62;
    }
  }

  if(ImGui::CollapsingHeader("Game Boy Advance Settings")) {
    ImGui::Checkbox("Game Boy Player", &settings.gameBoyAdvance.player);
  }

  if(ImGui::CollapsingHeader("Mega Drive Settings")) {
    ImGui::Checkbox("TMSS Boot Rom", &settings.megadrive.tmss);
  }
}

// --- Debug panel ---

static void DrawDebugPanel() {
  ImGui::SeparatorText("GDB-Server");

  static char portStr[16] = {};
  if(portStr[0] == 0) {
    snprintf(portStr, sizeof(portStr), "%d", (int)settings.debugServer.port);
  }

  if(ImGui::InputText("Port", portStr, sizeof(portStr))) {
    settings.debugServer.port = nall::string(portStr).integer();
    char check[16];
    snprintf(check, sizeof(check), "%d", (int)settings.debugServer.port);
    if(nall::string(check) != portStr) {
      snprintf(portStr, sizeof(portStr), "%d", (int)settings.debugServer.port);
    }
  }
  ImGui::SameLine();
  ImGui::TextDisabled("(?)");
  if(ImGui::IsItemHovered()) ImGui::SetTooltip("Safe range: 1024 - 32767");

  if(ImGui::Checkbox("Use IPv4", &settings.debugServer.useIPv4)) {
    nall::GDB::server.close();
    if(settings.debugServer.enabled) {
      nall::GDB::server.open(settings.debugServer.port, settings.debugServer.useIPv4);
    }
  }

  if(ImGui::Checkbox("Enabled", &settings.debugServer.enabled)) {
    nall::GDB::server.close();
    if(settings.debugServer.enabled) {
      nall::GDB::server.open(settings.debugServer.port, settings.debugServer.useIPv4);
    }
  }

  if(settings.debugServer.enabled) {
    ImGui::TextWrapped("%s", settings.debugServer.useIPv4
      ? "Note: IPv4 mode binds to any device, enabling anyone in your network to access this server"
      : "Note: localhost only (for Windows/WSL: please use IPv4 instead)");
  }
}

// --- Emulators panel ---

static void DrawEmulatorsPanel() {
  ImGui::SeparatorText("Load Menu Emulators");

  if(!ImGui::BeginTable("emulators", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) return;
  ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 24.0f);
  ImGui::TableSetupColumn("Name");
  ImGui::TableSetupColumn("Manufacturer");
  ImGui::TableHeadersRow();

  for(auto& emulator : emulators) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    bool visible = emulator->configuration.visible;
    ImGui::PushID(emulator->name.data());
    if(ImGui::Checkbox("##visible", &visible)) {
      emulator->configuration.visible = visible;
      if(!program._imguiMode) presentation.loadEmulators();
    }
    ImGui::PopID();
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(emulator->name.data());
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(emulator->manufacturer.data());
  }
  ImGui::EndTable();
}

// --- Firmware panel ---

static void DrawFirmwarePanel() {
  ImGui::SeparatorText("BIOS Firmware Locations");

  static int selectedFirmware = -1;

  if(!ImGui::BeginTable("firmware", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, ImVec2(0, -40))) return;
  ImGui::TableSetupColumn("Emulator");
  ImGui::TableSetupColumn("Type");
  ImGui::TableSetupColumn("Region");
  ImGui::TableSetupColumn("Location");
  ImGui::TableHeadersRow();

  int idx = 0;
  for(auto& emulator : emulators) {
    for(auto& fw : emulator->firmware) {
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      if(ImGui::Selectable(emulator->name.data(), selectedFirmware == idx, ImGuiSelectableFlags_SpanAllColumns)) {
        selectedFirmware = idx;
      }
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(fw.type.data());
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(fw.region.data());
      ImGui::TableNextColumn();
      if(file::exists(fw.location)) {
        ImGui::TextUnformatted(fw.location.data());
      } else {
        ImGui::TextDisabled("(unset)");
      }
      idx++;
    }
  }
  ImGui::EndTable();

  bool hasSelection = selectedFirmware >= 0;
  if(ImGui::Button("Assign") && hasSelection) {
    idx = 0;
    for(auto& emulator : emulators) {
      for(auto& fw : emulator->firmware) {
        if(idx == selectedFirmware) {
          auto title = string{"Select ", emulator->name, " ", fw.type, " (", fw.region, ")"};
          auto pathStr = ares::ui::openFileDialog(title.data());
          if(pathStr) {
            fw.location = pathStr;
          }
        }
        idx++;
      }
    }
  }
  ImGui::SameLine();
  if(ImGui::Button("Clear") && hasSelection) {
    idx = 0;
    for(auto& emulator : emulators) {
      for(auto& fw : emulator->firmware) {
        if(idx == selectedFirmware) fw.location = "";
        idx++;
      }
    }
  }
  ImGui::SameLine();
  if(ImGui::Button("Scan")) {
    for(auto& emulator : emulators) {
      for(auto& fw : emulator->firmware) {
        if(!file::exists(fw.location)) {
          auto firmwarePath = settings.paths.firmware ? settings.paths.firmware : locate("Firmware/");
          if(!directory::exists(firmwarePath)) continue;
          for(auto& filename : directory::files(firmwarePath)) {
            auto location = string{firmwarePath, filename};
            if(file::size(location) >= 10_MiB) continue;
            auto digest = Hash::SHA256(file::read(location)).digest();
            if(digest == fw.sha256) { fw.location = location; break; }
          }
        }
      }
    }
  }
}

// --- Input panel ---

static int inputSystemIdx = 0;
static int inputPortIdx = 0;
static int inputDeviceIdx = 0;
static std::shared_ptr<InputMapping> activeMapping;
static int activeBinding = -1;

static void DrawInputPanel() {
  // System/Port/Device selectors
  auto& ports = Emulator::enumeratePorts("Virtual Gamepads");
  if(inputSystemIdx == 0) {
    // Virtual Gamepads
  } else {
    // Emulator-specific ports
    int emuIdx = 1;
    for(auto& emulator : emulators) {
      if(emuIdx == inputSystemIdx) {
        ports = Emulator::enumeratePorts(emulator->name);
        break;
      }
      emuIdx++;
    }
  }

  if(ImGui::BeginCombo("System", inputSystemIdx == 0 ? "Virtual Gamepads" : (inputSystemIdx > 0 && inputSystemIdx <= (int)emulators.size() ? emulators[inputSystemIdx - 1]->name.data() : "Virtual Gamepads"))) {
    if(ImGui::Selectable("Virtual Gamepads", inputSystemIdx == 0)) {
      inputSystemIdx = 0; inputPortIdx = 0; inputDeviceIdx = 0;
    }
    for(u32 i = 0; i < emulators.size(); i++) {
      if(ImGui::Selectable(emulators[i]->name.data(), inputSystemIdx == (int)i + 1)) {
        inputSystemIdx = i + 1; inputPortIdx = 0; inputDeviceIdx = 0;
      }
    }
    ImGui::EndCombo();
  }

  if(ports.size() > 0) {
    if(inputPortIdx >= (int)ports.size()) inputPortIdx = 0;
    if(ImGui::BeginCombo("Port", ports[inputPortIdx].name.data())) {
      for(u32 i = 0; i < ports.size(); i++) {
        if(ImGui::Selectable(ports[i].name.data(), inputPortIdx == (int)i)) {
          inputPortIdx = i; inputDeviceIdx = 0;
        }
      }
      ImGui::EndCombo();
    }

    auto& port = ports[inputPortIdx];
    if(port.devices.size() > 0) {
      if(inputDeviceIdx >= (int)port.devices.size()) inputDeviceIdx = 0;
      if(ImGui::BeginCombo("Device", port.devices[inputDeviceIdx].name.data())) {
        for(u32 i = 0; i < port.devices.size(); i++) {
          if(ImGui::Selectable(port.devices[i].name.data(), inputDeviceIdx == (int)i)) {
            inputDeviceIdx = i;
          }
        }
        ImGui::EndCombo();
      }

      auto& device = port.devices[inputDeviceIdx];

      if(!ImGui::BeginTable("inputMappings", 1 + BindingLimit, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) return;
      ImGui::TableSetupColumn("Input");
      for(u32 b = 0; b < BindingLimit; b++) {
        auto label = string{"Mapping #", 1 + b};
        ImGui::TableSetupColumn(label.data());
      }
      ImGui::TableHeadersRow();

      bool assigning = inputAssign.waiting;
      for(u32 idx = 0; idx < device.inputs.size(); idx++) {
        auto& input = device.inputs[idx];
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(input.name.data());

        for(u32 b = 0; b < BindingLimit; b++) {
          ImGui::TableNextColumn();
          auto& mapping = input.effectiveMapping();
          auto text = mapping.bindings[b].text();

          bool isActive = assigning && inputAssign.activeNode == &device.inputs[idx]
                          && inputAssign.activeBinding == (int)b;
          char label[64];
          if(isActive) {
            snprintf(label, sizeof(label), "(press key...)##%u_%u", idx, b);
          } else if(text) {
            snprintf(label, sizeof(label), "%s##%u_%u", text.data(), idx, b);
          } else {
            snprintf(label, sizeof(label), "##%u_%u", idx, b);
          }

          if(ImGui::Selectable(label, false, ImGuiSelectableFlags_AllowDoubleClick)) {
            if(ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) || !text) {
              inputAssign.activeNode = &device.inputs[idx];
              inputAssign.activeBinding = b;
              inputAssign.waiting = true;
              inputManager.poll(true);
            }
          }
          if(ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            // Right-click to clear
            mapping.unbind(b);
          }
        }
      }
      if(assigning && ImGui::Button("Cancel Assignment")) {
        inputAssign.activeNode = nullptr;
        inputAssign.waiting = false;
        inputAssign.activeBinding = -1;
      }
      ImGui::EndTable();
    }
  }
}

// --- Hotkeys panel ---

static void DrawHotkeysPanel() {
  if(!ImGui::BeginTable("hotkeys", 1 + BindingLimit, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) return;
  ImGui::TableSetupColumn("Hotkey");
  for(u32 b = 0; b < BindingLimit; b++) {
    auto label = string{"Mapping #", 1 + b};
    ImGui::TableSetupColumn(label.data());
  }
  ImGui::TableHeadersRow();

  bool assigning = inputAssign.waiting;

  for(auto& mapping : inputManager.hotkeys) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(mapping.name.data());

    for(u32 b = 0; b < BindingLimit; b++) {
      ImGui::TableNextColumn();
      auto text = mapping.bindings[b].text();

      bool isActive = assigning && inputAssign.activeMapping == &mapping
                      && inputAssign.activeBinding == (int)b;
      char label[64];
      if(isActive) {
        snprintf(label, sizeof(label), "(press key...)##hk_%p_%u", (void*)&mapping, b);
      } else if(text) {
        snprintf(label, sizeof(label), "%s##hk_%p_%u", text.data(), (void*)&mapping, b);
      } else {
        snprintf(label, sizeof(label), "##hk_%p_%u", (void*)&mapping, b);
      }

      if(ImGui::Selectable(label, false, ImGuiSelectableFlags_AllowDoubleClick)) {
        if(ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) || !text) {
          inputAssign.activeMapping = &mapping;
          inputAssign.activeBinding = b;
          inputAssign.activeNode = nullptr;
          inputAssign.waiting = true;
          inputManager.poll(true);
        }
      }
      if(ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        mapping.unbind(b);
      }
    }
  }
  if(assigning && inputAssign.activeMapping && ImGui::Button("Cancel Assignment")) {
    inputAssign.activeMapping = nullptr;
    inputAssign.waiting = false;
    inputAssign.activeBinding = -1;
  }
  ImGui::EndTable();
}

// --- Import/Export panel ---

static void DrawImportExportPanel() {
  ImGui::SeparatorText("Settings File");
  ImGui::TextUnformatted("Current settings file:");
  ImGui::TextWrapped("%s", settings.filePath.data());

  ImGui::Spacing();
  if(ImGui::Button("Export Settings")) {
    auto path = ares::ui::saveFileDialog("Export Settings");
    if(path) {
      file::copy(settings.filePath, path);
    }
  }
  ImGui::SameLine();
  if(ImGui::Button("Import Settings")) {
    auto path = ares::ui::openFileDialog("Import Settings");
    if(path) {
      settings.filePath = path;
      settings.load();
      // Reconfigure for imgui mode
      if(program._imguiMode) {
        settings.input.driver = "SDL3";
      }
    }
  }
}

// --- Main settings window ---

auto DrawSettingsWindow() -> void {
  if(!showSettingsWindow) return;

  ImGui::SetNextWindowSize(ImVec2(680, 480), ImGuiCond_FirstUseEver);
  if(!ImGui::Begin("Settings", &showSettingsWindow)) {
    ImGui::End();
    return;
  }

  ImGui::BeginChild("panels", ImVec2(140, 0), ImGuiChildFlags_Borders);
  for(int i = 0; i < panelCount; i++) {
    if(ImGui::Selectable(panelNames[i], activePanel == i)) {
      activePanel = i;
    }
  }
  ImGui::EndChild();

  ImGui::SameLine();

  ImGui::BeginChild("content", ImVec2(0, 0));
  switch(activePanel) {
    case 0: DrawVideoPanel(); break;
    case 1: DrawAudioPanel(); break;
    case 2: DrawPathsPanel(); break;
    case 3: DrawOptionsPanel(); break;
    case 4: DrawDebugPanel(); break;
    case 5: DrawEmulatorsPanel(); break;
    case 6: DrawFirmwarePanel(); break;
    case 7: DrawInputPanel(); break;
    case 8: DrawHotkeysPanel(); break;
    case 9: DrawImportExportPanel(); break;
  }
  ImGui::EndChild();

  ImGui::End();
}

}  // namespace ares::ui
