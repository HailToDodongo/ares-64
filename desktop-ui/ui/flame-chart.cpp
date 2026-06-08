#include "ui.hpp"

#include "../desktop-ui.hpp"
#include <n64/n64.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

namespace ares::ui {

bool showFlameChart = false;

// Master clock: 187.5 ticks per microsecond (matches the CPU profiler / RSP viewer).
static constexpr f64 ticksPerMicrosecond = 187.5;

using Span = ares::Nintendo64::CPU::Profiler::Span;

// Stable per-function color from its address (golden-ratio hue hash).
static auto spanColor(u32 addr, bool isException) -> ImU32 {
  if(isException) return IM_COL32(120, 120, 130, 255);  //handlers: muted gray-blue
  u32 h = addr * 2654435761u;                            //Knuth multiplicative hash
  f32 hue = (h >> 8) / (f32)0x00ffffff;
  f32 r, g, b;
  ImGui::ColorConvertHSVtoRGB(hue, 0.55f, 0.85f, r, g, b);
  return IM_COL32((int)(r * 255), (int)(g * 255), (int)(b * 255), 255);
}

static auto fmtTime(f64 ticks, char* buf, size_t n) -> void {
  f64 us = ticks / ticksPerMicrosecond;
  if(us < 1000.0) snprintf(buf, n, "%.2f us", us);
  else            snprintf(buf, n, "%.3f ms", us / 1000.0);
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
    }
    autoActive = false;
    return;
  }

  ImGui::SetNextWindowSize(ImVec2(960, 500), ImGuiCond_FirstUseEver);
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
    autoActive = true;
  }

  // Copy + sort the published frame's spans once per emulation frame (keyed by the
  // frame's end cycle). The live buffer is swapped wholesale on the emu thread; we
  // work off a local sorted copy so culling can binary-search by start time.
  static std::vector<Span> spans;
  static u64 shownFrameEnd = ~0ull;
  static u64 frameT0 = 0, frameT1 = 0;
  static bool freeze = false;

  // On a new emulation frame, copy its spans and normalize timestamps to be
  // relative to the frame start (tick 0 = frame start). Working in frame-relative
  // time means the view's pan/zoom persists across frames (each frame has fresh
  // absolute timestamps, but a fixed relative window stays meaningful), instead
  // of being reset every frame. Long calls that began in the previous frame clamp
  // to 0 (they were already running at frame start).
  if(!freeze && prof.frameSpanEnd != shownFrameEnd) {
    shownFrameEnd = prof.frameSpanEnd;
    frameT0 = prof.frameSpanStart;
    frameT1 = prof.frameSpanEnd;
    spans.assign(prof.frameSpans.begin(), prof.frameSpans.end());
    for(auto& s : spans) {
      s.start = s.start > frameT0 ? s.start - frameT0 : 0;
      s.end   = s.end   > frameT0 ? s.end   - frameT0 : 0;
    }
    std::sort(spans.begin(), spans.end(),
              [](const Span& a, const Span& b) { return a.start < b.start; });
  }

  u64 frameTicks = frameT1 > frameT0 ? frameT1 - frameT0 : 1;
  u32 maxDepth = 0;
  for(auto& s : spans) maxDepth = std::max<u32>(maxDepth, s.depth);

  // --- view (pan/zoom) state, in frame-relative ticks ------------------------
  static u64 viewStart = 0;
  static u64 viewSpan = 0;
  auto fit = [&]() { viewStart = 0; viewSpan = frameTicks; };
  if(viewSpan == 0) fit();  //first data only; afterwards the user controls the view

  // --- toolbar ---------------------------------------------------------------
  if(ImGui::Button("Fit")) fit();
  ImGui::SameLine();
  ImGui::Checkbox("Freeze", &freeze);
  ImGui::SameLine();
  {
    char b[32]; fmtTime((f64)frameTicks, b, sizeof(b));
    ImGui::Text("frame: %s   spans: %zu   depth: %u", b, spans.size(), maxDepth + 1);
  }
  if(spans.size() >= ares::Nintendo64::CPU::Profiler::maxSpans) {
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

  // Zoom around cursor on wheel; pan on drag.
  if(hovered && io.MouseWheel != 0.0f) {
    f64 pxPerTick = avail.x / (f64)viewSpan;
    f64 mouseTick = viewStart + (io.MousePos.x - origin.x) / pxPerTick;
    f64 factor = std::pow(1.2, -io.MouseWheel);
    f64 newSpan = std::clamp<f64>(viewSpan * factor, 32.0, frameTicks * 8.0);
    f64 newPxPerTick = avail.x / newSpan;
    f64 ns = mouseTick - (io.MousePos.x - origin.x) / newPxPerTick;
    viewStart = (u64)std::max<f64>(0.0, ns);
    viewSpan = (u64)newSpan;
  }
  if(ImGui::IsItemActive() && io.MouseDelta.x != 0.0f) {
    f64 pxPerTick = avail.x / (f64)viewSpan;
    f64 ns = (f64)viewStart - io.MouseDelta.x / pxPerTick;
    viewStart = (u64)std::max<f64>(0.0, ns);
  }

  f64 pxPerTick = avail.x / (f64)viewSpan;
  u64 viewEnd = viewStart + viewSpan;
  f32 lanesTop = origin.y + axisH;

  // Time axis ticks (~every 120px), labelled in us/ms relative to frame start.
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
      char b[24]; fmtTime((f64)t, b, sizeof(b));  //t is already frame-relative
      dl->AddText(ImVec2(x + 3, origin.y + 2), IM_COL32(170, 170, 180, 255), b);
    }
  }

  // Spans, sorted by start: iterate and cull. Break once a span starts past the
  // view (nothing later can overlap); skip those entirely left of it. A frame
  // holds at most a few tens of thousands of spans, so a linear scan is fine.
  const Span* hover = nullptr;
  for(auto it = spans.begin(); it != spans.end(); ++it) {
    const Span& s = *it;
    if(s.start > viewEnd) break;     //sorted by start: nothing further can overlap
    if(s.end < viewStart) continue;  //entirely left of the view
    f32 x0 = origin.x + (f32)(((s64)s.start - (s64)viewStart) * pxPerTick);
    f32 x1 = origin.x + (f32)(((s64)s.end - (s64)viewStart) * pxPerTick);
    if(x1 - x0 < 1.0f) x1 = x0 + 1.0f;  //keep sub-pixel spans visible
    x0 = std::max(x0, origin.x);
    x1 = std::min(x1, origin.x + avail.x);
    if(x1 <= x0) continue;
    f32 y0 = lanesTop + s.depth * rowH;
    f32 y1 = y0 + rowH - 1.0f;
    if(y0 > origin.y + avail.y) continue;

    bool isHover = hovered && io.MousePos.x >= x0 && io.MousePos.x < x1
                           && io.MousePos.y >= y0 && io.MousePos.y < y1;
    if(isHover) hover = &s;

    ImU32 col = spanColor(s.funcAddr, s.isException);
    if(isHover) col = IM_COL32(255, 255, 255, 255);
    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), col, 2.0f);
    dl->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), IM_COL32(0, 0, 0, 90), 2.0f);

    if(x1 - x0 > 28.0f) {
      string name = prof.labelFor(s.funcAddr);
      dl->PushClipRect(ImVec2(x0 + 2, y0), ImVec2(x1 - 1, y1), true);
      dl->AddText(ImVec2(x0 + 3, y0 + 2), IM_COL32(15, 15, 18, 255), name.data());
      dl->PopClipRect();
    }
  }

  dl->PopClipRect();

  // Hover tooltip: function name, duration, % of frame.
  if(hover) {
    string name = prof.labelFor(hover->funcAddr);
    char dbuf[32]; fmtTime((f64)(hover->end - hover->start), dbuf, sizeof(dbuf));
    f64 pct = 100.0 * (f64)(hover->end - hover->start) / (f64)frameTicks;
    ImGui::BeginTooltip();
    ImGui::TextUnformatted(name.data());
    ImGui::Separator();
    ImGui::Text("duration: %s  (%.2f%% of frame)", dbuf, pct);
    ImGui::Text("depth: %u", hover->depth);
    ImGui::EndTooltip();
  }

  ImGui::End();
  settings.general.showFlameChart = true;
}

}  // namespace ares::ui
