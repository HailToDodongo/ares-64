#include "ui.hpp"

#include "../desktop-ui.hpp"

namespace ares::ui {

bool showAudioViewer = false;

static auto drawWaveform(ImDrawList* dl, ImVec2 pos, ImVec2 size, const f32* buffer, u32 bufferSize, u32 writePos, u32 color) -> void {
  if(bufferSize == 0 || size.x <= 0) return;

  // Background
  dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), IM_COL32(0, 0, 0, 180));

  float centerY = pos.y + size.y * 0.5f;
  float amplitude = size.y * 0.48f;

  // Figure out how many samples we can display (most recent first, right-aligned)
  u32 visibleSamples = std::min(bufferSize, (u32)size.x * 4);
  u32 readEnd = writePos;
  u32 readStart = (readEnd + bufferSize - visibleSamples) % bufferSize;

  // Draw zero line
  dl->AddLine(ImVec2(pos.x, centerY), ImVec2(pos.x + size.x, centerY), IM_COL32(80, 80, 80, 255), 1.0f);

  // For each pixel column, find min/max in that sample region
  float samplesPerPixel = (float)visibleSamples / size.x;
  if(samplesPerPixel < 1.0f) samplesPerPixel = 1.0f;

  float sampleIdx = 0;
  for(int px = 0; px < (int)size.x; px++) {
    u32 chunkStart = (u32)sampleIdx;
    u32 chunkEnd = (u32)(sampleIdx + samplesPerPixel);
    if(chunkEnd > visibleSamples) chunkEnd = visibleSamples;
    if(chunkStart >= chunkEnd) break;

    f32 minVal = 1.0f, maxVal = -1.0f;
    for(u32 i = chunkStart; i < chunkEnd; i++) {
      u32 idx = (readStart + i) % bufferSize;
      f32 val = buffer[idx];
      if(val < minVal) minVal = val;
      if(val > maxVal) maxVal = val;
    }

    float x = pos.x + (float)px;
    float y0 = centerY - maxVal * amplitude;
    float y1 = centerY - minVal * amplitude;
    if(y1 - y0 < 1.0f) y1 = y0 + 1.0f;
    dl->AddLine(ImVec2(x, y0), ImVec2(x, y1), color, 1.0f);

    sampleIdx += samplesPerPixel;
  }

  // Border
  dl->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y), IM_COL32(100, 100, 100, 255));
}

auto DrawAudioViewer() -> void {
  if(!showAudioViewer) return;

  ImGui::SetNextWindowSize(ImVec2(500, 250), ImGuiCond_FirstUseEver);
  if(!ImGui::Begin("Audio Viewer", &showAudioViewer)) {
    ImGui::End();
    return;
  }

  ImGui::Checkbox("Capture", &program.audioCapture.enabled);

  auto& cap = program.audioCapture;
  u32 writePos = cap.writePos.load(std::memory_order_acquire);

  ImVec2 avail = ImGui::GetContentRegionAvail();
  float channelHeight = (avail.y - ImGui::GetTextLineHeight() - ImGui::GetStyle().ItemSpacing.y * 3) / 2.0f;
  if(channelHeight < 30) channelHeight = 30;

  ImDrawList* dl = ImGui::GetWindowDrawList();

  // Left channel
  ImGui::TextUnformatted("Left");
  ImVec2 leftPos = ImGui::GetCursorScreenPos();
  drawWaveform(dl, leftPos, ImVec2(avail.x, channelHeight), cap.left, cap.bufferSize, writePos, IM_COL32(0, 200, 255, 255));
  ImGui::Dummy(ImVec2(avail.x, channelHeight));

  ImGui::Spacing();

  // Right channel
  ImGui::TextUnformatted("Right");
  ImVec2 rightPos = ImGui::GetCursorScreenPos();
  drawWaveform(dl, rightPos, ImVec2(avail.x, channelHeight), cap.right, cap.bufferSize, writePos, IM_COL32(255, 100, 50, 255));
  ImGui::Dummy(ImVec2(avail.x, channelHeight));

  ImGui::End();
}

}  // namespace ares::ui
