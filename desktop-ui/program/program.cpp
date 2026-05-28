#include "../desktop-ui.hpp"
#include "../ui/ui.hpp"
#include <n64/n64.hpp>
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

    program.requestFrameAdvance = false;
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
      auto& cap = ares::Nintendo64::rdp.capture;
      cap.committedCount.store(cap.writePos.load(std::memory_order_acquire), std::memory_order_release);
      cap.writePos.store(0, std::memory_order_release);
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
  Program::Guard guard;
  _quitRequested = false;
  _quitting = true;
  if(lock.owns_lock()) {
    lock.unlock();
  }
  _programConditionVariable.notify_all();
  worker.join();
  program._isRunning = false;
  unload();
  if(false && Application::state().initialized /* hiro removed */) {
    /* Application::processEvents removed */;
    /* Application::quit removed */;
  }

  ruby::video.reset();
}
