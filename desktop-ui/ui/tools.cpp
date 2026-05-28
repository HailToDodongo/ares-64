#include "ui.hpp"

#include "../desktop-ui.hpp"

namespace ares::ui {

bool showManifestViewer = false;
bool showCheatEditor = false;
bool showTracerViewer = false;

// --- Manifest shared state ---

static int manifestSelection = 0;
static std::vector<ares::Node::Object> manifestNodes;
static string manifestText;

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

// --- Cheats shared state ---

static char cheatDescription[256] = {};
static char cheatCode[256] = {};
static int cheatSelection = -1;
struct CheatEntry {
  string description;
  string code;
  bool enabled = false;
};
static std::vector<CheatEntry> cheatEntries;

// --- Tracer shared state ---

static std::vector<ares::Node::Debugger::Tracer::Tracer> tracerNodes;

auto RefreshTracerNodes() -> void {
  tracerNodes.clear();
  if(emulator && emulator->root) {
    tracerNodes = ares::Node::enumerate<ares::Node::Debugger::Tracer::Tracer>(emulator->root);
  }
}

// --- Refresh helper ---

auto RefreshTools() -> void {
  RefreshManifestNodes();
  RefreshTracerNodes();
}

// --- Manifest Viewer ---

auto DrawManifestViewer() -> void {
  if(!showManifestViewer) return;

  ImGui::SetNextWindowSize(ImVec2(500, 350), ImGuiCond_FirstUseEver);
  if(!ImGui::Begin("Manifest Viewer", &showManifestViewer)) {
    ImGui::End();
    settings.general.showManifestViewer = false;
    return;
  }

  if(!emulator) {
    ImGui::TextUnformatted("No game loaded.");
    ImGui::End();
    settings.general.showManifestViewer = true;
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

  ImGui::End();
  settings.general.showManifestViewer = true;
}

// --- Cheat Editor ---

auto DrawCheatEditor() -> void {
  if(!showCheatEditor) return;

  ImGui::SetNextWindowSize(ImVec2(500, 400), ImGuiCond_FirstUseEver);
  if(!ImGui::Begin("Cheat Editor", &showCheatEditor)) {
    ImGui::End();
    settings.general.showCheatEditor = false;
    return;
  }

  if(!emulator) {
    ImGui::TextUnformatted("No game loaded.");
    ImGui::End();
    settings.general.showCheatEditor = true;
    return;
  }

  // Cheat list
  if(!ImGui::BeginTable("cheats", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg, ImVec2(0, -80))) {
    ImGui::End();
    settings.general.showCheatEditor = true;
    return;
  }
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

  ImGui::End();
  settings.general.showCheatEditor = true;
}

// --- Tracer Viewer ---

auto DrawTracerViewer() -> void {
  if(!showTracerViewer) return;

  ImGui::SetNextWindowSize(ImVec2(500, 300), ImGuiCond_FirstUseEver);
  if(!ImGui::Begin("Trace Logger", &showTracerViewer)) {
    ImGui::End();
    settings.general.showTracerViewer = false;
    return;
  }

  if(!emulator) {
    ImGui::TextUnformatted("No game loaded.");
    ImGui::End();
    settings.general.showTracerViewer = true;
    return;
  }

  if(!ImGui::BeginTable("tracers", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
    ImGui::End();
    settings.general.showTracerViewer = true;
    return;
  }
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

  ImGui::End();
  settings.general.showTracerViewer = true;
}

}  // namespace ares::ui
