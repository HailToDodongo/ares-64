#include "ui.hpp"

#include "../desktop-ui.hpp"
#include <n64/n64.hpp>

namespace ares::ui {

bool showRdpViewer = false;

static auto cmdColor(u8 opcode) -> ImU32 {
  if(opcode >= 0x08 && opcode <= 0x0f) return IM_COL32(100, 200, 255, 255); // triangles
  if(opcode >= 0x24 && opcode <= 0x25) return IM_COL32(100, 255, 150, 255); // rects
  if(opcode >= 0x26 && opcode <= 0x29) return IM_COL32(255, 255, 100, 255); // sync
  if(opcode >= 0x2a && opcode <= 0x3f) return IM_COL32(255, 180, 100, 255); // set/load
  if(opcode == 0x36)                   return IM_COL32(255, 150, 200, 255); // fill rect
  return IM_COL32(128, 128, 128, 255); // invalid/nop
}

auto DrawRdpViewer() -> void {
  if(!showRdpViewer) return;

  ImGui::SetNextWindowSize(ImVec2(600, 400), ImGuiCond_FirstUseEver);
  if(!ImGui::Begin("RDP Commands", &showRdpViewer)) {
    ImGui::End();
    settings.general.showRdpViewer = false;
    return;
  }

  if(!emulator || emulator->name != "Nintendo 64") {
    ImGui::TextUnformatted("RDP viewer only available for Nintendo 64.");
    ImGui::End();
    settings.general.showRdpViewer = true;
    return;
  }

  auto& cap = ares::Nintendo64::rdp.capture;
  bool enabled = cap.enabled.load(std::memory_order_relaxed);
  if(ImGui::Checkbox("Capture", &enabled)) {
    cap.enabled.store(enabled, std::memory_order_relaxed);
  }

  ImGui::SameLine();
  u32 frameCounter = cap.frameCounter.load(std::memory_order_acquire);
  u32 totalWritten = cap.writePos.load(std::memory_order_acquire);
  u32 slotCount = std::min(totalWritten, cap.maxCommands);

  // Build list of commands from the latest frame
  static std::vector<u32> latestCmds;  // indices into cap.commands for the latest frame
  latestCmds.clear();

  u32 latestFrame = 0;
  u32 startPos = totalWritten > cap.maxCommands ? (totalWritten - cap.maxCommands) % cap.maxCommands : 0;

  // First pass: find the latest frame number
  for(u32 i = 0; i < slotCount; i++) {
    u32 pos = (startPos + i) % cap.maxCommands;
    auto& cmd = cap.commands[pos];
    if(cmd.frame > latestFrame) latestFrame = cmd.frame;
  }

  // Second pass: collect commands from the latest frame
  for(u32 i = 0; i < slotCount; i++) {
    u32 pos = (startPos + i) % cap.maxCommands;
    auto& cmd = cap.commands[pos];
    if(cmd.frame == latestFrame) latestCmds.push_back(pos);
  }

  ImGui::Text("Frame: %u | Commands: %zu", latestFrame, latestCmds.size());

  ImGui::Separator();

  if(latestCmds.empty()) {
    ImGui::TextUnformatted("No commands captured. Enable capture above.");
    ImGui::End();
    settings.general.showRdpViewer = true;
    return;
  }

  if(!ImGui::BeginTable("rdp_cmds", 3,
       ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
       ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit,
       ImVec2(0, 0))) {
    ImGui::End();
    settings.general.showRdpViewer = true;
    return;
  }
  ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 50);
  ImGui::TableSetupColumn("Command", ImGuiTableColumnFlags_WidthStretch);
  ImGui::TableSetupColumn("Hex", ImGuiTableColumnFlags_WidthFixed, 150);
  ImGui::TableSetupScrollFreeze(0, 1);
  ImGui::TableHeadersRow();

  ImGuiListClipper clipper;
  clipper.Begin(latestCmds.size());
  while(clipper.Step()) {
    for(int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++) {
      auto& cmd = cap.commands[latestCmds[row]];
      ImGui::TableNextRow();

      ImGui::TableNextColumn();
      ImGui::Text("%u", cmd.index);

      ImGui::TableNextColumn();
      auto name = ares::Nintendo64::rdpCommandName(cmd.opcode);
      ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(cmdColor(cmd.opcode)), "%s", name);

      ImGui::TableNextColumn();
      ImGui::Text("%016llX", (unsigned long long)cmd.word0);
    }
  }
  ImGui::EndTable();

  ImGui::End();
  settings.general.showRdpViewer = true;
}

}  // namespace ares::ui
