auto Program::identify(const string& filename) -> std::shared_ptr<Emulator> {
  Program::Guard guard;
using namespace hiro;

  std::vector<std::shared_ptr<Emulator>> matches;

  for(auto& system : mia::identify(filename)) {
    for(auto& emulator : emulators) {
      if(!emulator->configuration.visible) continue;
      if(emulator->name != system) continue;

      if(std::ranges::find(matches, emulator) == matches.end()) {
        matches.push_back(emulator);
      }
    }
  }

  if(matches.size() == 1)  return matches.front();

  if(matches.size() > 1) {
    if(kiosk) {
      error({"Multiple possible game types detected for: ", Location::file(filename), "\n\n",});
      return {};
    }

    std::vector<string> buttons;
    for(auto& emulator : matches) buttons.push_back(emulator->name);
    buttons.push_back("Cancel");

    string choice;
    if(_imguiMode) {
      // In ImGui mode, just pick the first match
      return matches.front();
    } else {
      choice = MessageDialog().setTitle(ares::Name).setText({
        "Filename: ", Location::file(filename), "\n\n",
        "Multiple possible systems were detected.\n",
        "Please choose which system to launch this file with."
      }).setAlignment(presentation).question(buttons);
    }

    for(auto& emulator : matches) {
      if(emulator->name == choice) return emulator;
    }

    return {};
  }

  // No matches → existing error path
  if(kiosk || _imguiMode) {
    error({"unable to determine game type for: ", Location::file(filename)});
  } else {
    MessageDialog().setTitle(ares::Name).setText({
      "Filename: ", Location::file(filename), "\n\n"
      "Unable to determine what type of game this file is.\n"
      "Please use the load menu to choose the appropriate game system instead."
    }).setAlignment(presentation).error();
  }

  return {};
}

/// Loads an emulator and, optionally, a ROM from the given location.
auto Program::load(std::shared_ptr<Emulator> emulator, string location) -> bool {
  Program::Guard guard;
  unload();
  ::emulator = emulator;

  if(emulator->arcade() && !location) {
    gameBrowserWindow.show(emulator);
    ::emulator.reset();
    return false;
  }

  auto r = load(location);
  return r;
}

/// Loads a ROM for an already-loaded emulator.
auto Program::load(string location) -> bool {
  Program::Guard guard;
  if(settings.debugServer.enabled) {
    nall::GDB::server.reset();
  }

  if(!emulator->load(location)) {
    emulator.reset();
    if(!_imguiMode && settings.video.adaptiveSizing) presentation.resizeWindow();
    if(!_imguiMode) presentation.showIcon(true);
    return false;
  }
  location = emulator->game->location;

  string savesPath = settings.paths.saves;
  if(!savesPath) savesPath = Location::path(location);
  if(!directory::writable(savesPath)) {
    if(kiosk || _imguiMode) {
      showMessage({
        "Current save path is read-only; progress may be lost. Save location: ", savesPath
      });
    } else {
      MessageDialog().setTitle(ares::Name).setText({
        "The current save path is read-only; please choose a writable save path now.\n"
        "Otherwise, any in-game progress will be lost once this game is unloaded!\n\n"
        "Current save location: ", savesPath
      }).warning();
    }
  }

  runAheadUpdate();

  // Auto-detect RSPQ from ELF alongside the ROM
  if(emulator && emulator->name == "Nintendo 64") {
    ares::Nintendo64::rsp.capture.autoDetect(location);

    if(ares::ui::logDump.active()) {
      ares::Nintendo64::rsp.capture.enabled.store(true, std::memory_order_release);
      ares::Nintendo64::rdp.capture.enabled.store(true, std::memory_order_release);
    }
    ares::Nintendo64::cpu.profiler.loadSymbols(location);
  }

  if(!_imguiMode) {
    presentation.loadEmulator();
    presentation.showIcon(false);
    if(settings.video.adaptiveSizing && !startPseudoFullScreen) presentation.resizeWindow();
  }
  if(!_imguiMode && toolsWindowConstructed) {
    manifestViewer.reload();
    cheatEditor.reload();
    memoryEditor.reload();
    graphicsViewer.reload();
    streamManager.reload();
    propertiesViewer.reload();
    traceLogger.reload();
    tapeViewer.reload();
  }
  state = {};
  if(settings.boot.debugger) {
    pause(true);
    if(!_imguiMode && toolsWindowConstructed) toolsWindow.show("Tracer");
    if(!_imguiMode) presentation.setFocused();
  } else if (settings.boot.awaitGDBClient) {
    pause(true);
  } else {
    pause(false);
  }

  showMessage({"Loaded ", Location::prefix(location)});

  if(settings.debugServer.enabled) {
    nall::GDB::server.open(settings.debugServer.port, settings.debugServer.useIPv4);
    nall::GDB::server.onClientConnectCallback = []() {
      if (settings.boot.awaitGDBClient)
        program.pause(false);
    };
  }

  string recentEntry = {emulator->name, ";", location};
  s32 last = Settings::Recent::count - 1;
  for(s32 index = 0; index < (s32)Settings::Recent::count; index++) {
    if(settings.recent.game[index] == recentEntry) { last = index; break; }
  }
  for(s32 index = last - 1; index >= 0; index--) {
    settings.recent.game[index + 1] = settings.recent.game[index];
  }
  settings.recent.game[0] = recentEntry;
  if(!_imguiMode) presentation.loadEmulators();

  configuration = emulator->root->attribute("configuration");

  if(program.startSaveStateSlot) {
    if(stateLoad(program.startSaveStateSlot.integer())) {
      state.slot = program.startSaveStateSlot.integer();
    }
  }

  return true;
}

auto Program::unload() -> void {
  Program::Guard guard;
  if(!emulator) return;

  nall::GDB::server.close();
  nall::GDB::server.reset();

  ares::ui::SyncWindowVisibility();
  settings.save();
  clearUndoStates();
  showMessage({"Unloaded ", Location::prefix(emulator->game->location)});
  emulator->unload();
  screens.clear();
  streams.clear();
  emulator.reset();
  rewindReset();
  if(!_imguiMode) presentation.unloadEmulator();
  if(!_imguiMode && toolsWindowConstructed) {
    toolsWindow.setVisible(false);
    manifestViewer.unload();
    cheatEditor.unload();
    memoryEditor.unload();
    graphicsViewer.unload();
    streamManager.unload();
    propertiesViewer.unload();
    traceLogger.unload();
    tapeViewer.unload();
  }
  if(gameBrowserWindowConstructed) gameBrowserWindow.setVisible(false);
  message.text = "";
  configuration = "";
  ruby::video.clear();
  ruby::audio.clear();
}
