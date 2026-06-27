#include "../desktop-ui.hpp"
#include "../ui/ui.hpp"
#include <n64/n64.hpp>
#if ARES_DEBUG_TOOLS
  #include "embedded-overlays.hpp"  // generated: ares::ui::embeddedRspqJson / embeddedF3dJson
#endif
#include "platform.cpp"
#include "load.cpp"
#include "states.cpp"
#include "rewind.cpp"
#include "status.cpp"
#include "utility.cpp"
#include "drivers.cpp"

Program program;
thread worker;

auto Program::create() -> void {
  ares::platform = this;

  videoDriverUpdate();
  audioDriverUpdate();
  inputDriverUpdate();

  if(kiosk) {
    if(startFullScreen) videoFullScreenToggle();
    if(startPseudoFullScreen) videoPseudoFullScreenToggle();
  }

  _isRunning = true;
  worker = thread::create(std::bind_front(&Program::emulatorRunLoop, this));
  program.rewindReset();

  if(!startGameLoad.empty()) {
    Program::Guard guard;
    auto gameToLoad = startGameLoad.front();
    startGameLoad.erase(startGameLoad.begin());
    auto emu = program.identify(gameToLoad);
    if(startSystem) {
      for(auto &emulator: emulators) {
        if(emulator->name == startSystem) {
          if(load(emulator, gameToLoad)) {
            if(!kiosk) {
              if(startFullScreen) videoFullScreenToggle();
              if(startPseudoFullScreen) videoPseudoFullScreenToggle();
            }
          }
          return;
        }
      }
      return;
    }

    if(auto emu2 = identify(gameToLoad)) {
      if(load(emu2, gameToLoad)) {
        if(!kiosk) {
          if(startFullScreen) videoFullScreenToggle();
          if(startPseudoFullScreen) videoPseudoFullScreenToggle();
        }
      } else {
      }
    } else {
    }
  }  // Guard destructor runs here
}

auto Program::waitForInterrupts() -> void {
  std::unique_lock<std::mutex> lock(_programMutex);
  _interruptWorking = true;
  _programConditionVariable.notify_one();
  _programConditionVariable.wait(lock, [this] { return !_interruptWorking || _quitting; });
}

auto Program::emulatorRunLoop(uintptr_t) -> void {
  thread::setName("dev.ares.worker");
  _programThread = true;
  while(!_quitting) {
    // Allow other threads to carry out tasks between emulator run loop iterations
    if(_interruptWaiting) {
      waitForInterrupts();
      continue;
    }
    if(!emulator) {
      usleep(20 * 1000);
      continue;
    }

    if(emulator && nall::GDB::server.isHalted()) {
      ruby::audio.clear();
      nall::GDB::server.updateLoop(); // sleeps internally
      continue;
    }

    bool defocused = settings.input.defocus == "Pause" && !ruby::video.fullScreen() && !program._imguiMode && !presentation.focused();

    if(!emulator || (paused && !program.requestFrameAdvance) || defocused) {
      ruby::audio.clear();
      nall::GDB::server.updateLoop();
      usleep(20 * 1000);
      continue;
    }

    rewindRun();

    nall::GDB::server.updateLoop();

    bool didFrameAdvance = program.requestFrameAdvance;
    program.requestFrameAdvance = false;

    // Advances exactly one frame, then runs the N64 RSP/RDP capture
    // frame-boundary bookkeeping. Factored out so the "Buffer" step type can
    // drive several frames in a row.
    auto runOneFrame = [&] {
      if(!runAhead || fastForwarding || rewinding) {
        emulator->root->run();
      } else {
        ares::setRunAhead(true);
        emulator->root->run();
        auto state = emulator->root->serialize(false);
        ares::setRunAhead(false);
        emulator->root->run();
        state.setReading();
        emulator->root->unserialize(state);
      }

      if(emulator && emulator->name == "Nintendo 64") {
        auto& vi = ares::Nintendo64::vi;
        auto& cap = ares::Nintendo64::rdp.capture;
        auto& rspCap = ares::Nintendo64::rsp.capture;

        // Commit + clear the RSP/RDP command buffers on a framebuffer swap (the VI
        // origin changing to a different buffer), not on every VI. Each swap marks
        // a presented frame: publish the finished frame to the committed-count
        // viewers, then reset the live write cursor so the next frame starts fresh.
        static u32 lastViAddr = 0;
        u32 viAddr = vi.io.dramAddress;
        // Skip the auto-clear while RSP or RDP stepping is active — the
        // user controls the buffer manually via Clear / Run buttons.
        bool stepping = rspCap.stepMode.load(std::memory_order_relaxed)
                     || cap.stepMode.load(std::memory_order_relaxed);
        if(!stepping && viAddr != 0 && viAddr != lastViAddr) {
          lastViAddr = viAddr;
          cap.committedCount.store(cap.writePos.load(std::memory_order_acquire), std::memory_order_release);
          rspCap.committedCount.store(rspCap.writePos.load(std::memory_order_acquire), std::memory_order_release);
          cap.writePos.store(0, std::memory_order_release);
          rspCap.writePos.store(0, std::memory_order_release);
          rspCap.frameNumber++;
          rspCap.refreshOverlayNames();
          rspCap.detectF3DEX2();  //Fast3D (SM64 etc.): auto-hook the DL dispatch once seen

          if(ares::ui::logDump.active()) {
            u32 rspN = min<u32>(rspCap.committedCount.load(std::memory_order_acquire), rspCap.maxCommands);
            u32 rdpN = min<u32>(cap.committedCount.load(std::memory_order_acquire), cap.maxCommands);
            if(ares::ui::LogDumpOnFrame(rspN, rdpN)) _quitRequested = true;
          }
          ares::Nintendo64::cpu.profiler.onFrame();
        }
      }
    };

    // "Frame" step: on N64 a single step advances whole frames until the VI
    // framebuffer origin is swapped to a different buffer (a real presented frame),
    // bounded to 100 frames so a static screen can't freeze the step. Other systems
    // (and normal play) just advance one frame.
    bool n64FrameStep = didFrameAdvance
                     && program.stepType == Program::StepType::Frame
                     && emulator && emulator->name == "Nintendo 64";
    u32 stepStartAddr = n64FrameStep ? (u32)ares::Nintendo64::vi.io.dramAddress : 0;

    //Apply a pending RDP renderer hot-swap at the frame boundary (no-op unless an N64
    //system is loaded and a switch was requested from the UI).
    ares::Nintendo64::system.applyPendingRenderer();

    runOneFrame();
    if(n64FrameStep) {
      for(u32 i = 1; i < 100 && (u32)ares::Nintendo64::vi.io.dramAddress == stepStartAddr; i++) {
        runOneFrame();
      }
    }

    nall::GDB::server.updateLoop();

    if(settings.general.autoSaveMemory) {
      static u64 previousTime = chrono::timestamp();
      u64 currentTime = chrono::timestamp();
      if(currentTime - previousTime >= 30) {
        previousTime = currentTime;
        emulator->save();
      }
    }

    if(emulator->latch.changed) {
      emulator->latch.changed = false;
      _needsResize = true;
    }
  }
}

auto Program::main() -> void {
  if(!_imguiMode) {
    if(false && Application::state().initialized /* hiro removed */ && false && Application::modal() /* hiro removed */) {
      ruby::audio.clear();
      return;
    }
  }

  inputManager.poll();
  inputManager.pollHotkeys();

  // Hold-to-repeat for step hotkey: after 400ms, auto-fire every frame
  if(inputManager.stepHotkeyHeld && emulator) {
    auto elapsed = std::chrono::steady_clock::now() - inputManager.stepHotkeyHoldStart;
    if(elapsed > std::chrono::milliseconds(400)) {
      if(program.stepType == Program::StepType::RSP) {
        program.stepSequence++;
        ares::Nintendo64::rsp.capture.stepPending.store(true, std::memory_order_release);
      } else if(program.stepType == Program::StepType::RDP) {
        program.stepSequence++;
        ares::Nintendo64::rdp.capture.stepPending.store(true, std::memory_order_release);
      } else
        program.requestFrameAdvance = true;
    }
  }

  if(pendingKioskExit) {
    pendingKioskExit = false;
    quit();
    return;
  }

  updateMessage();

  //If Platform::video() changed the screen resolution, resize the presentation window here.
  //Window operations must be performed from the main thread.

  if(_needsResize) {
    if(!_imguiMode && settings.video.adaptiveSizing && !startPseudoFullScreen) presentation.resizeWindow();
    _needsResize = false;
  }

  if(_imguiMode) {
    ares::ui::RefreshTools();
  } else if(toolsWindowConstructed) {
    memoryEditor.liveRefresh();
    graphicsViewer.liveRefresh();
    propertiesViewer.liveRefresh();
    tapeViewer.liveRefresh();
  }
  if (_quitRequested) {
    quit();
  }
}

auto Program::quit() -> void {
  if (_programThread) {
    _quitRequested = true;
    return;
  }

  // Signal worker to stop, release any step-mode spin-wait first
  if(emulator && emulator->name == "Nintendo 64") {
    ares::Nintendo64::rdp.capture.enabled.store(false, std::memory_order_release);
    ares::Nintendo64::rdp.capture.stepMode.store(false, std::memory_order_release);
    ares::Nintendo64::rdp.capture.stepPending.store(true, std::memory_order_release);
    ares::Nintendo64::rsp.capture.enabled.store(false, std::memory_order_release);
    ares::Nintendo64::rsp.capture.stepMode.store(false, std::memory_order_release);
    ares::Nintendo64::rsp.capture.stepPending.store(true, std::memory_order_release);
  }

  _quitting = true;
  _programConditionVariable.notify_all();

  // Safety net: force-quit after 3s if worker is irrevocably stuck
  thread::create([](uintptr) {
    usleep(3'000'000);
    _exit(0);
  }, 0);
  worker.join();

  program._isRunning = false;
  unload();
  program._isRunning = false;
  unload();
  if(false && Application::state().initialized /* hiro removed */) {
    /* Application::processEvents removed */;
    /* Application::quit removed */;
  }

  ruby::video.reset();
}
