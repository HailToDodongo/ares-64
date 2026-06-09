#include "../desktop-ui.hpp"
#include <nall/vector-helpers.hpp>

Settings settings;
SettingsWindow settingsWindow;
VideoSettings& videoSettings = settingsWindow.videoSettings;
AudioSettings& audioSettings = settingsWindow.audioSettings;
InputSettings& inputSettings = settingsWindow.inputSettings;
HotkeySettings& hotkeySettings = settingsWindow.hotkeySettings;
EmulatorSettings& emulatorSettings = settingsWindow.emulatorSettings;
OptionSettings& optionSettings = settingsWindow.optionSettings;
FirmwareSettings& firmwareSettings = settingsWindow.firmwareSettings;
PathSettings& pathSettings = settingsWindow.pathSettings;
DebugSettings& debugSettings = settingsWindow.debugSettings;
DriverSettings& driverSettings = settingsWindow.driverSettings;
ImportExportSettings& importExportSettings = settingsWindow.importExportSettings;

auto Settings::load() -> void {
  Markup::Node::operator=(BML::unserialize(string::read(filePath), " "));
  process(true);
  save();
}

auto Settings::save() -> void {
  process(false);
  file::write(filePath, BML::serialize(*this, " "));
}

auto Settings::process(bool load) -> void {
  if(load) {
    //initialize non-static default settings
    video.driver = ruby::Video::optimalDriver();
    audio.driver = "SDL";
    input.driver = "SDL3";
  }

  #define bind(type, path, name) \
    if(load) { \
      if(auto node = operator[](path)) name = node.type(); \
    } else { \
      operator()(path).setValue(name); \
    } \

  bind(string,  "Video/Driver", video.driver);
  bind(string,  "Video/Monitor", video.monitor);
  bind(string,  "Video/Format", video.format);
  bind(boolean, "Video/Blocking", video.blocking);
  bind(boolean, "Video/Flush", video.flush);
  bind(natural, "Video/WindowWidth", video.windowWidth);
  bind(natural, "Video/WindowHeight", video.windowHeight);
  bind(string,  "Video/Output", video.output);
  bind(natural, "Video/FixedScale", video.fixedScale);
  bind(string,  "Video/AspectCorrectionMode", video.aspectCorrection);
  bind(boolean, "Video/AdaptiveSizing", video.adaptiveSizing);
  bind(boolean, "Video/AutoCentering", video.autoCentering);
  bind(string,  "Video/Renderer", video.renderer);
  bind(string,  "Video/Quality", video.quality);
  bind(boolean, "Video/Supersampling", video.supersampling);
  bind(boolean, "Video/DisableVideoInterfaceProcessing", video.disableVideoInterfaceProcessing);
  bind(boolean, "Video/WeaveDeinterlacing", video.weaveDeinterlacing);

  bind(natural, "Audio/Frequency", audio.frequency);
  bind(natural, "Audio/Latency", audio.latency);
  bind(boolean, "Audio/Blocking", audio.blocking);
  bind(boolean, "Audio/Dynamic", audio.dynamic);
  bind(boolean, "Audio/Mute", audio.mute);
  bind(real,    "Audio/Volume", audio.volume);
  bind(real,    "Audio/Balance", audio.balance);

  bind(string,  "Input/Defocus", input.defocus);

  bind(boolean, "Boot/Fast", boot.fast);
  bind(boolean, "Boot/Debugger", boot.debugger);
  bind(boolean, "Boot/AwaitGDBClient", boot.awaitGDBClient);
  bind(string,  "Boot/Prefer", boot.prefer);

  bind(boolean, "General/Rewind", general.rewind);
  bind(boolean, "General/RunAhead", general.runAhead);
  bind(boolean, "General/AutoSaveMemory", general.autoSaveMemory);
  bind(boolean, "General/HomebrewMode", general.homebrewMode);
  bind(boolean, "General/ForceInterpreter", general.forceInterpreter);
  bind(boolean, "General/NoFilePrompt", general.noFilePrompt);
  bind(boolean, "General/ShowAudioViewer", general.showAudioViewer);
  bind(boolean, "General/ShowManifestViewer", general.showManifestViewer);
  bind(boolean, "General/ShowCheatEditor", general.showCheatEditor);
  bind(boolean, "General/ShowTracerViewer", general.showTracerViewer);
  bind(boolean, "General/ShowRdpViewer", general.showRdpViewer);
  bind(boolean, "General/ShowRspViewer", general.showRspViewer);
  bind(boolean, "General/ShowCpuProfiler", general.showCpuProfiler);
  bind(boolean, "General/ShowFlameChart", general.showFlameChart);
  bind(boolean, "General/ShowFramebufferViewer", general.showFramebufferViewer);
  bind(boolean, "General/ShowTmemViewer", general.showTmemViewer);
  bind(boolean, "General/ShowMemoryViewer", general.showMemoryViewer);
  bind(boolean, "General/DpiOverride", general.dpiOverride);
  bind(natural, "General/DpiScalePercent", general.dpiScalePercent);

  bind(natural, "Rewind/Length", rewind.length);
  bind(natural, "Rewind/Frequency", rewind.frequency);

  bind(string,  "Paths/Home", paths.home);
  bind(string,  "Paths/Firmware", paths.firmware);
  bind(string,  "Paths/Saves", paths.saves);
  bind(string,  "Paths/Screenshots", paths.screenshots);
  bind(string,  "Paths/Debugging", paths.debugging);
  bind(string,  "Paths/ArcadeRoms", paths.arcadeRoms);
  bind(string,  "Paths/SuperFamicom/GameBoy", paths.superFamicom.gameBoy);
  bind(string,  "Paths/SuperFamicom/BSMemory", paths.superFamicom.bsMemory);
  bind(string,  "Paths/SuperFamicom/SufamiTurbo", paths.superFamicom.sufamiTurbo);

  bind(natural, "DebugServer/Port", debugServer.port);
  bind(boolean, "DebugServer/Enabled", debugServer.enabled);
  bind(boolean, "DebugServer/UseIPv4", debugServer.useIPv4);

  bind(boolean, "Nintendo64/ExpansionPak", nintendo64.expansionPak);
  bind(string, "Nintendo64/ControllerPakBankString", nintendo64.controllerPakBankString);

  bind(boolean, "GameBoyAdvance/Player", gameBoyAdvance.player);

  bind(boolean, "MegaDrive/TMSS", megadrive.tmss);

  for(u32 index : range(9)) {
    string name = {"Recent/Game-", 1 + index};
    bind(string, name, recent.game[index]);
  }

  for(u32 index : range(5)) {
    auto& port = virtualPorts[index];
    for(auto& input : port.pad.inputs) {
      string name = {"VirtualPad", 1 + index, "/", string{input.name}.replace(" ", ".").replace("(", ".").replace(")", "")}, value;
      if(load == 0) for(auto& assignment : input.mapping->assignments) value.append(assignment, ";");
      if(load == 0) value.trimRight(";", 1L);
      bind(string, name, value);
      if(load == 1) {
        auto parts = nall::split(value, ";");
        parts.resize(BindingLimit);
        for(u32 binding : range(BindingLimit)) input.mapping->assignments[binding] = parts[binding];
      }
    }
    for(auto& input : port.mouse.inputs) {
      string name = {"VirtualMouse", 1 + index, "/", input.name}, value;
      if(load == 0) for(auto& assignment : input.mapping->assignments) value.append(assignment, ";");
      if(load == 0) value.trimRight(";", 1L);
      bind(string, name, value);
      if(load == 1) {
        auto parts = nall::split(value, ";");
        parts.resize(BindingLimit);
        for(u32 binding : range(BindingLimit)) input.mapping->assignments[binding] = parts[binding];
      }
    }
  }

  for(auto& emulator : emulators) {
    string base = string{emulator->name}.replace(" ", "");
    base.replace("(", "").replace(")", "");
    for(auto& port : emulator->ports) {
      for(auto& device : port.devices) {
        if(!device.hasDirectMappings()) continue;
        string portName = string{port.name}.replace(" ", ".").replace("[", "").replace("]", "").replace("(", "").replace(")", "").replace("*", "Star").replace("#", "Pound");
        string deviceName = string{device.name}.replace(" ", ".").replace("[", "").replace("]", "").replace("(", "").replace(")", "").replace("*", "Star").replace("#", "Pound");
        for(auto& input : device.inputs) {
          auto& mapping = input.configuredMapping();
          string inputName = string{input.name}.replace(" ", ".").replace("[", "").replace("]", "").replace("(", ".").replace(")", "").replace("*", "Star").replace("#", "Pound");
          string name = {base, "/Input/", portName, "/", deviceName, "/", inputName}, value;
          if(load == 0) for(auto& assignment : mapping.assignments) value.append(assignment, ";");
          if(load == 0) value.trimRight(";", 1L);
          bind(string, name, value);
          if(load == 1) {
            auto parts = nall::split(value, ";");
            parts.resize(BindingLimit);
            for(u32 binding : range(BindingLimit)) mapping.assignments[binding] = parts[binding];
          }
        }
        for(auto& pair : device.pairs) {
          string pairName = string{pair.name}.replace(" ", ".").replace("[", "").replace("]", "").replace("(", ".").replace(")", "").replace("*", "Star").replace("#", "Pound");
          for(auto index : range(2)) {
            string suffix = index == 0 ? "Lo" : "Hi";
            auto& mapping = index == 0 ? pair.configuredMappingLo() : pair.configuredMappingHi();
            string name = {base, "/Input/", portName, "/", deviceName, "/", pairName, "/", suffix}, value;
            if(load == 0) for(auto& assignment : mapping.assignments) value.append(assignment, ";");
            if(load == 0) value.trimRight(";", 1L);
            bind(string, name, value);
            if(load == 1) {
              auto parts = nall::split(value, ";");
              parts.resize(BindingLimit);
              for(u32 binding : range(BindingLimit)) mapping.assignments[binding] = parts[binding];
            }
          }
        }
      }
    }
  }

  for(auto& mapping : inputManager.hotkeys) {
    string name = {"Hotkey/", string{mapping.name}.replace(" ", "")}, value;
    if(load == 0) for(auto& assignment : mapping.assignments) value.append(assignment, ";");
    if(load == 0) value.trimRight(";", 1L);
    bind(string, name, value);
    if(load == 1) {
      auto parts = nall::split(value, ";");
      parts.resize(BindingLimit);
      for(u32 binding : range(BindingLimit)) mapping.assignments[binding] = parts[binding];
    }
  }

  for(auto& emulator : emulators) {
    string base = string{emulator->name}.replace(" ", ""), name;
    base.replace("(", "").replace(")", "");
    name = {base, "/Visible"};
    bind(boolean, name, emulator->configuration.visible);
    name = {base, "/Path"};
    bind(string,  name, emulator->configuration.game);
    for(auto& firmware : emulator->firmware) {
      string name = {base, "/Firmware/", firmware.type, ".", firmware.region};
      name.replace(" ", "-");
      bind(string, name, firmware.location);
    }
  }

  #undef bind
}

