#include "ui.hpp"

#include "../desktop-ui.hpp"
#include <n64/n64.hpp>
#include <SDL3/SDL_opengl.h>

namespace ares::ui {

bool showFramebufferViewer = false;

static GLuint fbTex = 0;
static u32  fbTexW = 0, fbTexH = 0;

static auto n64ToRGBA32(u32* dst, const u8* src, u32 w, u32 h, u8 format, u8 size) -> void {
  u32 pixels = w * h;

  switch(size) {
  case 3: // 32bpp
    if(format == 0) { // RGBA
      for(u32 i = 0; i < pixels; i++) {
        u32 p = (src[0] << 24) | (src[1] << 16) | (src[2] << 8) | src[3];
        dst[i] = (p & 0xFF00FF00) | ((p >> 16) & 0xFF) | ((p & 0xFF) << 16);
        src += 4;
      }
    }
    break;

  case 2: // 16bpp
    if(format == 0) { // RGBA 5551
      for(u32 i = 0; i < pixels; i++) {
        u16 p = (src[0] << 8) | src[1];
        u8 r = ((p >> 11) & 0x1F) * 255 / 31;
        u8 g = ((p >>  6) & 0x1F) * 255 / 31;
        u8 b = ((p >>  1) & 0x1F) * 255 / 31;
        u8 a = (p & 1) * 255;
        dst[i] = (a << 24) | (b << 16) | (g << 8) | r;
        src += 2;
      }
    } else if(format == 3) { // IA 16bpp
      for(u32 i = 0; i < pixels; i++) {
        u8 intensity = src[0];
        u8 alpha    = src[1];
        dst[i] = (alpha << 24) | (intensity << 16) | (intensity << 8) | intensity;
        src += 2;
      }
    }
    break;

  case 1: // 8bpp
    if(format >= 4) { // I
      for(u32 i = 0; i < pixels; i++) {
        u8 v = src[0];
        dst[i] = 0xFF000000 | (v << 16) | (v << 8) | v;
        src += 1;
      }
    }
    break;

  default:
    memset(dst, 0x33, pixels * 4); // gray fill for unsupported formats
    break;
  }
}

auto DrawFramebufferViewer() -> void {
  if(!showFramebufferViewer) return;

  ImGui::SetNextWindowSize(ImVec2(340, 280), ImGuiCond_FirstUseEver);
  if(!ImGui::Begin("Framebuffer", &showFramebufferViewer)) {
    ImGui::End();
    settings.general.showFramebufferViewer = false;
    return;
  }

  if(!emulator || emulator->name != "Nintendo 64") {
    ImGui::TextUnformatted("Only available for Nintendo 64.");
    ImGui::End();
    settings.general.showFramebufferViewer = true;
    return;
  }

  // Find the most recent Set_Color_Image (0x3f) and its position in the buffer.
  // Then scan forward from there to find the maximum scissor Y extent.
  auto& cap = ares::Nintendo64::rdp.capture;
  u32 committed = cap.committedCount.load(std::memory_order_acquire);
  if(committed > cap.maxCommands) committed = cap.maxCommands;

  u32 addr = 0, fmt = 0, sz = 0, w = 0;
  u32 colorPos = committed; // position of the color image command

  for(u32 i = committed; i > 0; i--) {
    auto& cmd = cap.commands[i - 1];
    if(cmd.opcode == 0x3f) {
      u64 w0 = cmd.word0;
      fmt = (w0 >> 53) & 7;
      sz  = (w0 >> 51) & 3;
      w   = ((w0 >> 32) & 0x3FF) + 1;
      addr = w0 & 0x3FFFFFF;
      colorPos = i - 1;
      break;
    }
  }

  if(w == 0 || w > 4096 || addr == 0) {
    ImGui::TextUnformatted("No framebuffer configured. Open the RDP viewer and run a frame.");
    ImGui::End();
    settings.general.showFramebufferViewer = true;
    return;
  }

  u32 bytesPerPixel = (sz == 3) ? 4 : (sz == 2) ? 2 : (sz == 1) ? 1 : 0;
  if(bytesPerPixel == 0) {
    ImGui::TextUnformatted("Unsupported pixel size.");
    ImGui::End();
    settings.general.showFramebufferViewer = true;
    return;
  }

  // Scan forward from color image for scissor commands to find max Y extent
  u32 maxY = 0;
  for(u32 i = colorPos; i < committed; i++) {
    auto& cmd = cap.commands[i];
    if(cmd.opcode == 0x2d) {
      u32 lry = cmd.word0 & 0xFFF; // lower_right.y in u10.2
      if(lry > maxY) maxY = lry;
    }
  }

  // Height: max scissor Y (integer part of u10.2), or 4:3 fallback
  u32 h;
  if(maxY > 0) {
    h = (maxY >> 2) + 1;
  } else {
    h = w * 3 / 4;
  }
  if(h < 1) h = 1;

  u32 lineBytes = w * bytesPerPixel;
  u32 maxLines = (8 * 1024 * 1024 - addr) / lineBytes;
  if(h > maxLines) h = maxLines;

  // Read pixels from RDRAM, word by word (read<4> handles N64 big-endian conversion)
  static std::vector<u8> rawBuf;
  u32 byteCount = w * h * bytesPerPixel;
  rawBuf.resize(byteCount);
  for(u32 i = 0; i < byteCount; i += 4) {
    u32 word = (u32)ares::Nintendo64::rdram.ram.read<4>(addr + i, ares::Nintendo64::RBusDevice::UNKNOWN);
    rawBuf[i + 0] = (word >> 24) & 0xFF;
    rawBuf[i + 1] = (word >> 16) & 0xFF;
    rawBuf[i + 2] = (word >>  8) & 0xFF;
    rawBuf[i + 3] = (word >>  0) & 0xFF;
  }

  // Upload to texture
  static std::vector<u32> pixelBuf;
  pixelBuf.resize(w * h);
  n64ToRGBA32(pixelBuf.data(), rawBuf.data(), w, h, fmt, sz);

  if(fbTex == 0) glGenTextures(1, &fbTex);
  glBindTexture(GL_TEXTURE_2D, fbTex);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixelBuf.data());
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glBindTexture(GL_TEXTURE_2D, 0);
  fbTexW = w; fbTexH = h;

  ImGui::Text("addr=0x%06X  %ux%u  fmt=%u sz=%u", addr, w, h, fmt, sz);

  auto avail = ImGui::GetContentRegionAvail();
  float scale = std::min(avail.x / (float)w, avail.y / (float)h);
  ImVec2 imgSize(w * scale, h * scale);
  ImGui::Image((ImTextureID)(intptr_t)fbTex, imgSize);

  ImGui::End();
  settings.general.showFramebufferViewer = true;
}

}  // namespace ares::ui
