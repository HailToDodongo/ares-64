#include "ui.hpp"

#include "../desktop-ui.hpp"
#include <n64/n64.hpp>

namespace ares::ui {

bool showRdpViewer = false;

static auto cmdColor(u8 opcode) -> ImU32 {
  if(opcode >= 0x08 && opcode <= 0x0f) return IM_COL32(100, 200, 255, 255);
  if(opcode >= 0x24 && opcode <= 0x25) return IM_COL32(100, 255, 150, 255);
  if(opcode >= 0x26 && opcode <= 0x29) return IM_COL32(255, 255, 100, 255);
  if(opcode >= 0x2a && opcode <= 0x3f) return IM_COL32(255, 180, 100, 255);
  if(opcode == 0x36)                   return IM_COL32(255, 150, 200, 255);
  return IM_COL32(128, 128, 128, 255);
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
  u32 count = cap.committedCount.load(std::memory_order_acquire);
  if(count > cap.maxCommands) count = cap.maxCommands;
  ImGui::Text("Commands: %u", count);

  // Step button — shown when RDP stepping is the active mode
  bool isRdpStep = (program.stepType == Program::StepType::RDP);
  if(isRdpStep) {
    ImGui::SameLine();
    if(ImGui::Button("Step >")) {
      program.stepSequence++;
      cap.stepPending.store(true, std::memory_order_release);
    }
    // Hold-to-repeat: 400ms initial delay, then fire every frame
    static bool steppingHeld = false;
    static auto stepHoldStart = std::chrono::steady_clock::now();
    if(ImGui::IsItemActive()) {
      if(!steppingHeld) { steppingHeld = true; stepHoldStart = std::chrono::steady_clock::now(); }
      auto elapsed = std::chrono::steady_clock::now() - stepHoldStart;
      if(elapsed > std::chrono::milliseconds(400)) {
        program.stepSequence++;
        cap.stepPending.store(true, std::memory_order_release);
      }
    } else {
      steppingHeld = false;
    }
  }
  // React to *either* stepper (RDP or RSP): when the other one steps, new rows
  // may be appended here too, and we want to follow + highlight them just the same.
  bool anyStep = (program.stepType == Program::StepType::RSP)
              || (program.stepType == Program::StepType::RDP);

  ImGui::Separator();

  if(count == 0) {
    ImGui::TextUnformatted("No commands captured.");
    ImGui::End();
    settings.general.showRdpViewer = true;
    return;
  }

  if(!ImGui::BeginTable("rdp_cmds", 4,
       ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
       ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit,
       ImVec2(0, 0))) {
    ImGui::End();
    settings.general.showRdpViewer = true;
    return;
  }
  ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 40);
  ImGui::TableSetupColumn("Command", ImGuiTableColumnFlags_WidthFixed, 180);
  ImGui::TableSetupColumn("Details", ImGuiTableColumnFlags_WidthStretch);
  ImGui::TableSetupColumn("Hex", ImGuiTableColumnFlags_WidthFixed, 150);
  ImGui::TableSetupScrollFreeze(0, 1);
  ImGui::TableHeadersRow();

  // Highlight only the rows added by the *most recent* step. We key off the
  // global step counter (not our own row count) so that a step which adds no RDP
  // rows still clears the previous highlight. [hlStart, count) is the range
  // appended since that step — one command for our own stepper, a whole batch
  // when the other stepper runs, or empty if this step added nothing here.
  static u32 seenSeq = ~0u, prevCount = 0, hlStart = 0;
  u32 seq = program.stepSequence;
  bool stepped = seq != seenSeq;
  bool countChanged = count != prevCount;
  if(stepped) { hlStart = prevCount; seenSeq = seq; }
  if(count < hlStart) hlStart = 0;  // guard against the per-frame count reset
  // Anchor auto-scroll on the last row (GetScrollMaxY lags a frame).
  bool needScroll = anyStep && (stepped || countChanged);
  prevCount = count;
  u32 lastRow = count ? count - 1 : ~0u;

  for(u32 row = 0; row < count; row++) {
    auto& cmd = cap.commands[row];
    auto name = ares::Nintendo64::rdpCommandName(cmd.opcode);
    auto desc = ares::Nintendo64::rdpCommandDescription(cmd.opcode, cmd.word0);

    int lines = 1;
    for(char c : desc) if(c == '\n') lines++;

    ImGui::TableNextRow(ImGuiTableRowFlags_None, ImGui::GetTextLineHeight() * lines);

    // Highlight rows added since the last step (the emulator is paused on the last).
    if(anyStep && row >= hlStart) {
      ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(60, 60, 0, 255));
    }

    ImGui::TableNextColumn();
    ImGui::Text("%u", row);

    ImGui::TableNextColumn();
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(cmdColor(cmd.opcode)), "%s", name);

    ImGui::TableNextColumn();
    if(cmd.opcode >= 0x37 && cmd.opcode <= 0x3b) {
      u32 c = (u32)cmd.word0;
      u8 r = (c >> 24) & 0xff, g = (c >> 16) & 0xff, b = (c >> 8) & 0xff, a = c & 0xff;
      ImVec2 pos = ImGui::GetCursorScreenPos();
      float h  = ImGui::GetTextLineHeight();
      float sz = h - 2;
      ImDrawList* dl = ImGui::GetWindowDrawList();
      dl->AddRectFilled(pos, ImVec2(pos.x+sz, pos.y+sz), IM_COL32(r, g, b, 255));
      dl->AddRect(pos, ImVec2(pos.x+sz, pos.y+sz), IM_COL32(255,255,255,60));
      dl->AddRectFilled(ImVec2(pos.x+sz+3, pos.y), ImVec2(pos.x+sz*2+3, pos.y+sz), IM_COL32(a, a, a, 255));
      dl->AddRect(ImVec2(pos.x+sz+3, pos.y), ImVec2(pos.x+sz*2+3, pos.y+sz), IM_COL32(255,255,255,60));
      ImGui::SetCursorPosX(ImGui::GetCursorPosX() + sz * 2 + 6);
    }

    if(lines == 1) {
      ImGui::TextUnformatted(desc.data());
    } else {
      u32 lstart = 0;
      for(u32 i = 0; i <= desc.length(); i++) {
        if(i == desc.length() || desc[i] == '\n') {
          ImGui::TextUnformatted(desc.data() + lstart, desc.data() + i);
          lstart = i + 1;
        }
      }
    }

    ImGui::TableNextColumn();
    ImGui::Text("%016llX", (unsigned long long)cmd.word0);

    // Anchor the auto-scroll on the last row so we reach the very end.
    if(needScroll && row == lastRow) ImGui::SetScrollHereY(1.0f);
  }

  ImGui::EndTable();

  ImGui::End();
  settings.general.showRdpViewer = true;
}

}  // namespace ares::ui
