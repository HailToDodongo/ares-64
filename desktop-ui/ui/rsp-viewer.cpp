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

  // Toggle for the synthetic internal rows (RSPQ Loop / Ucode Load / Buf Fetch)
  static bool showInternal = true;
  ImGui::SameLine();
  ImGui::Checkbox("Internal", &showInternal);

  // Toggle the timing column between raw ticks and microseconds.
  static bool showMicros = false;
  ImGui::SameLine();
  ImGui::Checkbox("us", &showMicros);

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

  // Step mode indicator for display logic. We react to *either* stepper (RSP or
  // RDP): when the other one steps, new rows may be appended here too, and we
  // want to follow + highlight them just the same.
  bool anyStep = (program.stepType == Program::StepType::RSP)
              || (program.stepType == Program::StepType::RDP);

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

  if(!ImGui::BeginTable("rsp_cmds", 5,
       ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
       ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit,
       ImVec2(0, 0))) {
    ImGui::End();
    settings.general.showRspViewer = true;
    return;
  }
  ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 45);
  ImGui::TableSetupColumn(showMicros ? "us" : "Cycles", ImGuiTableColumnFlags_WidthFixed, 60);
  ImGui::TableSetupColumn("Ovl", ImGuiTableColumnFlags_WidthFixed, 35);
  ImGui::TableSetupColumn("Command", ImGuiTableColumnFlags_WidthFixed, 140);
  ImGui::TableSetupColumn("Data", ImGuiTableColumnFlags_WidthStretch);
  ImGui::TableSetupScrollFreeze(0, 1);
  ImGui::TableHeadersRow();

  // Highlight only the rows added by the *most recent* step. We key off the
  // global step counter (not our own row count) so that a step which adds no RSP
  // rows still clears the previous highlight. [hlStart, displayCount) is the
  // range appended since that step — one command for our own stepper, a whole
  // batch when the other stepper runs, or empty if this step added nothing here.
  static u32 seenSeq = ~0u, prevCount = 0, hlStart = 0;
  u32 seq = program.stepSequence;
  bool stepped = seq != seenSeq;
  bool countChanged = displayCount != prevCount;
  if(stepped) { hlStart = prevCount; seenSeq = seq; }
  if(displayCount < hlStart) hlStart = 0;  // guard against the per-frame count reset
  // Anchor auto-scroll on the last visible row (GetScrollMaxY lags a frame).
  bool needScroll = anyStep && (stepped || countChanged);
  prevCount = displayCount;

  // Find the last row that will actually be rendered (respecting the filter).
  u32 lastVisibleRow = ~0u;
  for(s32 r = (s32)displayCount - 1; r >= 0; r--) {
    if(showInternal || !cap.commands[r].isOverhead) { lastVisibleRow = (u32)r; break; }
  }

  for(u32 row = 0; row < displayCount; row++) {
    auto& cmd = cap.commands[row];
    if(!showInternal && cmd.isOverhead) continue;

    ImGui::TableNextRow();

    // Highlight rows added since the last step (the emulator is paused on the
    // last of these). Same look as the RDP viewer.
    if(anyStep && row >= hlStart) {
      ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(60, 60, 0, 255));
    }

    ImGui::TableNextColumn();
    ImGui::Text("%u", row);

    ImGui::TableNextColumn();
    // cmd.cycle is in RCP master-clock ticks. In ares the RSP advances the shared
    // clock by 3 ticks per RSP cycle, so the master clock is 187.5 MHz (2x the
    // 93.75 MHz CPU) and the RSP runs at 62.5 MHz (libdragon RCP_FREQUENCY = 2/3
    // CPU). Hence 1 us = 187.5 ticks.
    static constexpr f64 ticksPerMicrosecond = 187.5;
    if(showMicros) {
      f64 us = (f64)cmd.cycle / ticksPerMicrosecond;
      if(cmd.isOverhead) ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1), "(%.2f)", us);
      else               ImGui::Text("%.2f", us);
    } else {
      if(cmd.isOverhead) ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1), "(%llu)", (unsigned long long)cmd.cycle);
      else               ImGui::Text("%llu", (unsigned long long)cmd.cycle);
    }

    // Internal/overhead rows (loop dispatch, overlay load, buffer fetch) are not
    // real commands, so don't decode them as ovl 0 / cmd 0.
    static const char* overheadNames[] = {"?", "RSPQ_Loop", "ucode DMA", "queue DMA"};
    static const char* overheadDesc[] = {
      "",
      "time in RSPQ_Loop dispatch (outside commands)",
      "time saving/loading overlay ucode + state",
      "time DMAing next command buffer from RDRAM",
    };
    const ImVec4 overheadCol(0.55f, 0.55f, 0.55f, 1);

    ImGui::TableNextColumn();
    if(cmd.isOverhead) {
      ImGui::TextColored(overheadCol, "<internal>");
    } else {
      auto& ovlName = cap.overlayNameMap[cmd.overlayId];
      if(ovlName) {
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(overlayColor(cmd.overlayId)),
                           "%s", ovlName.data());
      } else {
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(overlayColor(cmd.overlayId)),
                           "%X", cmd.overlayId);
      }
    }

    ImGui::TableNextColumn();
    if(cmd.isOverhead) {
      ImGui::TextColored(overheadCol, "%s",
        overheadNames[cmd.overheadType < 4 ? cmd.overheadType : 0]);
    } else {
      auto& name = cap.commandNameMap[cmd.overlayId][cmd.commandId];
      if(name) {
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(overlayColor(cmd.overlayId)),
                           "%s", name.data());
      } else {
        ImGui::Text("%02X", cmd.commandId);
      }
    }

    ImGui::TableNextColumn();
    if(cmd.isOverhead) {
      ImGui::TextColored(overheadCol, "%s",
        overheadDesc[cmd.overheadType < 4 ? cmd.overheadType : 0]);
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

    // Anchor the auto-scroll on the last visible row so we reach the very end.
    if(needScroll && row == lastVisibleRow) ImGui::SetScrollHereY(1.0f);
  }

  ImGui::EndTable();
  ImGui::End();
  settings.general.showRspViewer = true;
}

}  // namespace ares::ui
