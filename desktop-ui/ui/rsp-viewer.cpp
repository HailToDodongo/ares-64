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

  // Toggle the synthetic internal rows (RSPQ Loop / Ucode Load / Buf Fetch)
  static bool showInternal = true;
  ImGui::SameLine();
  ImGui::Checkbox("Internal", &showInternal);

  // Toggle the statistics panel below the command table.
  static bool showStats = true;
  ImGui::SameLine();
  ImGui::Checkbox("Stats", &showStats);

  // Timing unit selector: microseconds (us) or raw RCP master-clock ticks.
  static int timeUnit = 0;  // 0 = us (default)
  ImGui::SameLine();
  ImGui::SetNextItemWidth(70);
  ImGui::Combo("##timeUnit", &timeUnit, "us\0Cycles\0");

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

  // Reserve room at the bottom for the statistics panel (when enabled).
  float statsH = 0.0f;
  if(showStats) {
    statsH = std::clamp(ImGui::GetContentRegionAvail().y * 0.34f, 140.0f, 320.0f);
  }

  if(!ImGui::BeginTable("rsp_cmds", 5,
       ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
       ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit,
       ImVec2(0, showStats ? -statsH : 0))) {
    ImGui::End();
    settings.general.showRspViewer = true;
    return;
  }

  float rowX0 = ImGui::GetCursorScreenPos().x;
  float rowW  = ImGui::GetContentRegionAvail().x;

  ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 45);
  ImGui::TableSetupColumn(timeUnit == 0 ? "us" : "Cycles", ImGuiTableColumnFlags_WidthFixed, 60);
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

  bool pendingSeparator = false;

  for(u32 row = 0; row < displayCount; row++) {
    auto& cmd = cap.commands[row];
    bool hidden = !showInternal && cmd.isOverhead;

    if(hidden) {
      if(cmd.isOverhead && cmd.overheadType == 2) pendingSeparator = true;
      continue;
    }

    // Insert a 2px orange separator row between overlay blocks.
    if(pendingSeparator) {
      pendingSeparator = false;
      ImGui::TableNextRow(ImGuiTableRowFlags_None, 2.0f);
      ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(0x77, 0x77, 0x77, 0xFF));
      for(int c = 1; c < 6; c++) ImGui::TableNextColumn();
    }

    // Highlight rows added since the last step.
    if(anyStep && row >= hlStart) {
      ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(60, 60, 0, 255));
    }

    ImGui::TableNextRow();

    ImGui::TableNextColumn();
    ImGui::Text("%u", row);

    ImGui::TableNextColumn();
    // cmd.cycle is in RCP master-clock ticks. In ares the RSP advances the shared
    // clock by 3 ticks per RSP cycle, so the master clock is 187.5 MHz (2x the
    // 93.75 MHz CPU) and the RSP runs at 62.5 MHz (libdragon RCP_FREQUENCY = 2/3
    // CPU). Hence 1 us = 187.5 ticks.
    static constexpr f64 ticksPerMicrosecond = 187.5;
    char timeBuf[32];
    if(timeUnit == 0) {
      f64 us = (f64)cmd.cycle / ticksPerMicrosecond;
      snprintf(timeBuf, sizeof(timeBuf), "%.2f", us);
    } else {
      snprintf(timeBuf, sizeof(timeBuf), "%llu", (unsigned long long)cmd.cycle);
    }
    // Right-align within the column (monospaced digits for clean numeric display).
    float colW = ImGui::GetColumnWidth();
    ImVec2 textSz = ImGui::CalcTextSize(timeBuf);
    float pad = ImGui::GetStyle().ItemSpacing.x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + colW - textSz.x - pad);
    if(cmd.isOverhead)
      ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1), "%s", timeBuf);
    else
      ImGui::Text("%s", timeBuf);

    // Internal/overhead rows (loop dispatch, overlay load, buffer fetch) are not
    // real commands, so don't decode them as ovl 0 / cmd 0.
    static const char* overheadNames[] = {"?", "RSPQ_Loop", "DMA ucode", "DMA cmd."};
    static const char* overheadDesc[] = {
      "",
      "RSPQ_Loop / command overhead",
      "Load/Save ucode + state",
      "DMA new commands",
    };
    const ImVec4 overheadCol(0.55f, 0.55f, 0.55f, 1);

    ImGui::TableNextColumn();
    if(cmd.isOverhead) {
      ImGui::TextColored(overheadCol, "RSPQ");
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
      // Custom per-command argument text from the JSON descriptor, if any.
      string desc = cap.formatArgs(cmd.overlayId, cmd.commandId, cmd.words, cmd.wordCount);
      if(!desc) {
        // No descriptor: fall back to raw hex words, shown dimmed.
        for(u32 i = 0; i < min<u32>(cmd.wordCount, 6); i++) {
          if(i > 0) desc.append(" ");
          desc.append(hex(cmd.words[i], 8L));
        }
        if(cmd.wordCount > 6) desc.append(" ...");
        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1), "%s", desc.data());
      } else {
        ImGui::TextUnformatted(desc.data());
      }
    }

    // If this row is a ucode switch, insert an orange separator after it.
    if(cmd.isOverhead && cmd.overheadType == 2) pendingSeparator = true;

    // Anchor the auto-scroll on the last visible row so we reach the very end.
    if(needScroll && row == lastVisibleRow) ImGui::SetScrollHereY(1.0f);
  }

  ImGui::EndTable();

  // ---- Statistics panel ------------------------------------------------------
  if(showStats) {
    static constexpr f64 ticksPerMicrosecond = 187.5;

    static u64 ovlTime[16]; static u32 ovlCnt[16];
    static u64 cmdTime[16][256]; static u32 cmdCnt[16][256];
    static u64 ohTime[4]; static u32 ohCnt[4];
    memset(ovlTime, 0, sizeof(ovlTime)); memset(ovlCnt, 0, sizeof(ovlCnt));
    memset(cmdTime, 0, sizeof(cmdTime)); memset(cmdCnt, 0, sizeof(cmdCnt));
    memset(ohTime, 0, sizeof(ohTime)); memset(ohCnt, 0, sizeof(ohCnt));

    u64 totalTime = 0;
    for(u32 i = 0; i < displayCount; i++) {
      auto& c = cap.commands[i];
      totalTime += c.cycle;
      if(c.isOverhead) {
        u8 t = c.overheadType < 4 ? c.overheadType : 0;
        ohTime[t] += c.cycle; ohCnt[t]++;
      } else {
        u16 o = c.overlayId & 15; u8 cm = c.commandId;
        ovlTime[o] += c.cycle; ovlCnt[o]++;
        cmdTime[o][cm] += c.cycle; cmdCnt[o][cm]++;
      }
    }

    auto fmtTime = [&](u64 cyc, char* buf, size_t n) {
      if(timeUnit == 0) snprintf(buf, n, "%.2f", (f64)cyc / ticksPerMicrosecond);
      else              snprintf(buf, n, "%llu", (unsigned long long)cyc);
    };
    f64 invTotal = totalTime ? 100.0 / (f64)totalTime : 0.0;

    auto numCell = [&](const char* text) {
      float colW = ImGui::GetColumnWidth();
      float tw = ImGui::CalcTextSize(text).x;
      float pad = ImGui::GetStyle().ItemSpacing.x;
      ImGui::SetCursorPosX(ImGui::GetCursorPosX() + colW - tw - pad);
      ImGui::TextUnformatted(text);
    };

    static const char* ohNames[] = {"?", "RSPQ_Loop", "DMA ucode", "DMA cmd."};
    const char* timeHdr = timeUnit == 0 ? "us" : "Cycles";

    ImGui::BeginChild("##stats", ImVec2(0, 0), ImGuiChildFlags_None);
    if(ImGui::BeginTabBar("##statTabs")) {

      // --- By Overlay ------------------------------------------------------
      if(ImGui::BeginTabItem("By Overlay")) {
        struct Row { string label; ImU32 color; u32 count; u64 time; };
        std::vector<Row> rows;
        for(u32 o = 0; o < 16; o++) {
          if(!ovlCnt[o]) continue;
          string lbl = cap.overlayNameMap[o] ? cap.overlayNameMap[o] : string{hex(o)};
          rows.push_back({lbl, overlayColor(o), ovlCnt[o], ovlTime[o]});
        }
        for(u32 t = 1; t < 4; t++) {
          if(!ohCnt[t]) continue;
          rows.push_back({string{ohNames[t]}, IM_COL32(150, 150, 150, 255), ohCnt[t], ohTime[t]});
        }
        std::sort(rows.begin(), rows.end(), [](auto& a, auto& b) { return a.time > b.time; });

        if(ImGui::BeginTable("##ovlStats", 4,
             ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY)) {
          ImGui::TableSetupColumn("Overlay", ImGuiTableColumnFlags_WidthStretch);
          ImGui::TableSetupColumn("Count", ImGuiTableColumnFlags_WidthFixed, 60);
          ImGui::TableSetupColumn(timeHdr, ImGuiTableColumnFlags_WidthFixed, 80);
          ImGui::TableSetupColumn("%", ImGuiTableColumnFlags_WidthFixed, 55);
          ImGui::TableSetupScrollFreeze(0, 1);
          ImGui::TableHeadersRow();

          char buf[32];
          for(auto& r : rows) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(r.color), "%s", r.label.data());
            ImGui::TableNextColumn(); snprintf(buf, sizeof(buf), "%u", r.count); numCell(buf);
            ImGui::TableNextColumn(); fmtTime(r.time, buf, sizeof(buf)); numCell(buf);
            ImGui::TableNextColumn(); snprintf(buf, sizeof(buf), "%.1f", (f64)r.time * invTotal); numCell(buf);
          }
          ImGui::TableNextRow();
          ImGui::TableNextColumn(); ImGui::TextDisabled("Total");
          ImGui::TableNextColumn(); snprintf(buf, sizeof(buf), "%u", displayCount); numCell(buf);
          ImGui::TableNextColumn(); fmtTime(totalTime, buf, sizeof(buf)); numCell(buf);
          ImGui::TableNextColumn(); numCell("100.0");
          ImGui::EndTable();
        }
        ImGui::EndTabItem();
      }

      // --- By Command -------------------------------------------------------
      if(ImGui::BeginTabItem("By Command")) {
        struct Row { string ovl; ImU32 color; string cmd; u32 count; u64 time; };
        std::vector<Row> rows;
        for(u32 o = 0; o < 16; o++) {
          for(u32 cm = 0; cm < 256; cm++) {
            if(!cmdCnt[o][cm]) continue;
            string ovl = cap.overlayNameMap[o] ? cap.overlayNameMap[o] : string{hex(o)};
            string cmd = cap.commandNameMap[o][cm] ? cap.commandNameMap[o][cm] : string{hex(cm, 2L)};
            rows.push_back({ovl, overlayColor(o), cmd, cmdCnt[o][cm], cmdTime[o][cm]});
          }
        }
        std::sort(rows.begin(), rows.end(), [](auto& a, auto& b) { return a.time > b.time; });

        if(ImGui::BeginTable("##cmdStats", 5,
             ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY)) {
          ImGui::TableSetupColumn("Overlay", ImGuiTableColumnFlags_WidthFixed, 90);
          ImGui::TableSetupColumn("Command", ImGuiTableColumnFlags_WidthStretch);
          ImGui::TableSetupColumn("Count", ImGuiTableColumnFlags_WidthFixed, 60);
          ImGui::TableSetupColumn(timeHdr, ImGuiTableColumnFlags_WidthFixed, 80);
          ImGui::TableSetupColumn("%", ImGuiTableColumnFlags_WidthFixed, 55);
          ImGui::TableSetupScrollFreeze(0, 1);
          ImGui::TableHeadersRow();

          char buf[32];
          for(auto& r : rows) {
            ImVec4 col = ImGui::ColorConvertU32ToFloat4(r.color);
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextColored(col, "%s", r.ovl.data());
            ImGui::TableNextColumn(); ImGui::TextColored(col, "%s", r.cmd.data());
            ImGui::TableNextColumn(); snprintf(buf, sizeof(buf), "%u", r.count); numCell(buf);
            ImGui::TableNextColumn(); fmtTime(r.time, buf, sizeof(buf)); numCell(buf);
            ImGui::TableNextColumn(); snprintf(buf, sizeof(buf), "%.1f", (f64)r.time * invTotal); numCell(buf);
          }
          ImGui::EndTable();
        }
        ImGui::EndTabItem();
      }

      ImGui::EndTabBar();
    }
    ImGui::EndChild();
  }

  ImGui::End();
  settings.general.showRspViewer = true;
}

}  // namespace ares::ui
