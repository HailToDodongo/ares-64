auto InputManager::createHotkeys() -> void {
  static bool fastForwardVideoBlocking;
  static bool fastForwardAudioBlocking;
  static bool fastForwardAudioDynamic;
  static bool toggleFastForwardState = false;

  hotkeys.push_back(InputHotkey("Toggle Fullscreen").onPress([&] {
    program.videoFullScreenToggle();
  }));

  hotkeys.push_back(InputHotkey("Toggle Pseudo-Fullscreen").onPress([&] {
    program.videoPseudoFullScreenToggle();
  }));

  hotkeys.push_back(InputHotkey("Toggle Mouse Capture").onPress([&] {
    Program::Guard guard;
    if(!emulator) return;
    if(!ruby::input.acquired()) {
      ruby::input.acquire();
    } else {
      ruby::input.release();
    }
  }));

  hotkeys.push_back(InputHotkey("Toggle Keyboard Capture").onPress([&] {
    Program::Guard guard;
    if(!emulator) return;
    program.keyboardCaptured = !program.keyboardCaptured;
    print("Keyboard capture: ", program.keyboardCaptured, "\n");
  }));

  hotkeys.push_back(InputHotkey("Fast Forward").onPress([&] {
    Program::Guard guard;
    if(!emulator || program.rewinding) return;
    if(!toggleFastForwardState) {
      program.fastForwarding = true;
      fastForwardVideoBlocking = ruby::video.blocking();
      fastForwardAudioBlocking = ruby::audio.blocking;
      fastForwardAudioDynamic  = ruby::audio.dynamic;
      ruby::video.setBlocking(false);
      ruby::audio.setBlocking(false);
      ruby::audio.setDynamic(false);
    }

    // Also break any active RSP/RDP step spin so the worker doesn't stay stuck.
    if(emulator->name == "Nintendo 64") {
      auto& rspCap = ares::Nintendo64::rsp.capture;
      auto& rdpCap = ares::Nintendo64::rdp.capture;
      if(rspCap.stepMode.load(std::memory_order_relaxed)) {
        rspCap.stepMode.store(false, std::memory_order_release);
        rspCap.stepPending.store(true, std::memory_order_release);
      }
      if(rdpCap.stepMode.load(std::memory_order_relaxed)) {
        rdpCap.stepMode.store(false, std::memory_order_release);
        rdpCap.stepPending.store(true, std::memory_order_release);
      }
    }
  }).onRelease([&] {
    Program::Guard guard;
    if(!emulator) return;
    if(!toggleFastForwardState) {
      program.fastForwarding = false;
      ruby::video.setBlocking(fastForwardVideoBlocking);
      ruby::audio.setBlocking(fastForwardAudioBlocking);
      ruby::audio.setDynamic(fastForwardAudioDynamic);
    }
  }));

  hotkeys.push_back(InputHotkey("Toggle Fast Forward").onPress([&] {
    Program::Guard guard;
    if(!emulator || program.rewinding) return;
    program.fastForwarding = !program.fastForwarding;

    if (program.fastForwarding) {
      toggleFastForwardState = true;
      fastForwardVideoBlocking = ruby::video.blocking();
      fastForwardAudioBlocking = ruby::audio.blocking;
      fastForwardAudioDynamic  = ruby::audio.dynamic;
      ruby::video.setBlocking(false);
      ruby::audio.setBlocking(false);
      ruby::audio.setDynamic(false);
    } else {
      toggleFastForwardState = false;
      ruby::video.setBlocking(fastForwardVideoBlocking);
      ruby::audio.setBlocking(fastForwardAudioBlocking);
      ruby::audio.setDynamic(fastForwardAudioDynamic);
    }

    // When toggling fast-forward, also break any active RSP/RDP step spin
    // so the worker doesn't stay stuck waiting for a step that won't come.
    if(emulator->name == "Nintendo 64") {
      auto& rspCap = ares::Nintendo64::rsp.capture;
      auto& rdpCap = ares::Nintendo64::rdp.capture;
      if(rspCap.stepMode.load(std::memory_order_relaxed)) {
        rspCap.stepMode.store(false, std::memory_order_release);
        rspCap.stepPending.store(true, std::memory_order_release);
      }
      if(rdpCap.stepMode.load(std::memory_order_relaxed)) {
        rdpCap.stepMode.store(false, std::memory_order_release);
        rdpCap.stepPending.store(true, std::memory_order_release);
      }
    }
  }));

  hotkeys.push_back(InputHotkey("Rewind").onPress([&] {
    Program::Guard guard;
    if(!emulator || program.fastForwarding) return;
    if(program.rewind.frequency == 0) {
      return program.showMessage("Please enable rewind support in the emulator settings first.");
    }
    program.rewinding = true;
    program.rewindSetMode(Program::Rewind::Mode::Rewinding);
  }).onRelease([&] {
    if(!emulator) return;
    program.rewinding = false;
    program.rewindSetMode(Program::Rewind::Mode::Playing);
  }));

  // Hold-to-repeat state for the Frame Advance hotkey
  stepHotkeyHeld = false;

  hotkeys.push_back(InputHotkey("Frame Advance").onPress([&] {
    if(!emulator) return;
    stepHotkeyHeld = true;
    stepHotkeyHoldStart = std::chrono::steady_clock::now();
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
  }).onRelease([&] {
    stepHotkeyHeld = false;
  }));

  hotkeys.push_back(InputHotkey("Capture Screenshot").onPress([&] {
    Program::Guard guard;
    if(!emulator) return;
    program.requestScreenshot = true;
  }));

  hotkeys.push_back(InputHotkey("Save State").onPress([&] {
    Program::Guard guard;
    if(!emulator) return;
    program.stateSave(program.state.slot);
  }));

  hotkeys.push_back(InputHotkey("Load State").onPress([&] {
    Program::Guard guard;
    if(!emulator) return;
    program.stateLoad(program.state.slot);
  }));

  hotkeys.push_back(InputHotkey("Decrement State Slot").onPress([&] {
    if(!emulator) return;
    if(program.state.slot == 1) program.state.slot = 9;
    else program.state.slot--;
    program.showMessage({"Selected state slot ", program.state.slot});
  }));

  hotkeys.push_back(InputHotkey("Increment State Slot").onPress([&] {
    if(!emulator) return;
    if(program.state.slot == 9) program.state.slot = 1;
    else program.state.slot++;
    program.showMessage({"Selected state slot ", program.state.slot});
  }));

  hotkeys.push_back(InputHotkey("Pause Emulation").onPress([&] {
    if(!emulator) return;
    auto& rspCap = ares::Nintendo64::rsp.capture;
    auto& rdpCap = ares::Nintendo64::rdp.capture;
    if(program.paused || rspCap.stepMode.load() || rdpCap.stepMode.load()) {
      // Unpausing: break spin-waits FIRST (no Guard), then Guard for pause(false)
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
      // RSP/RDP: just set stepMode, emulation keeps running
      rspCap.stepMode.store(program.stepType == Program::StepType::RSP, std::memory_order_release);
      rdpCap.stepMode.store(program.stepType == Program::StepType::RDP, std::memory_order_release);
    }
  }));

  hotkeys.push_back(InputHotkey("Reset System").onPress([&] {
    Program::Guard guard;
    if(!emulator) return;
    emulator->root->power(true);
  }));

  hotkeys.push_back(InputHotkey("Reload Current Game").onPress([&] {
    Program::Guard guard;
    if(!emulator) return;
    program.load(emulator, emulator->game->location);
  }));

  hotkeys.push_back(InputHotkey("Quit Emulator").onPress([&] {
    Program::Guard guard;
    program.quit();
  }));

  hotkeys.push_back(InputHotkey("Mute Audio").onPress([&] {
    if(!emulator) return;
    program.mute();
  }));

  hotkeys.push_back(InputHotkey("Increase Audio").onPress([&] {
    if(!emulator) return;
    if(settings.audio.volume <= (f64)(1.9)) settings.audio.volume += (f64)(0.1);
  }));

  hotkeys.push_back(InputHotkey("Decrease Audio").onPress([&] {
    if(!emulator) return;
    if(settings.audio.volume >= (f64)(0.1)) settings.audio.volume -= (f64)(0.1);
  }));

}

	auto InputManager::pollHotkeys() -> void {
	  if(program._imguiMode) {
	    if(ImGui::GetIO().WantCaptureKeyboard) return;
	  } else {
	    if(false && ::Application::modal() /* hiro removed */) return; // hiro removed
	    if(
	      program.settingsWindowConstructed && settingsWindow.focused() ||
	      program.toolsWindowConstructed && toolsWindow.focused()
	    ) return;
	    if(settings.input.defocus != "Allow") {
	      if (!presentation.focused() && !ruby::video.fullScreen()) return;
	    }
	  }

	  for(auto& hotkey : hotkeys) {
	    auto state = hotkey.value();
	    if(hotkey.state == 0 && state == 1 && hotkey.press) hotkey.press();
	    if(hotkey.state == 1 && state == 0 && hotkey.release) hotkey.release();
	    hotkey.state = state;
	  }
	}
