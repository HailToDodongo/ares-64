#include "ui.hpp"

#include "../desktop-ui.hpp"

#include <cstdio>

namespace ares::ui {

bool showMemoryViewer = false;

// All access goes through Node::Debugger::Memory read/write callbacks. These are
// the cores' debug accessors (e.g. RDRAM uses RBusDevice::ARES_DEBUGGER, RSP reads
// DMEM/IMEM directly) so viewing/editing memory here does not perturb emulation.
static std::vector<ares::Node::Debugger::Memory> memoryNodes;
static ares::Node::Object lastRoot;        // re-enumerate when the loaded system changes
static int memSelection = 0;
static char gotoBuf[16] = {};
static s64 scrollToRow = -1;               // pending jump request (row index), -1 = none
static s64 editAddr = -1;                  // byte currently being edited, -1 = none
static bool editFocus = false;             // grab keyboard focus on the first edit frame
static char editBuf[4] = {};

static auto RefreshMemoryNodes() -> void {
  memoryNodes.clear();
  memSelection = 0;
  editAddr = -1;
  if(emulator && emulator->root) {
    for(auto node : ares::Node::enumerate<ares::Node::Debugger::Memory>(emulator->root)) {
      memoryNodes.push_back(node);
    }
  }
  lastRoot = emulator ? emulator->root : ares::Node::Object{};
}

auto DrawMemoryViewer() -> void {
  if(!showMemoryViewer) return;

  ImGui::SetNextWindowSize(ImVec2(560, 420), ImGuiCond_FirstUseEver);
  if(!ImGui::Begin("Memory Editor", &showMemoryViewer)) {
    ImGui::End();
    settings.general.showMemoryViewer = false;
    return;
  }

  if(!emulator) {
    ImGui::TextUnformatted("No game loaded.");
    ImGui::End();
    settings.general.showMemoryViewer = true;
    return;
  }

  // (Re)build the node list when a new system is loaded.
  if((emulator ? emulator->root : ares::Node::Object{}) != lastRoot || memoryNodes.empty()) {
    RefreshMemoryNodes();
  }

  if(memoryNodes.empty()) {
    ImGui::TextUnformatted("No debug memory regions available.");
    ImGui::End();
    settings.general.showMemoryViewer = true;
    return;
  }
  if(memSelection >= (int)memoryNodes.size()) memSelection = 0;

  // --- Toolbar: memory selector + goto ---
  ImGui::SetNextItemWidth(160);
  if(ImGui::BeginCombo("##memsel", memoryNodes[memSelection]->name().data())) {
    for(int i = 0; i < (int)memoryNodes.size(); i++) {
      if(ImGui::Selectable(memoryNodes[i]->name().data(), memSelection == i)) {
        memSelection = i;
        editAddr = -1;
      }
    }
    ImGui::EndCombo();
  }

  auto node = memoryNodes[memSelection];
  u32 memSize = node->size();

  ImGui::SameLine();
  ImGui::TextDisabled("size 0x%X", memSize);

  ImGui::SameLine();
  ImGui::SetNextItemWidth(90);
  if(ImGui::InputText("##goto", gotoBuf, sizeof(gotoBuf),
                      ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_EnterReturnsTrue)) {
    u32 addr = (u32)strtoul(gotoBuf, nullptr, 16);
    if(memSize) scrollToRow = (addr % memSize) / 16;
  }
  ImGui::SameLine();
  if(ImGui::Button("Go")) {
    u32 addr = (u32)strtoul(gotoBuf, nullptr, 16);
    if(memSize) scrollToRow = (addr % memSize) / 16;
  }

  ImGui::Separator();

  const int bytesPerRow = 16;
  u32 totalRows = (memSize + bytesPerRow - 1) / bytesPerRow;

  ImGui::PushFont(monoFont);
  float glyph = ImGui::CalcTextSize("F").x;
  ImGui::PopFont();
  float addrCol = glyph * 10.0f;                 // "00000000: "
  float byteW = glyph * 3.0f;                    // "FF "
  float groupGap = glyph * 1.0f;                 // extra gap between the two 8-byte halves
  float asciiCol = addrCol + bytesPerRow * byteW + groupGap + glyph;

  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 1));
  if(ImGui::BeginChild("##hex", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar)) {
    float lineHeight = ImGui::GetTextLineHeightWithSpacing();

    // Honour a pending goto request.
    if(scrollToRow >= 0) {
      ImGui::SetScrollY((float)scrollToRow * lineHeight);
      scrollToRow = -1;
    }

    ImGuiListClipper clipper;
    clipper.Begin((int)totalRows, lineHeight);
    while(clipper.Step()) {
      for(int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++) {
        u32 rowAddr = (u32)row * bytesPerRow;

        ImGui::PushFont(monoFont);
        ImGui::Text("%08X:", rowAddr);
        ImGui::PopFont();

        // Hex byte cells — monospaced so columns stay aligned.
        char ascii[bytesPerRow + 1];
        ImGui::PushFont(monoFont);
        for(int c = 0; c < bytesPerRow; c++) {
          u32 addr = rowAddr + c;
          if(addr >= memSize) { ascii[c] = 0; break; }

          float x = addrCol + c * byteW + (c >= 8 ? groupGap : 0.0f);
          ImGui::SameLine(x);

          u8 value = node->read(addr);
          ascii[c] = (value >= 0x20 && value < 0x7f) ? (char)value : '.';

          // Scope each cell by address: byte values repeat, so the widget labels
          // ("FF", "##edit") would otherwise collide and trip ImGui's ID check.
          ImGui::PushID((int)addr);
          if(editAddr == (s64)addr) {
            // Inline editor for this byte.
            ImGui::SetNextItemWidth(byteW);
            if(editFocus) { ImGui::SetKeyboardFocusHere(); editFocus = false; }
            bool commit = ImGui::InputText("##edit", editBuf, sizeof(editBuf),
              ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_EnterReturnsTrue
              | ImGuiInputTextFlags_AutoSelectAll | ImGuiInputTextFlags_NoHorizontalScroll);
            bool deactivated = ImGui::IsItemDeactivated();
            if(commit || deactivated) {
              if(editBuf[0]) node->write(addr, (u8)strtoul(editBuf, nullptr, 16));
              if(commit && addr + 1 < memSize) {
                // Advance to the next byte for fast sequential editing.
                editAddr = addr + 1;
                editBuf[0] = 0;
                editFocus = true;
              } else {
                editAddr = -1;
              }
            }
          } else {
            char buf[4];
            snprintf(buf, sizeof(buf), "%02X", value);
            if(value == 0) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::Selectable(buf, false, ImGuiSelectableFlags_None, ImVec2(glyph * 2.0f, 0));
            if(value == 0) ImGui::PopStyleColor();
            if(ImGui::IsItemClicked()) {
              editAddr = addr;
              editBuf[0] = 0;
              editFocus = true;
            }
          }
          ImGui::PopID();
        }
        ImGui::PopFont();
        ascii[bytesPerRow] = 0;

        // ASCII column (monospaced so characters align with hex above).
        ImGui::SameLine(asciiCol);
        ImGui::PushFont(monoFont);
        ImGui::TextUnformatted(ascii);
        ImGui::PopFont();
      }
    }
    clipper.End();
  }
  ImGui::EndChild();
  ImGui::PopStyleVar();

  ImGui::End();
  settings.general.showMemoryViewer = true;
}

}  // namespace ares::ui
