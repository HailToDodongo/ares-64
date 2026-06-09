#include "ui.hpp"

#include "../desktop-ui.hpp"
#include <n64/n64.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <vector>

namespace ares::ui {

bool showFlameChart = false;

// Master clock: 187.5 ticks per microsecond (matches the CPU profiler / RSP viewer).
static constexpr f64 ticksPerMicrosecond = 187.5;

using Span = ares::Nintendo64::CPU::Profiler::Span;

// A flattened RSP command span, window-relative (tick 0 = window start).
struct RspSpan {
  u64 start = 0, end = 0;
  u16 overlayId = 0;
  u8  commandId = 0;
  bool overhead = false;
  u8  overheadType = 0;
};

// A flattened RDP "DP flush" block, window-relative. The RDP has no per-command
// wall-clock timing, so each flush is one block anchored at its real submit time;
// its width is synthetic (command count * a nominal per-command tick budget).
struct RdpSpan {
  u64 start = 0, end = 0;
  u32 count = 0;
};

// Stable per-function color from its address (golden-ratio hue hash).
static auto spanColor(u32 addr, bool isException) -> ImU32 {
  if(isException) return IM_COL32(120, 120, 130, 255);  //handlers: muted gray-blue
  u32 h = addr * 2654435761u;                            //Knuth multiplicative hash
  f32 hue = (h >> 8) / (f32)0x00ffffff;
  f32 r, g, b;
  ImGui::ColorConvertHSVtoRGB(hue, 0.55f, 0.85f, r, g, b);
  return IM_COL32((int)(r * 255), (int)(g * 255), (int)(b * 255), 255);
}

// RSP overlay palette (mirrors the RSP viewer's overlayColor).
static auto rspOverlayColor(u8 overlayId) -> ImU32 {
  static const ImU32 colors[] = {
    IM_COL32(128, 128, 128, 255), IM_COL32(100, 200, 255, 255),
    IM_COL32(100, 255, 150, 255), IM_COL32(255, 255, 100, 255),
    IM_COL32(255, 180, 100, 255), IM_COL32(255, 150, 200, 255),
    IM_COL32(180, 130, 255, 255), IM_COL32(255, 120, 120, 255),
  };
  return colors[overlayId & 7];
}

static auto rspLabel(u8 overheadType, bool overhead, u16 overlayId, u8 commandId) -> string {
  static const char* overheadNames[] = {"?", "RSPQ_Loop", "DMA ucode", "DMA cmd."};
  auto& rcap = ares::Nintendo64::rsp.capture;
  if(overhead) return string{overheadNames[overheadType < 4 ? overheadType : 0]};
  auto& name = rcap.commandNameMap[overlayId & 15][commandId];
  if(name) return name;
  return string{"ovl", hex(overlayId, 1L), ":", hex(commandId, 2L)};
}

static auto fmtTime(f64 ticks, char* buf, size_t n) -> void {
  f64 us = ticks / ticksPerMicrosecond;
  if(us < 1000.0) snprintf(buf, n, "%.2f us", us);
  else            snprintf(buf, n, "%.3f ms", us / 1000.0);
}

// Signed elapsed time (for the measurement marker -> cursor delta readout).
static auto fmtDelta(f64 ticks, char* buf, size_t n) -> void {
  char sign = ticks < 0 ? '-' : '+';
  f64 us = std::abs(ticks) / ticksPerMicrosecond;
  if(us < 1000.0) snprintf(buf, n, "%c%.2f us", sign, us);
  else            snprintf(buf, n, "%c%.3f ms", sign, us / 1000.0);
}

auto DrawFlameChart() -> void {
  bool isN64 = emulator && emulator->name == "Nintendo 64";

  // Auto-enable capture + timeline recording while the window is open. The
  // profiler "enabled" flag is shared with the CPU Profiler window, so only
  // tear it down on close when that window is also closed.
  static bool autoActive = false;
  if(!showFlameChart) {
    if(autoActive && isN64) {
      ares::Nintendo64::cpu.profiler.recordTimeline.store(false, std::memory_order_relaxed);
      if(!showCpuProfiler) ares::Nintendo64::cpu.profiler.setEnabled(false);
      // Leave RSP capture running if the RSP viewer is still open (it owns the toggle).
      if(!showRspViewer) ares::Nintendo64::rsp.capture.enabled.store(false, std::memory_order_relaxed);
    }
    autoActive = false;
    return;
  }

  ImGui::SetNextWindowSize(ImVec2(960, 620), ImGuiCond_FirstUseEver);
  if(!ImGui::Begin("Flame Chart", &showFlameChart)) {
    ImGui::End();
    settings.general.showFlameChart = showFlameChart;
    return;
  }

  if(!isN64) {
    ImGui::TextUnformatted("Flame chart only available for Nintendo 64.");
    ImGui::End();
    settings.general.showFlameChart = true;
    return;
  }

  auto& prof = ares::Nintendo64::cpu.profiler;
  if(!autoActive) {
    prof.setEnabled(true);
    prof.recordTimeline.store(true, std::memory_order_relaxed);
    ares::Nintendo64::rsp.capture.enabled.store(true, std::memory_order_relaxed);
    autoActive = true;
  }

  static std::vector<Span> spans;
  static std::vector<RspSpan> rspSpans;
  static std::vector<RdpSpan> rdpSpans;
  static std::vector<std::pair<u64, u64>> haltSpans;  //window-relative RSP halt intervals
  static std::vector<u64> viMarks;  //window-relative VI swap times
  static bool freeze = false;

  // Synthetic on-screen width for an RDP flush: the RDP has no per-command timing,
  // so a flush block is sized by its command count
  static constexpr u64 rdpTicksPerCmd = 64;

  // Window length selector: integer multiples of one VI period (~16.67 ms at the NTSC 60 Hz field rate). viTicks = 187.5 MHz / 60.
  static constexpr u64 viTicks = 3'125'000;
  static const char* windowItems[] = {
    "16.7 ms", "33.3 ms", "66.7 ms", "133 ms", "267 ms",
  };
  static const u32 windowMul[] = {1, 2, 4, 8, 16};
  static int windowIdx = 1;  //default: 2 VI
  u64 windowTicks = (u64)windowMul[windowIdx] * viTicks;

  auto& rcap = ares::Nintendo64::rsp.capture;

  // Collect the spans whose [start,end] intersects the window. Both rings are
  // appended in end order (now() is monotonic), so we walk back from the newest
  // entry and stop at the first span that ends before the window
  // everything older is outside it. Spans that began before the window clamp their start to
  // 0 (they were already running at the window's left edge).
  static u64 winStart = 0;
  bool ringFull = false;

  // Collection-time decimation. A wide window can hold hundreds of thousands of CPU
  // spans (the ring caps at maxSpans), and copying + sorting all of them every UI
  // frame is the dominant cost. At any zoom only ~1 span per (depth, screen
  // pixel) is visible, so we drop narrow spans that land in an already-occupied
  // cell, using the PREVIOUS frame's view (computed below, stored at end of frame)
  // in absolute ticks so it doesn't drift as the window slides. Wide spans (the
  // overview structure) are always kept. The exact per-pixel merge still happens at
  // draw time; this is just a count cap so the sort stays cheap. One-frame-stale
  // zoom is imperceptible.
  static constexpr u32 GCols = 4096, GDepth = 96;
  static std::vector<u32> seenGen;
  static u32 decimGen = 0;
  static u64 lastViewAbs = 0;     //absolute tick at the left edge of last frame's view
  static f64 lastPxPerTick = 0.0; //last frame's pixels-per-tick (0 = not ready)
  if(seenGen.empty()) seenGen.resize((size_t)GCols * GDepth, 0);

  if(!freeze) {
    u64 rightEdge = prof.now();
    winStart = rightEdge > windowTicks ? rightEdge - windowTicks : 0;
    decimGen++;

    spans.clear();
    u64 w = prof.timelineWrite.load(std::memory_order_acquire);
    u64 n = std::min<u64>(w, prof.maxSpans);
    for(u64 i = 0; i < n; i++) {
      const Span& s = prof.timeline[(w - 1 - i) % prof.maxSpans];
      if(s.end < winStart) break;
      if(lastPxPerTick > 0.0) {  //decimate narrow spans to ~1 per pixel per depth
        f64 px0 = ((f64)s.start - (f64)lastViewAbs) * lastPxPerTick;
        f64 px1 = ((f64)s.end   - (f64)lastViewAbs) * lastPxPerTick;
        if(px1 - px0 < 1.5) {
          u32 c = (u32)std::clamp(px0, 0.0, (f64)(GCols - 1));
          u32 d = s.depth < GDepth ? s.depth : GDepth - 1;
          u32& g = seenGen[(size_t)d * GCols + c];
          if(g == decimGen) continue;  //cell already has a span
          g = decimGen;
        }
      }
      Span t = s;
      t.start = t.start > winStart ? t.start - winStart : 0;
      t.end   = t.end   > winStart ? t.end   - winStart : 0;
      spans.push_back(t);
    }
    ringFull = (n == prof.maxSpans);  //walked the whole ring; oldest may be dropped
    std::sort(spans.begin(), spans.end(),
              [](const Span& a, const Span& b) { return a.start < b.start; });

    rspSpans.clear();
    u64 rw = rcap.timelineWrite.load(std::memory_order_acquire);
    u64 rn = std::min<u64>(rw, rcap.maxTimeline);
    for(u64 i = 0; i < rn; i++) {
      const auto& s = rcap.timeline[(rw - 1 - i) % rcap.maxTimeline];
      if(s.end < winStart) break;
      u64 rs = s.start > winStart ? s.start - winStart : 0;
      u64 re = s.end   > winStart ? s.end   - winStart : 0;
      rspSpans.push_back({rs, re, s.overlayId, s.commandId, s.overhead, s.overheadType});
    }
    std::sort(rspSpans.begin(), rspSpans.end(),
              [](const RspSpan& a, const RspSpan& b) { return a.start < b.start; });

    // RSP hardware halt/break intervals (the bar below the command stream). Closed
    // intervals from the ring, plus the still-open halt drawn live up to the right
    // edge so a currently-stopped RSP shows immediately.
    haltSpans.clear();
    u64 hw = rcap.haltWrite.load(std::memory_order_acquire);
    u64 hn = std::min<u64>(hw, rcap.maxHaltSpans);
    for(u64 i = 0; i < hn; i++) {
      const auto& s = rcap.haltSpans[(hw - 1 - i) % rcap.maxHaltSpans];
      if(s.end < winStart) break;
      u64 hs = s.start > winStart ? s.start - winStart : 0;
      u64 he = s.end   > winStart ? s.end   - winStart : 0;
      haltSpans.emplace_back(hs, he);
    }
    if(rcap.haltOpen.load(std::memory_order_acquire)) {
      u64 hStart = rcap.haltStartWall.load(std::memory_order_relaxed);
      if(hStart <= rightEdge) {
        u64 hs = hStart > winStart ? hStart - winStart : 0;
        haltSpans.emplace_back(hs, rightEdge - winStart);
      }
    }

    // RDP: one block per DP flush, anchored at its real submit time with a
    // count-proportional synthetic width. Walk back from newest; entries are
    // start-ordered, so once a block's synthetic end falls before the window the
    // rest are too.
    auto& dcap = ares::Nintendo64::rdp.capture;
    rdpSpans.clear();
    u64 dw = dcap.timelineWrite.load(std::memory_order_acquire);
    u64 dn = std::min<u64>(dw, dcap.maxTimeline);
    for(u64 i = 0; i < dn; i++) {
      const auto& s = dcap.timeline[(dw - 1 - i) % dcap.maxTimeline];
      u64 end = s.start + (u64)s.count * rdpTicksPerCmd;
      if(end < winStart) break;
      u64 ds = s.start > winStart ? s.start - winStart : 0;
      u64 de = end     > winStart ? end     - winStart : 0;
      rdpSpans.push_back({ds, de, s.count});
    }
    std::sort(rdpSpans.begin(), rdpSpans.end(),
              [](const RdpSpan& a, const RdpSpan& b) { return a.start < b.start; });
    // Clamp each flush so it never overruns the next one (synthetic widths can
    // overlap when flushes are dense); keeps blocks readable and start times true.
    for(size_t i = 0; i + 1 < rdpSpans.size(); i++)
      rdpSpans[i].end = std::min(rdpSpans[i].end, rdpSpans[i + 1].start);

    // VI framebuffer-swap markers within the window.
    viMarks.clear();
    u64 vw = prof.viMarkWrite.load(std::memory_order_acquire);
    u64 vn = std::min<u64>(vw, prof.maxViMarks);
    for(u64 i = 0; i < vn; i++) {
      u64 m = prof.viMarks[(vw - 1 - i) % prof.maxViMarks];
      if(m < winStart) break;
      if(m <= rightEdge) viMarks.push_back(m - winStart);
    }
  }

  u64 frameTicks = windowTicks;  //axis/view length (kept name for the renderer below)
  u32 maxDepth = 0;
  for(auto& s : spans) maxDepth = std::max<u32>(maxDepth, s.depth);

  // --- view (pan/zoom) state, in window-relative ticks -----------------------
  static u64 viewStart = 0;
  static u64 viewSpan = 0;
  static bool userAdjusted = false;  //true once the user pans/zooms
  static int shownWindowIdx = -1;
  auto fit = [&]() { viewStart = 0; viewSpan = std::max<u64>(1, frameTicks); };
  // Fit to the whole window until the user takes control; also refit whenever the
  // window length changes.
  if(viewSpan == 0 || !userAdjusted || shownWindowIdx != windowIdx) { fit(); shownWindowIdx = windowIdx; }

  // --- toolbar ---------------------------------------------------------------
  if(ImGui::Button("Fit")) { fit(); userAdjusted = false; }
  ImGui::SameLine();
  ImGui::Checkbox("Freeze", &freeze);
  ImGui::SameLine();
  ImGui::SetNextItemWidth(150.0f);
  ImGui::Combo("Window", &windowIdx, windowItems, IM_ARRAYSIZE(windowItems));
  ImGui::SameLine();
  ImGui::Text("cpu: %zu (max depth: %u)   rsp: %zu   rdp: %zu   halt: %zu", spans.size(), maxDepth + 1, rspSpans.size(), rdpSpans.size(), haltSpans.size());
  if(ringFull) {
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(1, 0.6f, 0.3f, 1), "(span cap hit)");
  }
  ImGui::Separator();

  // --- canvas ----------------------------------------------------------------
  const f32 rowH = 18.0f;
  const f32 axisH = 18.0f;
  ImVec2 origin = ImGui::GetCursorScreenPos();
  ImVec2 avail = ImGui::GetContentRegionAvail();
  if(avail.x < 50) avail.x = 50;
  if(avail.y < 50) avail.y = 50;
  ImGui::InvisibleButton("canvas", avail);
  bool hovered = ImGui::IsItemHovered();
  ImGuiIO& io = ImGui::GetIO();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->PushClipRect(origin, ImVec2(origin.x + avail.x, origin.y + avail.y), true);
  dl->AddRectFilled(origin, ImVec2(origin.x + avail.x, origin.y + avail.y), IM_COL32(20, 20, 24, 255));

  // Vertical scroll for the lanes (deep call stacks + the RSP/RDP lanes below
  // them): Shift+wheel, or vertical drag. The time axis stays pinned at the top.
  static f32 laneScroll = 0.0f;
  const f32 laneGap = 19.0f;  //divider+label gap above each device lane
  const f32 haltH = 6.0f;     //thin RSP halt/stopped indicator bar
  f32 contentH = (maxDepth + 1) * rowH + (laneGap + rowH + haltH) /*RSP + halt bar*/ + (laneGap + rowH) /*RDP*/;
  f32 visibleH = avail.y - axisH;
  f32 maxScroll = std::max(0.0f, contentH - visibleH);

  // Zoom around cursor on wheel (Shift+wheel scrolls vertically); pan on drag.
  if(hovered && io.MouseWheel != 0.0f) {
    if(io.KeyShift) {
      laneScroll -= io.MouseWheel * rowH * 2.0f;
    } else {
      f64 pxPerTick = avail.x / (f64)viewSpan;
      f64 mouseTick = viewStart + (io.MousePos.x - origin.x) / pxPerTick;
      f64 factor = std::pow(1.2, -io.MouseWheel);
      f64 newSpan = std::clamp<f64>(viewSpan * factor, 32.0, frameTicks * 8.0);
      f64 newPxPerTick = avail.x / newSpan;
      f64 ns = mouseTick - (io.MousePos.x - origin.x) / newPxPerTick;
      viewStart = (u64)std::max<f64>(0.0, ns);
      viewSpan = (u64)newSpan;
      userAdjusted = true;
    }
  }
  if(ImGui::IsItemActive()) {
    if(io.MouseDelta.x != 0.0f) {
      f64 pxPerTick = avail.x / (f64)viewSpan;
      f64 ns = (f64)viewStart - io.MouseDelta.x / pxPerTick;
      viewStart = (u64)std::max<f64>(0.0, ns);
      userAdjusted = true;
    }
    if(io.MouseDelta.y != 0.0f) laneScroll -= io.MouseDelta.y;
  }
  laneScroll = std::clamp(laneScroll, 0.0f, maxScroll);

  // Measurement marker: a left click (without a pan drag) drops a marker at the
  // clicked instant; right click clears it. Stored in absolute master-clock ticks
  // so it stays pinned to a real moment as the window slides. The marker -> cursor
  // delta is shown live under the time axis (drawn below).
  static u64 markerAbs = 0;
  static bool hasMarker = false;
  static f32 pressX = 0.0f;
  {
    f64 pxPerTickNow = avail.x / (f64)viewSpan;
    if(ImGui::IsItemActivated()) pressX = io.MousePos.x;
    if(hovered && ImGui::IsMouseReleased(ImGuiMouseButton_Left)
              && std::abs(io.MousePos.x - pressX) < 4.0f) {
      f64 relTick = (f64)viewStart + (io.MousePos.x - origin.x) / pxPerTickNow;
      markerAbs = winStart + (u64)std::max<f64>(0.0, relTick);
      hasMarker = true;
    }
    if(hovered && ImGui::IsMouseReleased(ImGuiMouseButton_Right)) hasMarker = false;
  }

  f64 pxPerTick = avail.x / (f64)viewSpan;
  u64 viewEnd = viewStart + viewSpan;
  // Publish this frame's view (in absolute ticks) for next frame's collection-time decimation grid.
  lastViewAbs = winStart + viewStart;
  lastPxPerTick = pxPerTick;
  f32 lanesTop = origin.y + axisH - laneScroll;
  f32 clipTop = origin.y + axisH;  //lanes are clipped below the pinned axis

  // Time axis ticks (~every 120px), labelled in us/ms relative to window start.
  {
    f64 targetPx = 120.0;
    f64 stepTicks = targetPx / pxPerTick;
    // round step to a "nice" 1/2/5 * 10^k value
    f64 mag = std::pow(10.0, std::floor(std::log10(std::max(1.0, stepTicks))));
    f64 norm = stepTicks / mag;
    f64 nice = norm < 1.5 ? 1 : norm < 3 ? 2 : norm < 7 ? 5 : 10;
    u64 step = (u64)std::max(1.0, nice * mag);
    u64 first = (viewStart / step) * step;
    for(u64 t = first; t <= viewEnd; t += step) {
      f32 x = origin.x + (f32)((t - viewStart) * pxPerTick);
      if(x < origin.x || x > origin.x + avail.x) continue;
      dl->AddLine(ImVec2(x, origin.y), ImVec2(x, origin.y + avail.y), IM_COL32(60, 60, 70, 120));
      char b[24]; fmtTime((f64)t, b, sizeof(b));  //t is already window-relative
      dl->AddText(ImVec2(x + 3, origin.y + 2), IM_COL32(170, 170, 180, 255), b);
    }
  }

  // Clip the lanes to below the pinned time axis so vertical scrolling doesn't
  // overdraw the axis labels.
  dl->PushClipRect(ImVec2(origin.x, clipTop), ImVec2(origin.x + avail.x, origin.y + avail.y), true);

  // Spans, sorted by start: iterate and cull. Break once a span starts past the
  // view (nothing later can overlap); skip those entirely left of it.
  //
  // Pixel coalescing: when zoomed out, thousands of spans fall into the same pixels.
  // rowMaxX tracks the last drawn right edge per depth; a span
  // that would add less than a pixel of new content in its row is skipped. This
  // caps the rect count at ~canvas-width * depth regardless of how many spans the
  // window holds. Hovering individual spans only matters when zoomed in (where spans are wider than a pixel and nothing coalesces).
  static std::vector<f32> rowMaxX;
  rowMaxX.assign((size_t)maxDepth + 2, -1e9f);
  f32 canvasR = origin.x + avail.x;
  f32 canvasB = origin.y + avail.y;
  const Span* hover = nullptr;
  for(auto it = spans.begin(); it != spans.end(); ++it) {
    const Span& s = *it;
    if(s.start > viewEnd) break;     //sorted by start: nothing further can overlap
    if(s.end < viewStart) continue;  //entirely left of the view
    f32 x0 = origin.x + (f32)(((s64)s.start - (s64)viewStart) * pxPerTick);
    f32 x1 = origin.x + (f32)(((s64)s.end - (s64)viewStart) * pxPerTick);
    f32& lx = rowMaxX[s.depth];
    if(x1 <= lx + 1.0f) continue;    //less than a pixel of new content in this row
    if(x1 - x0 < 1.0f) x1 = x0 + 1.0f;
    x0 = std::max(x0, origin.x);
    x1 = std::min(x1, canvasR);
    if(x1 <= x0) continue;
    lx = x1;
    f32 y0 = lanesTop + s.depth * rowH;
    f32 y1 = y0 + rowH - 1.0f;
    if(y0 > canvasB) continue;

    bool isHover = hovered && io.MousePos.x >= x0 && io.MousePos.x < x1
                           && io.MousePos.y >= y0 && io.MousePos.y < y1;
    if(isHover) hover = &s;

    ImU32 col = spanColor(s.funcAddr, s.isException);
    if(isHover) col = IM_COL32(255, 255, 255, 255);
    f32 w = x1 - x0;
    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), col, w > 4.0f ? 2.0f : 0.0f);
    if(w > 3.0f) dl->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), IM_COL32(0, 0, 0, 90), 2.0f);

    if(w > 28.0f) {
      string name = prof.labelFor(s.funcAddr);
      dl->PushClipRect(ImVec2(x0 + 2, y0), ImVec2(x1 - 1, y1), true);
      dl->AddText(ImVec2(x0 + 3, y0 + 2), IM_COL32(15, 15, 18, 255), name.data());
      dl->PopClipRect();
    }
  }

  // --- RSP lane: command spans on the same axis, below the CPU call stack -----
  const RspSpan* rhover = nullptr;
  const RdpSpan* dhover = nullptr;
  {
    f32 cpuBottom = lanesTop + (maxDepth + 1) * rowH;
    f32 labelY = cpuBottom + 4.0f;
    f32 rspTop = labelY + 15.0f;
    dl->AddLine(ImVec2(origin.x, labelY), ImVec2(origin.x + avail.x, labelY), IM_COL32(70, 70, 80, 200));
    dl->AddText(ImVec2(origin.x + 3, labelY + 1), IM_COL32(150, 200, 255, 255), "RSP");

    f32 rspMaxX = -1e9f;  //per-row pixel coalescing (see CPU lane)
    for(auto it = rspSpans.begin(); it != rspSpans.end(); ++it) {
      const RspSpan& s = *it;
      if(s.start > viewEnd) break;
      if(s.end < viewStart) continue;
      f32 x0 = origin.x + (f32)(((s64)s.start - (s64)viewStart) * pxPerTick);
      f32 x1 = origin.x + (f32)(((s64)s.end - (s64)viewStart) * pxPerTick);
      if(x1 <= rspMaxX + 1.0f) continue;
      if(x1 - x0 < 1.0f) x1 = x0 + 1.0f;
      x0 = std::max(x0, origin.x);
      x1 = std::min(x1, canvasR);
      if(x1 <= x0) continue;
      rspMaxX = x1;
      f32 y0 = rspTop;
      f32 y1 = y0 + rowH - 1.0f;
      if(y0 > canvasB) continue;

      bool isHover = hovered && io.MousePos.x >= x0 && io.MousePos.x < x1
                             && io.MousePos.y >= y0 && io.MousePos.y < y1;
      if(isHover) rhover = &s;

      ImU32 col = s.overhead ? IM_COL32(90, 90, 100, 255) : rspOverlayColor(s.overlayId);
      if(isHover) col = IM_COL32(255, 255, 255, 255);
      f32 w = x1 - x0;
      dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), col, w > 4.0f ? 2.0f : 0.0f);
      if(w > 3.0f) dl->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), IM_COL32(0, 0, 0, 90), 2.0f);
      if(w > 28.0f) {
        string name = rspLabel(s.overheadType, s.overhead, s.overlayId, s.commandId);
        dl->PushClipRect(ImVec2(x0 + 2, y0), ImVec2(x1 - 1, y1), true);
        dl->AddText(ImVec2(x0 + 3, y0 + 2), IM_COL32(15, 15, 18, 255), name.data());
        dl->PopClipRect();
      }
    }

    // RSP hardware-stopped (SP_STATUS.halted / BREAK) bar, directly under the
    // command stream. Only drawn where halted — running shows no bar.
    {
      f32 hy0 = rspTop + rowH;
      f32 hy1 = hy0 + haltH - 1.0f;
      for(auto& hsp : haltSpans) {
        if(hsp.first > viewEnd || hsp.second < viewStart) continue;
        f32 x0 = origin.x + (f32)(((s64)hsp.first  - (s64)viewStart) * pxPerTick);
        f32 x1 = origin.x + (f32)(((s64)hsp.second - (s64)viewStart) * pxPerTick);
        if(x1 - x0 < 1.0f) x1 = x0 + 1.0f;
        x0 = std::max(x0, origin.x);
        x1 = std::min(x1, origin.x + avail.x);
        if(x1 <= x0) continue;
        if(hy0 > origin.y + avail.y) continue;
        dl->AddRectFilled(ImVec2(x0, hy0), ImVec2(x1, hy1), IM_COL32(220, 70, 70, 235));
      }
    }

    // --- RDP lane: one block per DP flush, below the RSP lane ------------------
    f32 rdpLabelY = rspTop + rowH + haltH + 4.0f;
    f32 rdpTop = rdpLabelY + 15.0f;
    dl->AddLine(ImVec2(origin.x, rdpLabelY), ImVec2(origin.x + avail.x, rdpLabelY), IM_COL32(70, 70, 80, 200));
    dl->AddText(ImVec2(origin.x + 3, rdpLabelY + 1), IM_COL32(150, 255, 200, 255), "RDP");

    for(auto it = rdpSpans.begin(); it != rdpSpans.end(); ++it) {
      const RdpSpan& s = *it;
      if(s.start > viewEnd) break;
      if(s.end < viewStart) continue;
      f32 x0 = origin.x + (f32)(((s64)s.start - (s64)viewStart) * pxPerTick);
      f32 x1 = origin.x + (f32)(((s64)s.end - (s64)viewStart) * pxPerTick);
      if(x1 - x0 < 1.0f) x1 = x0 + 1.0f;
      x0 = std::max(x0, origin.x);
      x1 = std::min(x1, origin.x + avail.x);
      if(x1 <= x0) continue;
      f32 y0 = rdpTop;
      f32 y1 = y0 + rowH - 1.0f;
      if(y0 > origin.y + avail.y) continue;

      bool isHover = hovered && io.MousePos.x >= x0 && io.MousePos.x < x1
                             && io.MousePos.y >= y0 && io.MousePos.y < y1;
      if(isHover) dhover = &s;

      ImU32 col = isHover ? IM_COL32(255, 255, 255, 255) : IM_COL32(90, 200, 160, 255);
      dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), col, 2.0f);
      dl->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), IM_COL32(0, 0, 0, 90), 2.0f);
      if(x1 - x0 > 28.0f) {
        string name = {"DP ", s.count, " cmds"};
        dl->PushClipRect(ImVec2(x0 + 2, y0), ImVec2(x1 - 1, y1), true);
        dl->AddText(ImVec2(x0 + 3, y0 + 2), IM_COL32(15, 15, 18, 255), name.data());
        dl->PopClipRect();
      }
    }
  }

  dl->PopClipRect();  //lanes clip

  // VI framebuffer-swap markers: vertical lines over the whole canvas, drawn last
  // (above the lanes) so frame boundaries stay visible.
  for(u64 m : viMarks) {
    if(m < viewStart || m > viewEnd) continue;
    f32 x = origin.x + (f32)(((s64)m - (s64)viewStart) * pxPerTick);
    dl->AddLine(ImVec2(x, origin.y + axisH), ImVec2(x, origin.y + avail.y), IM_COL32(255, 90, 90, 150), 1.0f);
    dl->AddText(ImVec2(x + 2, origin.y + axisH + 1), IM_COL32(255, 120, 120, 255), "VI");
  }

  // Measurement marker (yellow) + live delta-to-cursor readout under the axis.
  if(hasMarker) {
    f64 markerRel = (f64)markerAbs - (f64)winStart;  //window-relative ticks
    f32 mx = origin.x + (f32)((markerRel - (f64)viewStart) * pxPerTick);
    if(mx >= origin.x && mx <= origin.x + avail.x)
      dl->AddLine(ImVec2(mx, origin.y), ImVec2(mx, origin.y + avail.y), IM_COL32(255, 220, 80, 220), 1.5f);

    if(hovered) {
      f32 cx = std::clamp(io.MousePos.x, origin.x, origin.x + avail.x);
      dl->AddLine(ImVec2(cx, origin.y + axisH), ImVec2(cx, origin.y + avail.y), IM_COL32(255, 220, 80, 90), 1.0f);
      //horizontal ruler slightly below the tick labels, marker -> cursor
      f32 ry = origin.y + 16.0f;
      f32 lx = std::clamp(mx, origin.x, origin.x + avail.x);
      dl->AddLine(ImVec2(lx, ry), ImVec2(cx, ry), IM_COL32(255, 220, 80, 200), 1.0f);
      f64 cursorAbs = (f64)winStart + (f64)viewStart + (io.MousePos.x - origin.x) / pxPerTick;
      char b[32]; fmtDelta(cursorAbs - (f64)markerAbs, b, sizeof(b));
      ImVec2 ts = ImGui::CalcTextSize(b);
      f32 tx = cx + 4.0f; if(tx + ts.x > origin.x + avail.x) tx = cx - 4.0f - ts.x;
      dl->AddText(ImVec2(tx, ry + 2.0f), IM_COL32(255, 230, 120, 255), b);
    }
  }

  dl->PopClipRect();  //canvas clip

  // RSP hover tooltip.
  if(rhover) {
    string name = rspLabel(rhover->overheadType, rhover->overhead, rhover->overlayId, rhover->commandId);
    char dbuf[32]; fmtTime((f64)(rhover->end - rhover->start), dbuf, sizeof(dbuf));
    f64 pct = 100.0 * (f64)(rhover->end - rhover->start) / (f64)frameTicks;
    ImGui::BeginTooltip();
    ImGui::TextUnformatted(name.data());
    ImGui::Separator();
    ImGui::Text("RSP  duration: %s  (%.2f%% of window)", dbuf, pct);
    ImGui::EndTooltip();
  }

  // RDP flush tooltip. Width is synthetic (no per-command RDP timing), so report
  // the command count rather than a duration.
  if(dhover) {
    ImGui::BeginTooltip();
    ImGui::Text("DP flush");
    ImGui::Separator();
    ImGui::Text("commands: %u", dhover->count);
    ImGui::TextDisabled("(submit time exact; width = count, not real RDP time)");
    ImGui::EndTooltip();
  }

  // Hover tooltip: function name, duration, % of frame.
  if(hover) {
    string name = prof.labelFor(hover->funcAddr);
    char dbuf[32]; fmtTime((f64)(hover->end - hover->start), dbuf, sizeof(dbuf));
    f64 pct = 100.0 * (f64)(hover->end - hover->start) / (f64)frameTicks;
    ImGui::BeginTooltip();
    ImGui::TextUnformatted(name.data());
    ImGui::Separator();
    ImGui::Text("duration: %s  (%.2f%% of window)", dbuf, pct);
    ImGui::Text("depth: %u", hover->depth);
    ImGui::EndTooltip();
  }

  ImGui::End();
  settings.general.showFlameChart = true;
}

}  // namespace ares::ui
