#include "ui.hpp"

#include "../desktop-ui.hpp"
#include <imgui_impl_opengl3.h>
#include <SDL3/SDL_opengl.h>

namespace ares::ui {

static int activeTool = 0;
static const char* toolNames[] = {
  "Manifest", "Cheats", "Memory", "Graphics",
  "Streams", "Properties", "Tracer", "Tape"
};
static constexpr int toolCount = sizeof(toolNames) / sizeof(toolNames[0]);

// --- Manifest panel ---

static int manifestSelection = 0;
static std::vector<ares::Node::Object> manifestNodes;
static string manifestText;

static void DrawManifestPanel() {
  ImGui::SeparatorText("Manifest Viewer");

  if(!emulator) {
    ImGui::TextUnformatted("No game loaded.");
    return;
  }

  if(ImGui::BeginCombo("Node", manifestSelection < (int)manifestNodes.size()
      ? manifestNodes[manifestSelection]->name().data() : "Select...")) {
    for(u32 i = 0; i < manifestNodes.size(); i++) {
      if(ImGui::Selectable(manifestNodes[i]->name().data(), manifestSelection == (int)i)) {
        manifestSelection = i;
        manifestText = "";
        if(auto pak = manifestNodes[i]->pak()) {
          if(auto fp = pak->read("manifest.bml")) {
            manifestText = fp->reads();
          }
        }
      }
    }
    ImGui::EndCombo();
  }

  ImGui::TextWrapped("%s", manifestText.data());
}

// The manifest node list is populated from Program::main() or explicitly
static bool manifestDirty = true;
auto RefreshManifestNodes() -> void {
  manifestNodes.clear();
  manifestSelection = 0;
  manifestText = "";
  if(emulator && emulator->root) {
    for(auto node : ares::Node::enumerate<ares::Node::Object>(emulator->root)) {
      if(auto pak = node->pak()) {
        if(pak->read("manifest.bml")) {
          manifestNodes.push_back(node);
        }
      }
    }
  }
  if(!manifestNodes.empty()) {
    if(auto pak = manifestNodes[0]->pak()) {
      if(auto fp = pak->read("manifest.bml")) {
        manifestText = fp->reads();
      }
    }
  }
}

// --- Cheats panel ---

static char cheatDescription[256] = {};
static char cheatCode[256] = {};
static int cheatSelection = -1;
struct CheatEntry {
  string description;
  string code;
  bool enabled = false;
};
static std::vector<CheatEntry> cheatEntries;

static void DrawCheatsPanel() {
  ImGui::SeparatorText("Cheats");

  if(!emulator) {
    ImGui::TextUnformatted("No game loaded.");
    return;
  }

  // Cheat list
  if(!ImGui::BeginTable("cheats", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg, ImVec2(0, -80))) return;
  ImGui::TableSetupColumn("Description");
  ImGui::TableSetupColumn("Code");
  ImGui::TableHeadersRow();

  for(int i = 0; i < (int)cheatEntries.size(); i++) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    if(ImGui::Selectable(cheatEntries[i].description.data(), cheatSelection == i, ImGuiSelectableFlags_SpanAllColumns)) {
      cheatSelection = i;
      memory::copy(cheatDescription, cheatEntries[i].description.data(),
                   min((u32)sizeof(cheatDescription) - 1, cheatEntries[i].description.length()));
      cheatDescription[min((u32)sizeof(cheatDescription) - 1, cheatEntries[i].description.length())] = 0;
      memory::copy(cheatCode, cheatEntries[i].code.data(),
                   min((u32)sizeof(cheatCode) - 1, cheatEntries[i].code.length()));
      cheatCode[min((u32)sizeof(cheatCode) - 1, cheatEntries[i].code.length())] = 0;
    }
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(cheatEntries[i].code.data());
  }
  ImGui::EndTable();

  // Edit area
  ImGui::InputText("Description", cheatDescription, sizeof(cheatDescription));
  ImGui::InputText("Code", cheatCode, sizeof(cheatCode));

  if(ImGui::Button("Save")) {
    string desc = cheatDescription;
    string code = cheatCode;
    if(desc) {
      // Update existing or add new
      auto it = std::ranges::find_if(cheatEntries, [&](auto& c) { return c.description == desc; });
      if(it != cheatEntries.end()) {
        it->code = code;
      } else {
        cheatEntries.push_back({desc, code, false});
      }
    }
  }
  ImGui::SameLine();
  if(ImGui::Button("Delete") && cheatSelection >= 0 && cheatSelection < (int)cheatEntries.size()) {
    cheatEntries.erase(cheatEntries.begin() + cheatSelection);
    cheatSelection = -1;
    cheatDescription[0] = 0;
    cheatCode[0] = 0;
  }
}

// --- Memory panel ---

static void DrawMemoryPanel() {
  ImGui::SeparatorText("Memory Viewer");

  // Simple hex editor — shows N64 RDRAM if available
  ImGui::TextUnformatted("Hex editor not yet fully implemented.");
  ImGui::TextUnformatted("Use the original ares tools window for memory viewing.");
}

// --- Graphics panel ---

static int graphicsSelection = 0;
static std::vector<ares::Node::Debugger::Graphics> graphicsNodes;
static std::vector<u32> graphicsPixels;
static u32 graphicsWidth = 0, graphicsHeight = 0;
static GLuint graphicsTex = 0;
static bool graphicsDirty = true;

auto RefreshGraphicsNodes() -> void {
  graphicsNodes.clear();
  graphicsSelection = 0;
  graphicsPixels.clear();
  graphicsWidth = graphicsHeight = 0;
  if(emulator && emulator->root) {
    graphicsNodes = ares::Node::enumerate<ares::Node::Debugger::Graphics>(emulator->root);
  }
  graphicsDirty = true;
}

static void DrawGraphicsPanel() {
  ImGui::SeparatorText("Graphics Viewer");

  if(!emulator) {
    ImGui::TextUnformatted("No game loaded.");
    return;
  }

  if(ImGui::BeginCombo("Source", graphicsSelection < (int)graphicsNodes.size()
      ? graphicsNodes[graphicsSelection]->name().data() : "Select...")) {
    for(u32 i = 0; i < graphicsNodes.size(); i++) {
      if(ImGui::Selectable(graphicsNodes[i]->name().data(), graphicsSelection == (int)i)) {
        graphicsSelection = i;
        graphicsDirty = true;
      }
    }
    ImGui::EndCombo();
  }

  static bool liveOption = false;
  ImGui::Checkbox("Live", &liveOption);
  ImGui::SameLine();
  if(ImGui::Button("Refresh") || (liveOption && graphicsDirty)) {
    graphicsDirty = false;
    if(graphicsSelection < (int)graphicsNodes.size()) {
      auto g = graphicsNodes[graphicsSelection];
      graphicsWidth = g->width();
      graphicsHeight = g->height();
      auto input = g->capture();
      graphicsPixels.resize(graphicsWidth * graphicsHeight);
      for(u32 y = 0; y < graphicsHeight; y++) {
        for(u32 x = 0; x < graphicsWidth; x++) {
          graphicsPixels[y * graphicsWidth + x] = input[y * graphicsWidth + x] | 0xff000000;
        }
      }
      // Upload to GL texture
      if(graphicsTex == 0) glGenTextures(1, &graphicsTex);
      glBindTexture(GL_TEXTURE_2D, graphicsTex);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, graphicsWidth, graphicsHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, graphicsPixels.data());
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }
  }
  ImGui::SameLine();
  if(ImGui::Button("Export") && graphicsSelection < (int)graphicsNodes.size()) {
    auto g = graphicsNodes[graphicsSelection];
    auto datetime = chrono::local::datetime().replace("-", "").replace(":", "").replace(" ", "-");
    auto location = emulator->locate({"graphics-", datetime, ".png"}, ".png", settings.paths.debugging);
    Encode::PNG::RGB8(location, graphicsPixels.data(), graphicsWidth * sizeof(u32), graphicsWidth, graphicsHeight);
  }

  if(graphicsTex && graphicsWidth > 0) {
    ImGui::Image((ImTextureID)(intptr_t)graphicsTex, ImVec2((float)graphicsWidth, (float)graphicsHeight));
  }
}

// --- Streams panel ---

static std::vector<ares::Node::Audio::Stream> streamNodes;
static bool streamsDirty = true;

auto RefreshStreams() -> void {
  streamNodes.clear();
  if(emulator && emulator->root) {
    streamNodes = ares::Node::enumerate<ares::Node::Audio::Stream>(emulator->root);
  }
  streamsDirty = true;
}

static void DrawStreamsPanel() {
  ImGui::SeparatorText("Audio Streams");

  if(!emulator) {
    ImGui::TextUnformatted("No game loaded.");
    return;
  }

  if(!ImGui::BeginTable("streams", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) return;
  ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 24.0f);
  ImGui::TableSetupColumn("Name");
  ImGui::TableSetupColumn("Channels");
  ImGui::TableSetupColumn("Frequency");
  ImGui::TableHeadersRow();

  for(auto& stream : streamNodes) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    bool muted = stream->muted();
    if(ImGui::Checkbox("##mute", &muted)) {
      stream->setMuted(!muted);
    }
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(stream->name().data());
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(string{stream->channels()}.data());
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(string{u32(stream->frequency() + 0.5), "hz"}.data());
  }
  ImGui::EndTable();
}

// --- Properties panel ---

static int propertiesSelection = 0;
static std::vector<ares::Node::Debugger::Properties> propertiesNodes;
static string propertiesText;
static bool propertiesDirty = true;

auto RefreshPropertiesNodes() -> void {
  propertiesNodes.clear();
  propertiesSelection = 0;
  propertiesText = "";
  if(emulator && emulator->root) {
    propertiesNodes = ares::Node::enumerate<ares::Node::Debugger::Properties>(emulator->root);
  }
  if(!propertiesNodes.empty()) {
    propertiesText = propertiesNodes[0]->query();
  }
}

static void DrawPropertiesPanel() {
  ImGui::SeparatorText("Properties Viewer");

  if(!emulator) {
    ImGui::TextUnformatted("No game loaded.");
    return;
  }

  if(ImGui::BeginCombo("Source", propertiesSelection < (int)propertiesNodes.size()
      ? propertiesNodes[propertiesSelection]->name().data() : "Select...")) {
    for(u32 i = 0; i < propertiesNodes.size(); i++) {
      if(ImGui::Selectable(propertiesNodes[i]->name().data(), propertiesSelection == (int)i)) {
        propertiesSelection = i;
        propertiesText = propertiesNodes[i]->query();
      }
    }
    ImGui::EndCombo();
  }

  static bool liveOption = false;
  if(ImGui::Checkbox("Live", &liveOption)) {}
  ImGui::SameLine();
  if(ImGui::Button("Refresh")) {
    if(propertiesSelection < (int)propertiesNodes.size()) {
      propertiesText = propertiesNodes[propertiesSelection]->query();
    }
  }

  ImGui::TextWrapped("%s", propertiesText.data());
}

// --- Tracer panel ---

static std::vector<ares::Node::Debugger::Tracer::Tracer> tracerNodes;
static bool tracerDirty = true;

auto RefreshTracerNodes() -> void {
  tracerNodes.clear();
  if(emulator && emulator->root) {
    tracerNodes = ares::Node::enumerate<ares::Node::Debugger::Tracer::Tracer>(emulator->root);
  }
}

static void DrawTracerPanel() {
  ImGui::SeparatorText("Trace Logger");

  if(!emulator) {
    ImGui::TextUnformatted("No game loaded.");
    return;
  }

  if(!ImGui::BeginTable("tracers", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) return;
  ImGui::TableSetupColumn("Name");
  ImGui::TableSetupColumn("Prefix");
  ImGui::TableSetupColumn("Terminal");
  ImGui::TableSetupColumn("File");
  ImGui::TableHeadersRow();

  for(auto& tracer : tracerNodes) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    string name = {tracer->component(), " ", tracer->name()};
    ImGui::TextUnformatted(name.data());

    ImGui::TableNextColumn();
    bool prefix = tracer->prefix();
    if(ImGui::Checkbox("##prefix", &prefix)) tracer->setPrefix(prefix);

    ImGui::TableNextColumn();
    bool terminal = tracer->terminal();
    if(ImGui::Checkbox("##terminal", &terminal)) tracer->setTerminal(terminal);

    ImGui::TableNextColumn();
    bool file = tracer->file();
    if(ImGui::Checkbox("##file", &file)) tracer->setFile(file);
  }
  ImGui::EndTable();
}

// --- Tape panel ---

static void DrawTapePanel() {
  ImGui::SeparatorText("Tape Viewer");

  if(!emulator) {
    ImGui::TextUnformatted("No game loaded.");
    return;
  }

  ImGui::TextUnformatted("Tape control not yet implemented.");
}

// --- Refresh helpers called from Program::main() ---

auto RefreshTools() -> void {
  RefreshManifestNodes();
  RefreshGraphicsNodes();
  RefreshStreams();
  RefreshPropertiesNodes();
  RefreshTracerNodes();
}

// --- Main tools window ---

auto DrawToolsWindow() -> void {
  if(!showToolsWindow) return;

  ImGui::SetNextWindowSize(ImVec2(680, 480), ImGuiCond_FirstUseEver);
  if(!ImGui::Begin("Tools", &showToolsWindow)) {
    ImGui::End();
    return;
  }

  ImGui::BeginChild("tool_panels", ImVec2(140, 0), ImGuiChildFlags_Borders);
  for(int i = 0; i < toolCount; i++) {
    if(ImGui::Selectable(toolNames[i], activeTool == i)) {
      activeTool = i;
    }
  }
  ImGui::EndChild();

  ImGui::SameLine();

  ImGui::BeginChild("tool_content", ImVec2(0, 0));
  switch(activeTool) {
    case 0: DrawManifestPanel(); break;
    case 1: DrawCheatsPanel(); break;
    case 2: DrawMemoryPanel(); break;
    case 3: DrawGraphicsPanel(); break;
    case 4: DrawStreamsPanel(); break;
    case 5: DrawPropertiesPanel(); break;
    case 6: DrawTracerPanel(); break;
    case 7: DrawTapePanel(); break;
  }
  ImGui::EndChild();

  ImGui::End();
}

}  // namespace ares::ui
