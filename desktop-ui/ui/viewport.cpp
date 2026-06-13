#include "ui.hpp"

#include "../desktop-ui.hpp"
#include "../application/application.hpp"
#include <n64/n64.hpp>
#include <cstdio>

namespace ares::ui {

// CRT-style overscan as a fraction cropped off each edge (0..0.45 sanity cap).
static auto overscanFraction() -> float {
  float f = (float)settings.video.overscanPercent / 100.0f;
  if(f < 0.0f) f = 0.0f;
  if(f > 0.45f) f = 0.45f;
  return f;
}

static auto overscanUV(ImVec2& uv0, ImVec2& uv1) -> void {
  float f = settings.video.overscanOverlay ? 0.0f : overscanFraction();
  uv0 = ImVec2(f, f);
  uv1 = ImVec2(1.0f - f, 1.0f - f);
}

static auto drawOverscanOverlay(ImVec2 pMin, ImVec2 pMax) -> void {
  if(!settings.video.overscanOverlay) return;
  float f = overscanFraction();
  if(f <= 0.0f) return;
  float w = pMax.x - pMin.x, h = pMax.y - pMin.y;
  float ix0 = pMin.x + w * f, ix1 = pMax.x - w * f;
  float iy0 = pMin.y + h * f, iy1 = pMax.y - h * f;
  const ImU32 col = IM_COL32(0, 0, 0, 175);
  auto* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(ImVec2(pMin.x, pMin.y), ImVec2(pMax.x, iy0), col);  // top
  dl->AddRectFilled(ImVec2(pMin.x, iy1), ImVec2(pMax.x, pMax.y), col);  // bottom
  dl->AddRectFilled(ImVec2(pMin.x, iy0), ImVec2(ix0, iy1), col);        // left
  dl->AddRectFilled(ImVec2(ix1, iy0), ImVec2(pMax.x, iy1), col);        // right
}

auto DrawViewport() -> void {
  ruby::video.renderFrame();
  auto tex = ruby::video.outputTexture();

  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));

  auto flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
  ImGui::SetNextWindowSize({640_px, 480_px}, ImGuiCond_FirstUseEver);
  ImGui::Begin("Output", nullptr, flags);

  if(tex) {
    auto avail = ImGui::GetContentRegionAvail();
    float statusH = program.message.text ? ImGui::GetTextLineHeight() + ImGui::GetStyle().ItemSpacing.y + 4 : 0;
    if(avail.x > 0 && avail.y > statusH) {
      u32 outW = 0, outH = 0;
      if(emulator) {
        outW = emulator->latch.width;
        outH = emulator->latch.height;
      }
      if(outW && outH) {
        float scale = std::min((avail.y - statusH) / (float)outH, avail.x / (float)outW);
        ImVec2 sz(outW * scale, outH * scale);
        if(avail.x > sz.x) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail.x - sz.x) * 0.5f);
        if(avail.y - statusH > sz.y) ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (avail.y - statusH - sz.y) * 0.5f);
        ImVec2 uv0, uv1; overscanUV(uv0, uv1);
        NearestSampler();
        ImGui::Image((ImTextureID)(intptr_t)tex, sz, uv0, uv1);
        RestoreSampler();
        drawOverscanOverlay(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
      } else {
        ImVec2 uv0, uv1; overscanUV(uv0, uv1);
        NearestSampler();
        ImGui::Image((ImTextureID)(intptr_t)tex, ImVec2(avail.x, avail.y - statusH), uv0, uv1);
        RestoreSampler();
        drawOverscanOverlay(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
      }
    }
    if(program.message.text) {
      ImGui::Separator();
      ImGui::TextUnformatted(program.message.text.data());
    }
  } else {
    auto avail = ImGui::GetContentRegionAvail();
    auto text = "Load a game from File menu";
    auto textSize = ImGui::CalcTextSize(text);
    ImGui::SetCursorPos(ImVec2((avail.x - textSize.x) * 0.5f, (avail.y - textSize.y) * 0.5f));
    ImGui::TextUnformatted(text);
  }

  ImGui::End();
  ImGui::PopStyleVar();
}

// Play mode: draw only the game output, filling the host window while keeping the
// emulator's aspect ratio (centered, letterboxed by the black render-pass clear).
// Called directly inside the full-viewport MainDockSpace window, so the available
// content region is the entire window.
auto DrawPlayMode() -> void {
  ruby::video.renderFrame();
  auto tex = ruby::video.outputTexture();
  if(!tex) return;

  ImVec2 avail = ImGui::GetContentRegionAvail();
  ImVec2 origin = ImGui::GetCursorScreenPos();
  if(avail.x <= 0 || avail.y <= 0) return;

  u32 outW = 0, outH = 0;
  if(emulator) { outW = emulator->latch.width; outH = emulator->latch.height; }

  ImVec2 p0 = origin, p1 = ImVec2(origin.x + avail.x, origin.y + avail.y);
  if(outW && outH) {
    float scale = std::min(avail.x / (float)outW, avail.y / (float)outH);
    ImVec2 sz(outW * scale, outH * scale);
    p0 = ImVec2(origin.x + (avail.x - sz.x) * 0.5f, origin.y + (avail.y - sz.y) * 0.5f);
    p1 = ImVec2(p0.x + sz.x, p0.y + sz.y);
  }
  ImVec2 uv0, uv1; overscanUV(uv0, uv1);
  NearestSampler();
  ImGui::GetWindowDrawList()->AddImage((ImTextureID)(intptr_t)tex, p0, p1, uv0, uv1);
  RestoreSampler();
  drawOverscanOverlay(p0, p1);
}

}  // namespace ares::ui
