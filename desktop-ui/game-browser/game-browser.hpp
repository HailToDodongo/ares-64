// Game browser type (hiro widget members removed)

struct GameBrowserEntry {
  string title, name, board, path;
};

struct GameBrowserWindow {
  auto show(std::shared_ptr<Emulator>) -> void {}
  auto setVisible(bool = true) -> void {}
};

extern GameBrowserWindow gameBrowserWindow;
