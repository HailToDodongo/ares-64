// Tool panel types (hiro widget members removed; ImGui handles all UI)

struct ManifestViewer {
  auto construct() -> void {}
  auto reload() -> void {}
  auto unload() -> void {}
};

struct CheatEditor {
  struct Cheat {
    bool enabled;
    string description;
    string code;
  };

  auto construct() -> void {}
  auto reload() -> void {}
  auto unload() -> void {}
  auto find(u32 address) -> maybe<u32> { return {}; }

  std::vector<Cheat> cheats;
};

struct MemoryEditor {
  auto construct() -> void {}
  auto reload() -> void {}
  auto unload() -> void {}
  auto liveRefresh() -> void {}
};

struct GraphicsViewer {
  auto construct() -> void {}
  auto reload() -> void {}
  auto unload() -> void {}
  auto liveRefresh() -> void {}
};

struct StreamManager {
  auto construct() -> void {}
  auto reload() -> void {}
  auto unload() -> void {}
};

struct PropertiesViewer {
  auto construct() -> void {}
  auto reload() -> void {}
  auto unload() -> void {}
  auto liveRefresh() -> void {}
};

struct TraceLogger {
  auto construct() -> void {}
  auto reload() -> void {}
  auto unload() -> void {}

  file_buffer fp;
};

struct TapeViewer {
  auto construct() -> void {}
  auto reload() -> void {}
  auto unload() -> void {}
  auto liveRefresh() -> void {}
};

struct ToolsWindow {
  auto show(const nall::string&) -> void {}
  auto setVisible(bool = true) -> void {}
  auto focused() -> bool { return false; }

  ManifestViewer manifestViewer;
  CheatEditor cheatEditor;
  MemoryEditor memoryEditor;
  GraphicsViewer graphicsViewer;
  StreamManager streamManager;
  PropertiesViewer propertiesViewer;
  TraceLogger traceLogger;
  TapeViewer tapeViewer;
};

extern ToolsWindow toolsWindow;
extern CheatEditor& cheatEditor;
extern ManifestViewer& manifestViewer;
extern MemoryEditor& memoryEditor;
extern GraphicsViewer& graphicsViewer;
extern StreamManager& streamManager;
extern PropertiesViewer& propertiesViewer;
extern TraceLogger& traceLogger;
extern TapeViewer& tapeViewer;
