#include "ui.hpp"

#include "../desktop-ui.hpp"
#include <n64/n64.hpp>

namespace ares::ui {

bool showRspViewer = false;

static auto overlayColor(u8 overlayId) -> ImU32 {
  static const ImU32 colors[] = {
    IM_COL32(128, 128, 128, 255), // 0: builtin (gray)
    IM_COL32(100, 200, 255, 255), // 1: blue
    IM_COL32(100, 255, 150, 255), // 2: green
    IM_COL32(255, 255, 100, 255), // 3: yellow
    IM_COL32(255, 180, 100, 255), // 4: orange
    IM_COL32(255, 150, 200, 255), // 5: pink
    IM_COL32(180, 130, 255, 255), // 6: purple
    IM_COL32(255, 120, 120, 255), // 7: red
  };
  return colors[overlayId & 7];
}

auto DrawRspViewer() -> void {
  if(!showRspViewer) return;

  ImGui::SetNextWindowSize(ImVec2(700, 400), ImGuiCond_FirstUseEver);
  if(!ImGui::Begin("RSP Commands", &showRspViewer)) {
    ImGui::End();
    settings.general.showRspViewer = false;
    return;
  }

  if(!emulator || emulator->name != "Nintendo 64") {
    ImGui::TextUnformatted("RSP viewer only available for Nintendo 64.");
    ImGui::End();
    settings.general.showRspViewer = true;
    return;
  }

  auto& cap = ares::Nintendo64::rsp.capture;

  // Status line
  bool enabled = cap.enabled.load(std::memory_order_relaxed);
  if(ImGui::Checkbox("Capture", &enabled)) {
    cap.enabled.store(enabled, std::memory_order_relaxed);
  }

  // Config status
  ImGui::SameLine();
  if(!cap.configLoaded) {
    ImGui::TextColored(ImVec4(1, 0.5f, 0, 1), "No config");
  } else {
    ImGui::TextColored(ImVec4(0, 1, 0, 1), "Ready");
  }

  // Command count
  ImGui::SameLine();
  u32 liveCount = cap.writePos.load(std::memory_order_acquire);
  u32 committed = cap.committedCount.load(std::memory_order_acquire);
  if(liveCount > cap.maxCommands) liveCount = cap.maxCommands;
  ImGui::Text("Live: %u  |  Frame: %u", liveCount, committed);

  // Step button — shown when RSP stepping is the active mode
  bool isRspStep = (program.stepType == Program::StepType::RSP);
  if(isRspStep) {
    ImGui::SameLine();
    if(ImGui::Button("Step >")) {
      cap.stepPending.store(true, std::memory_order_release);
    }
    // Hold-to-repeat: 400ms initial delay, then fire every frame
    static bool steppingHeld = false;
    static auto stepHoldStart = std::chrono::steady_clock::now();
    if(ImGui::IsItemActive()) {
      if(!steppingHeld) { steppingHeld = true; stepHoldStart = std::chrono::steady_clock::now(); }
      auto elapsed = std::chrono::steady_clock::now() - stepHoldStart;
      if(elapsed > std::chrono::milliseconds(400)) {
        cap.stepPending.store(true, std::memory_order_release);
      }
    } else {
      steppingHeld = false;
    }
  }

  // Step mode indicator for display logic
  bool sm = isRspStep;

  ImGui::Separator();

  // Always show live data — writePos is the authoritative count
  u32 displayCount = liveCount;
  if(displayCount > cap.maxCommands) displayCount = cap.maxCommands;
  if(displayCount == 0 && committed > 0) displayCount = committed;

  if(!cap.configLoaded) {
    ImGui::TextUnformatted("No RSPQ config detected. Load a libdragon ROM with an ELF file alongside it.");
    ImGui::End();
    settings.general.showRspViewer = true;
    return;
  }

  if(displayCount == 0 && liveCount == 0) {
    ImGui::TextUnformatted("Waiting for RSP commands...");
    ImGui::End();
    settings.general.showRspViewer = true;
    return;
  }

  if(displayCount == 0) {
    ImGui::End();
    settings.general.showRspViewer = true;
    return;
  }

  if(!ImGui::BeginTable("rsp_cmds", 7,
       ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
       ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit,
       ImVec2(0, 0))) {
    ImGui::End();
    settings.general.showRspViewer = true;
    return;
  }
  ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 45);
  ImGui::TableSetupColumn("Frame", ImGuiTableColumnFlags_WidthFixed, 50);
  ImGui::TableSetupColumn("Cycles", ImGuiTableColumnFlags_WidthFixed, 70);
  ImGui::TableSetupColumn("Ovl", ImGuiTableColumnFlags_WidthFixed, 30);
  ImGui::TableSetupColumn("Cmd", ImGuiTableColumnFlags_WidthFixed, 30);
  ImGui::TableSetupColumn("Data", ImGuiTableColumnFlags_WidthStretch);
  ImGui::TableSetupColumn("Hex words", ImGuiTableColumnFlags_WidthFixed, 280);
  ImGui::TableSetupScrollFreeze(0, 1);
  ImGui::TableHeadersRow();

  for(u32 row = 0; row < displayCount; row++) {
    auto& cmd = cap.commands[row];

    ImGui::TableNextRow();

    ImGui::TableNextColumn();
    ImGui::Text("%u", row);

    ImGui::TableNextColumn();
    ImGui::Text("%u", cmd.frame);

    ImGui::TableNextColumn();
    if(cmd.isOverhead) {
      ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1), "(%llu)", (unsigned long long)cmd.cycle);
    } else {
      ImGui::Text("%llu", (unsigned long long)cmd.cycle);
    }

    ImGui::TableNextColumn();
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(overlayColor(cmd.overlayId)),
                       "%X", cmd.overlayId);

    ImGui::TableNextColumn();
    ImGui::Text("%02X", cmd.commandId);

    ImGui::TableNextColumn();
    if(cmd.isOverhead) {
      const char* overheadNames[] = {"?", "Wait CPU", "Ovl Switch", "Buf Fetch"};
      ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1), "[%s]",
        overheadNames[cmd.overheadType < 4 ? cmd.overheadType : 0]);
    } else {
      string desc = {"ovl:", hex(cmd.overlayId, 1L), " cmd:", hex(cmd.commandId, 2L)};
      if(cmd.wordCount > 0) {
        desc.append(" args: ");
        for(u32 i = 0; i < min<u32>(cmd.wordCount, 4); i++) {
          if(i > 0) desc.append(", ");
          desc.append(hex(cmd.words[i], 8L));
        }
        if(cmd.wordCount > 4) desc.append(", ...");
      }
      ImGui::TextUnformatted(desc.data());
    }

    ImGui::TableNextColumn();
    if(!cmd.isOverhead) {
      string hexStr;
      for(u32 i = 0; i < min<u32>(cmd.wordCount, 8); i++) {
        if(i > 0) hexStr.append(" ");
        hexStr.append(hex(cmd.words[i], 8L));
      }
      if(cmd.wordCount > 8) hexStr.append(" ...");
      ImGui::TextUnformatted(hexStr.data());
    }
  }

  // Auto-scroll to bottom when stepping
  static u32 lastDisplayCount = ~0u;
  if(sm && displayCount != lastDisplayCount) {
    ImGui::SetScrollY(ImGui::GetScrollMaxY());
    lastDisplayCount = displayCount;
  }

  ImGui::EndTable();
  ImGui::End();
  settings.general.showRspViewer = true;
}

}  // namespace ares::ui
