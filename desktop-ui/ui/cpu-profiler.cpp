#include "ui.hpp"

#include "../desktop-ui.hpp"
#include <n64/n64.hpp>

#include <algorithm>
#include <vector>

namespace ares::ui {

bool showCpuProfiler = false;

// CPU master clock: cpu.profile cycles advance 2 per CPU cycle, i.e. 187.5 MHz,
// matching the RSP viewer's tick rate. 1 us = 187.5 ticks.
static constexpr f64 ticksPerMicrosecond = 187.5;

auto DrawCpuProfiler() -> void {
  bool isN64 = emulator && emulator->name == "Nintendo 64";

  // Auto-enable capture while the window is open, and stop it on close so the
  // JIT returns to full speed. A manual Capture toggle is still respected for as
  // long as the window stays open (we only force-enable on the open edge).
  static bool autoCaptureActive = false;
  if(!showCpuProfiler) {
    if(autoCaptureActive && isN64) ares::Nintendo64::cpu.profiler.setEnabled(false);
    autoCaptureActive = false;
    return;
  }

  ImGui::SetNextWindowSize(ImVec2(720, 440), ImGuiCond_FirstUseEver);
  if(!ImGui::Begin("CPU Profiler", &showCpuProfiler)) {
    ImGui::End();
    settings.general.showCpuProfiler = showCpuProfiler;
    return;
  }

  if(!emulator || emulator->name != "Nintendo 64") {
    ImGui::TextUnformatted("CPU profiler only available for Nintendo 64.");
    ImGui::End();
    settings.general.showCpuProfiler = true;
    return;
  }

  auto& prof = ares::Nintendo64::cpu.profiler;

  // Force capture on the first frame the window is open (default-on behaviour).
  if(!autoCaptureActive) {
    prof.setEnabled(true);
    autoCaptureActive = true;
  }

  // --- controls ---------------------------------------------------------------
  bool enabled = prof.enabled.load(std::memory_order_relaxed);
  if(ImGui::Checkbox("Capture", &enabled)) {
    prof.setEnabled(enabled);  // toggles the per-instruction JIT hook
  }

  static int timeUnit = 0;  // 0 = us, 1 = ms, 2 = Cycles
  ImGui::SameLine();
  ImGui::SetNextItemWidth(70);
  ImGui::Combo("##timeUnit", &timeUnit, "us\0ms\0Cycles\0");

  static int windowMode = 1;  // 0 = Continuous, 1 = Per-frame
  ImGui::SameLine();
  ImGui::SetNextItemWidth(110);
  ImGui::Combo("##window", &windowMode, "Continuous\0Per-frame\0");

  // In continuous mode, choose between accumulated totals and per-frame averages,
  // and show how many frames have been accumulated (capped at maxFrames).
  static int contMode = 1;
  if(windowMode == 0) {
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110);
    ImGui::Combo("##contMode", &contMode, "Total\0Avg/frame\0");

    ImGui::SameLine();
    u64 fc = prof.frameCount;
    bool capped = fc >= ares::Nintendo64::CPU::Profiler::maxFrames;
    ImGui::TextColored(capped ? ImVec4(1.0f, 0.65f, 0.3f, 1) : ImGui::GetStyle().Colors[ImGuiCol_TextDisabled],
                       "%llu/%u%s", (unsigned long long)fc,
                       ares::Nintendo64::CPU::Profiler::maxFrames, capped ? " (full)" : "");
  }

  ImGui::SameLine();
  if(ImGui::Button("Clear")) {
    // The emulator cothread is parked while the UI draws (same thread that
    // already reads these maps below), so clear immediately — works whether the
    // game is running or paused, and in either window mode.
    prof.clearStats();
  }

  ImGui::SameLine();
  if(prof.symbolsLoaded) {
    //ImGui::TextColored(ImVec4(0, 1, 0, 1), "Symbols: %u", prof.symbolCount);
  } else {
    ImGui::TextColored(ImVec4(1, 0.5f, 0, 1), "No ELF");
  }

  // Name filter — hides non-matching rows. Percentages still use the unfiltered
  // total (computed below over every function, before filtering at render time).
  // On its own line so it stays visible when the window is narrow.
  static char filterBuf[64] = "";
  ImGui::InputTextWithHint("##filter", "Filter by name...", filterBuf, sizeof(filterBuf));

  ImGui::Separator();

  if(!enabled && prof.stats.empty() && prof.frameStats.empty()) {
    ImGui::TextUnformatted("Enable 'Capture' to profile CPU function costs.\n"
                           "Tip: load a libdragon ROM with its .elf alongside for function names.");
    ImGui::End();
    settings.general.showCpuProfiler = true;
    return;
  }

  // Snapshot the active map (continuous totals or the last completed frame).
  auto& srcMap = (windowMode == 0) ? prof.stats : prof.frameStats;

  struct Row { u32 addr; string name; bool isSpin; bool isExc; u64 calls; u64 incl; u64 excl; u64 wait; };
  std::vector<Row> rows;
  rows.reserve(srcMap.size());
  u64 totalExcl = 0, spinExcl = 0;
  for(auto& [addr, st] : srcMap) {
    bool isExc = prof.isExceptionAddr(addr);
    string name = prof.labelFor(addr);
    bool spin = !isExc && st.isSpin;
    rows.push_back({addr, name, spin, isExc, st.callCount, st.inclCycles, st.exclCycles, st.waitCycles});
    totalExcl += st.exclCycles;
    if(spin) spinExcl += st.exclCycles;
  }

  // (rows are sorted below according to the table's clickable column headers)

  
  bool avgMode = (windowMode == 0) && (contMode == 1);
  f64 divisor = (avgMode && prof.frameCount > 0) ? (f64)prof.frameCount : 1.0;

  auto fmtTime = [&](f64 cyc, char* buf, size_t n) {
    if(timeUnit == 0)      snprintf(buf, n, "%.2f", cyc / ticksPerMicrosecond);
    else if(timeUnit == 1) snprintf(buf, n, "%.2f", cyc / (ticksPerMicrosecond * 1000.0));
    else                   snprintf(buf, n, "%.0f", cyc);
  };
  auto fmtCount = [&](u64 raw, char* buf, size_t n) {
    if(avgMode) snprintf(buf, n, "%.2f", (f64)raw / divisor);
    else        snprintf(buf, n, "%llu", (unsigned long long)raw);
  };
  auto numCell = [&](const char* text) {
    ImGui::PushFont(monoFont);
    float colW = ImGui::GetColumnWidth();
    float tw = ImGui::CalcTextSize(text).x;
    float pad = ImGui::GetStyle().ItemSpacing.x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + colW - tw - pad);
    ImGui::TextUnformatted(text);
    ImGui::PopFont();
  };

  const char* timeHdr = timeUnit == 0 ? "us" : timeUnit == 1 ? "ms" : "Cycles";
  f64 invTotal = totalExcl ? 100.0 / (f64)totalExcl : 0.0;

  // Reserve room at the bottom for the summary panel.
  float statsH = std::clamp(ImGui::GetContentRegionAvail().y * 0.30f, 120.0f, 260.0f);

  // --- main function table ----------------------------------------------------
  if(ImGui::BeginTable("cpu_funcs", 7,
       ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Sortable |
       ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit,
       ImVec2(0, -statsH))) {
    ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort, 40);
    ImGui::TableSetupColumn("Function", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Calls", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_PreferSortDescending, 70);
    ImGui::TableSetupColumn("Excl", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_PreferSortDescending | ImGuiTableColumnFlags_DefaultSort, 80);
    ImGui::TableSetupColumn("Incl", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_PreferSortDescending, 80);
    ImGui::TableSetupColumn("Wait", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_PreferSortDescending, 80);
    ImGui::TableSetupColumn("%", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort, 55);
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableHeadersRow();

    // Sort the freshly-rebuilt rows to match the active column header. Done every
    // frame (not just on SpecsDirty) because the row data is regenerated above.
    if(ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs(); specs && specs->SpecsCount > 0) {
      auto& s = specs->Specs[0];
      bool asc = s.SortDirection == ImGuiSortDirection_Ascending;
      std::sort(rows.begin(), rows.end(), [&](const Row& a, const Row& b) {
        switch(s.ColumnIndex) {
        case 1:  return asc ? (a.name < b.name) : (b.name < a.name);
        case 2:  return asc ? a.calls < b.calls : a.calls > b.calls;
        case 4:  return asc ? a.incl  < b.incl  : a.incl  > b.incl;
        case 5:  return asc ? a.wait  < b.wait  : a.wait  > b.wait;
        case 3:
        default: return asc ? a.excl  < b.excl  : a.excl  > b.excl;
        }
      });
    }

    char buf[32];
    u32 shown = 0;
    for(auto& r : rows) {
      // Case-insensitive name filter; hidden rows still counted in the total/%.
      if(filterBuf[0] && !(bool)r.name.ifind(filterBuf)) continue;
      ImGui::TableNextRow();

      ImGui::TableNextColumn();
      ImGui::PushFont(monoFont);
      ImGui::Text("%u", shown++);
      ImGui::PopFont();

      ImGui::TableNextColumn();
      if(r.isExc)       ImGui::TextColored(ImVec4(0.45f, 0.8f, 1.0f, 1), "%s", r.name.data());  //interrupt/exception
      else if(r.isSpin) ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.3f, 1), "%s", r.name.data());  //spin/wait
      else              ImGui::TextUnformatted(r.name.data());

      ImGui::TableNextColumn(); fmtCount(r.calls, buf, sizeof(buf)); numCell(buf);
      ImGui::TableNextColumn(); fmtTime(r.excl / divisor, buf, sizeof(buf)); numCell(buf);
      ImGui::TableNextColumn(); fmtTime(r.incl / divisor, buf, sizeof(buf)); numCell(buf);
      // Wait: inclusive time spent in spin/wait functions inside this call's
      // subtree (highlighted when non-zero).
      ImGui::TableNextColumn();
      fmtTime(r.wait / divisor, buf, sizeof(buf));
      if(r.wait) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.65f, 0.3f, 1));
      numCell(buf);
      if(r.wait) ImGui::PopStyleColor();
      ImGui::TableNextColumn(); snprintf(buf, sizeof(buf), "%.1f", (f64)r.excl * invTotal); numCell(buf);
    }
    ImGui::EndTable();
  }

  // --- summary panel ----------------------------------------------------------
  ImGui::BeginChild("##cpuStats", ImVec2(0, 0), ImGuiChildFlags_None);
  if(ImGui::BeginTabBar("##cpuStatTabs")) {

    if(ImGui::BeginTabItem("Summary")) {
      f64 invT = totalExcl ? 100.0 / (f64)totalExcl : 0.0;
      u64 activeExcl = totalExcl - spinExcl;
      char buf[32];

      if(ImGui::BeginTable("##cpuSummary", 3,
           ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Category", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(timeHdr, ImGuiTableColumnFlags_WidthFixed, 90);
        ImGui::TableSetupColumn("%", ImGuiTableColumnFlags_WidthFixed, 60);
        ImGui::TableHeadersRow();

        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextUnformatted("Active CPU");
        ImGui::TableNextColumn(); fmtTime(activeExcl / divisor, buf, sizeof(buf)); numCell(buf);
        ImGui::TableNextColumn(); snprintf(buf, sizeof(buf), "%.1f", (f64)activeExcl * invT); numCell(buf);

        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        if(prof.symbolsLoaded) ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.3f, 1), "Spin / Wait");
        else                   ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1), "Spin / Wait (needs ELF)");
        ImGui::TableNextColumn();
        if(prof.symbolsLoaded) { fmtTime(spinExcl / divisor, buf, sizeof(buf)); numCell(buf); }
        else                   { numCell("N/A"); }
        ImGui::TableNextColumn();
        if(prof.symbolsLoaded) { snprintf(buf, sizeof(buf), "%.1f", (f64)spinExcl * invT); numCell(buf); }
        else                   { numCell("-"); }

        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextDisabled(avgMode ? "Per-frame total" : "Profiled total");
        ImGui::TableNextColumn(); fmtTime(totalExcl / divisor, buf, sizeof(buf)); numCell(buf);
        ImGui::TableNextColumn(); numCell("100.0");
        ImGui::EndTable();
      }
      ImGui::EndTabItem();
    }

    if(ImGui::BeginTabItem("Top functions")) {
      if(ImGui::BeginTable("##cpuTop", 4,
           ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY)) {
        ImGui::TableSetupColumn("Function", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Calls", ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn(timeHdr, ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn("%", ImGuiTableColumnFlags_WidthFixed, 55);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        // rows are already sorted; show top entries by exclusive time.
        std::vector<const Row*> top;
        for(auto& r : rows) top.push_back(&r);
        std::sort(top.begin(), top.end(), [](auto* a, auto* b){ return a->excl > b->excl; });
        char buf[32];
        u32 shown = 0;
        for(auto* r : top) {
          if(shown++ >= 20) break;
          ImGui::TableNextRow();
          ImGui::TableNextColumn();
          if(r->isSpin) ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.3f, 1), "%s", r->name.data());
          else          ImGui::TextUnformatted(r->name.data());
          ImGui::TableNextColumn(); fmtCount(r->calls, buf, sizeof(buf)); numCell(buf);
          ImGui::TableNextColumn(); fmtTime(r->excl / divisor, buf, sizeof(buf)); numCell(buf);
          ImGui::TableNextColumn(); snprintf(buf, sizeof(buf), "%.1f", (f64)r->excl * invTotal); numCell(buf);
        }
        ImGui::EndTable();
      }
      ImGui::EndTabItem();
    }

    ImGui::EndTabBar();
  }
  ImGui::EndChild();

  ImGui::End();
  settings.general.showCpuProfiler = true;
}

}  // namespace ares::ui
