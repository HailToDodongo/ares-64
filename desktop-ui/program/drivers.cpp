auto Program::videoDriverUpdate() -> void {
  Program::Guard guard;
  //Reset stale/unknown driver names (e.g. configs from the old OpenGL backend).
  if(!ruby::Video::hasDriver(settings.video.driver)) {
    settings.video.driver = ruby::Video::optimalDriver();
  }
  ruby::video.create(settings.video.driver);
  if(_imguiMode) {
    ruby::video.setContext(_videoContext);
  } else {
    ruby::video.setContext(presentation.viewport.handle());
  }
  videoMonitorUpdate();
  videoFormatUpdate();
  ruby::video.setBlocking(settings.video.blocking);
  ruby::video.setFlush(settings.video.flush);

  if(!ruby::video.ready()) {
    driverInitFailed(settings.video.driver, "video", [&] { driverSettings.videoDriverUpdate(); });
    return;
  }
}

auto Program::videoMonitorUpdate() -> void {
  Program::Guard guard;
  if(!ruby::video.hasMonitor(settings.video.monitor)) {
    settings.video.monitor = ruby::video.monitor();
  }
  ruby::video.setMonitor(settings.video.monitor);
}

auto Program::videoFormatUpdate() -> void {
  Program::Guard guard;
  if(!ruby::video.hasFormat(settings.video.format)) {
    settings.video.format = ruby::video.format();
  }
  ruby::video.setFormat(settings.video.format);
}

auto Program::videoFullScreenToggle() -> void {
  Program::Guard guard;
  if(_imguiMode || !ruby::video.hasFullScreen()) return;

  ruby::video.clear();
  if(!ruby::video.fullScreen()) {
    ruby::video.setFullScreen(true);
    if(!ruby::input.acquired()) {
      if(ruby::video.hasMonitors().size() == 1) {
        ruby::input.acquire();
      }
    }
  } else {
    if(ruby::input.acquired()) {
      ruby::input.release();
    }
    ruby::video.setFullScreen(false);
    presentation.viewport.setFocused();
  }
}

auto Program::videoPseudoFullScreenToggle() -> void {
  Program::Guard guard;
  if(_imguiMode || ruby::video.fullScreen()) return;

  ruby::video.clear();
  if(!presentation.fullScreen()) {
    presentation.setFullScreen(true);
    presentation.menuBar.setVisible(false);
    if(!ruby::input.acquired() && ruby::video.hasMonitors().size() == 1) {
      ruby::input.acquire();
    }
    startPseudoFullScreen = true;
  } else {
    if(ruby::input.acquired()) {
      ruby::input.release();
    }
    if(!kiosk) presentation.menuBar.setVisible(true);
    presentation.setFullScreen(false);
    presentation.viewport.setFocused();
    startPseudoFullScreen = false;
  }
}

auto Program::audioDriverUpdate() -> void {
  Program::Guard guard;
  ruby::audio.create();
  audioFrequencyUpdate();
  audioLatencyUpdate();
  ruby::audio.setBlocking(settings.audio.blocking);
  ruby::audio.setDynamic(settings.audio.dynamic);
}

auto Program::audioFrequencyUpdate() -> void {
  Program::Guard guard;
  ruby::audio.setFrequency(settings.audio.frequency);
  for(auto& stream : streams) {
    stream->setResamplerFrequency(ruby::audio.frequency);
  }
}

auto Program::audioLatencyUpdate() -> void {
  Program::Guard guard;
  ruby::audio.setLatency(settings.audio.latency);
}

auto Program::inputDriverUpdate() -> void {
  Program::Guard guard;
  ruby::input.create();
  ruby::input.setContext((uintptr)AresApp::window);
  ruby::input.onChange(std::bind_front(&InputManager::eventInput, &inputManager));
  inputManager.poll(true);
}

auto Program::driverInitFailed(nall::string& driver, const char* kind, auto&& updateSettingsWindow) -> void {
  error({"Failed to initialize ", driver, " ", kind, " driver."});

  driver = "None";
  if(settingsWindowConstructed) updateSettingsWindow();
}
