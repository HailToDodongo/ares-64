// Global definitions for tool/presentation/game-browser objects.
// All actual UI is handled by ImGui.

#include "../desktop-ui.hpp"

ToolsWindow toolsWindow;
CheatEditor& cheatEditor = toolsWindow.cheatEditor;
ManifestViewer& manifestViewer = toolsWindow.manifestViewer;
MemoryEditor& memoryEditor = toolsWindow.memoryEditor;
GraphicsViewer& graphicsViewer = toolsWindow.graphicsViewer;
StreamManager& streamManager = toolsWindow.streamManager;
PropertiesViewer& propertiesViewer = toolsWindow.propertiesViewer;
TraceLogger& traceLogger = toolsWindow.traceLogger;
TapeViewer& tapeViewer = toolsWindow.tapeViewer;

Presentation presentation;
GameBrowserWindow gameBrowserWindow;
