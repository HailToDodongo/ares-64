// Presentation type (hiro widget members removed; ImGui handles all UI)

struct Presentation {
  auto resizeWindow() -> void {}
  auto loadEmulators() -> void {}
  auto loadEmulator() -> void {}
  auto unloadEmulator(bool = false) -> void {}
  auto showIcon(bool) -> void {}
  auto loadShaders() -> void {}
  auto refreshSystemMenu() -> void {}
  auto focused() -> bool { return false; }
  auto fullScreen() -> bool { return false; }
  auto setFullScreen(bool) -> void {}
  auto setFocused() -> void {}

  // Dummy members (only accessed behind _imguiMode guards, never in ImGui path)
  struct DummyMenuBar { auto setVisible(bool) -> DummyMenuBar& { return *this; } } menuBar;
  struct DummyViewport { auto handle() const -> uintptr { return 0; } auto setFocused() -> void {} } viewport;
  struct DummyLabel { auto setText(const nall::string& = "") -> DummyLabel& { return *this; } } statusLeft, statusRight;
  struct DummyCheckItem { auto setChecked(bool) -> DummyCheckItem& { return *this; } } pauseEmulation, muteAudioSetting;

  std::vector<nall::string> shaderDirectories;
  static inline bool shaderArgApplied = false;
};

extern Presentation presentation;
