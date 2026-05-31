#include "ui.hpp"

#include "../desktop-ui.hpp"
#include "../application/application.hpp"
#include <n64/n64.hpp>
#include <SDL3/SDL_gpu.h>

namespace ares::ui {

bool showFramebufferViewer = false;

static SDL_GPUTexture* fbTex = nullptr;
static SDL_GPUTransferBuffer* fbXfer = nullptr;
static u32  fbTexW = 0, fbTexH = 0;
static int  fbViewMode = 0; // 0=Color, 1=Coverage, 2=Depth, 3=D-Delta
static int  fbScaleMode = 1; // 0=Integer, 1=Linear
static float fbDepthMin = 0.0f, fbDepthMax = 100.0f; // range for depth view
static int  fbDeltaMul = 8; // multiplier for D-Delta mode (percent)

// Upload an RGBA8 buffer into a persistent SDL_GPU texture, recreating it on resize.
static auto fbUpload(const u32* pixels, u32 w, u32 h) -> SDL_GPUTexture* {
  SDL_GPUDevice* gpu = AresApp::gpu;
  if(!gpu || w == 0 || h == 0) return nullptr;

  if(!fbTex || fbTexW != w || fbTexH != h) {
    if(fbXfer) { SDL_ReleaseGPUTransferBuffer(gpu, fbXfer); fbXfer = nullptr; }
    if(fbTex) { SDL_ReleaseGPUTexture(gpu, fbTex); fbTex = nullptr; }

    SDL_GPUTextureCreateInfo texInfo = {};
    texInfo.type = SDL_GPU_TEXTURETYPE_2D;
    texInfo.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    texInfo.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    texInfo.width = w;
    texInfo.height = h;
    texInfo.layer_count_or_depth = 1;
    texInfo.num_levels = 1;
    texInfo.sample_count = SDL_GPU_SAMPLECOUNT_1;
    fbTex = SDL_CreateGPUTexture(gpu, &texInfo);
    if(!fbTex) return nullptr;

    SDL_GPUTransferBufferCreateInfo xferInfo = {};
    xferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    xferInfo.size = w * h * sizeof(u32);
    fbXfer = SDL_CreateGPUTransferBuffer(gpu, &xferInfo);
    if(!fbXfer) { SDL_ReleaseGPUTexture(gpu, fbTex); fbTex = nullptr; return nullptr; }

    fbTexW = w; fbTexH = h;
  }

  void* mapped = SDL_MapGPUTransferBuffer(gpu, fbXfer, true);
  if(!mapped) return nullptr;
  memcpy(mapped, pixels, w * h * sizeof(u32));
  SDL_UnmapGPUTransferBuffer(gpu, fbXfer);

  SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(gpu);
  SDL_GPUCopyPass* copyPass = SDL_BeginGPUCopyPass(cmd);
  SDL_GPUTextureTransferInfo src = {};
  src.transfer_buffer = fbXfer;
  src.pixels_per_row = w;
  src.rows_per_layer = h;
  SDL_GPUTextureRegion dst = {};
  dst.texture = fbTex;
  dst.w = w; dst.h = h; dst.d = 1;
  SDL_UploadToGPUTexture(copyPass, &src, &dst, true);
  SDL_EndGPUCopyPass(copyPass);
  SDL_SubmitGPUCommandBuffer(cmd);
  return fbTex;
}

static auto n64ToRGBA32(u32* dst, const u8* src, u32 w, u32 h, u8 format, u8 size) -> void {
  u32 pixels = w * h;
  switch(size) {
  case 3: // 32bpp
    if(format == 0) { // RGBA: src bytes are [R,G,B,A] in host order
      for(u32 i = 0; i < pixels; i++) {
        dst[i] = src[0] | (src[1] << 8) | (src[2] << 16) | (src[3] << 24);
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
        dst[i] = 0xFF000000 | (b << 16) | (g << 8) | r;
        src += 2;
      }
    } else if(format == 3) { // IA
      for(u32 i = 0; i < pixels; i++) {
        u8 v = src[0], a = src[1];
        dst[i] = (a << 24) | (v << 16) | (v << 8) | v;
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
    memset(dst, 0x33, pixels * 4);
    break;
  }
}

// Show only coverage/alpha channel as grayscale
static auto coverageToGray(u32* dst, const u8* src, u32 w, u32 h, u8 format, u8 size) -> void {
  u32 pixels = w * h;
  switch(size) {
  case 3: // RGBA 32bpp: alpha is byte 3
    for(u32 i = 0; i < pixels; i++) {
      u8 a = 255 - src[3];
      dst[i] = 0xFF000000 | (a << 16) | (a << 8) | a;
      src += 4;
    }
    break;
  case 2: // RGBA 16bpp: coverage is bit 0
    for(u32 i = 0; i < pixels; i++) {
      u16 p = (src[0] << 8) | src[1];
      u8 a = (p & 1) ? 0 : 255;
      dst[i] = 0xFF000000 | (a << 16) | (a << 8) | a;
      src += 2;
    }
    break;
  default:
    memset(dst, 0x33, pixels * 4);
    break;
  }
}

// Decompress N64 14-bit depth to 18-bit linear Z (from parallel-RDP z_encode.h)
static auto zDecompress(u16 z) -> u32 {
  int exponent = z >> 11;
  int mantissa = z & 0x7ff;
  int shift = std::max(6 - exponent, 0);
  int base = 0x40000 - (0x40000 >> exponent);
  return (mantissa << shift) + base; // 0x00000..0x3FFFF
}

// Depth delta: R=|Z(x)-Z(x+1)|, G=|Z(x)-Z(x+1)|
// Decompresses the raw depth buffer, computes horizontal/vertical absolute
// differences, then normalises to 0..255.
static auto depthDelta(u32* dst, const u8* src, const u8* hidden, u32 hdrSize,
                       u32 w, u32 h, u32 rdramAddr) -> void {
  u32 pixels = w * h;
  std::vector<u64> zVals(pixels);

  // Decompress all Z values first.
  for(u32 i = 0; i < pixels; i++) {
    u32 byteOff = i * 2;
    u16 word = (src[byteOff] << 8) | src[byteOff + 1];
    u32 d = word >> 2;
    u32 hi = (rdramAddr + byteOff) / 2;
    u8 dzLo = 0;
    if(hidden && hi < hdrSize) dzLo = hidden[hi] & 3;
    u32 dz = dzLo | ((word & 3) << 2);
    zVals[i] = ((u64)zDecompress((u16)d) << 4) | dz;
  }

  // Compute deltas and find max (first pass).
  u64 dMax = 1;
  for(u32 y = 0; y < h; y++) {
    for(u32 x = 0; x < w; x++) {
      u32 idx = y*w + x;
      u64 cur = zVals[idx];
      u64 dh = (x + 1 < w) ? (cur > zVals[idx + 1] ? cur - zVals[idx + 1] : zVals[idx + 1] - cur) : 0;
      u64 dv = (y + 1 < h) ? (cur > zVals[idx + w] ? cur - zVals[idx + w] : zVals[idx + w] - cur) : 0;
      if(dh > dMax) dMax = dh;
      if(dv > dMax) dMax = dv;
    }
  }

  // Second pass: normalise to 0..255 and output R/G.
  float s = dMax ? (255.0f / (float)dMax) : 1.0f;
  for(u32 y = 0; y < h; y++) {
    for(u32 x = 0; x < w; x++) {
      u32 idx = y*w + x;
      u64 cur = zVals[idx];
      u64 dh = (x + 1 < w) ? (cur > zVals[idx + 1] ? cur - zVals[idx + 1] : zVals[idx + 1] - cur) : 0;
      u64 dv = (y + 1 < h) ? (cur > zVals[idx + w] ? cur - zVals[idx + w] : zVals[idx + w] - cur) : 0;
      u32 r = (u32)((float)dh * s * fbDeltaMul); if(r > 255) r = 255;
      u32 g = (u32)((float)dv * s * fbDeltaMul); if(g > 255) g = 255;
      dst[idx] = 0xFF000000 | (r << 16) | (g << 8);
    }
  }
}

// Show depth buffer: decompress 14-bit Z + 4-bit dz from hidden RDRAM.

// Show depth buffer: decompress 14-bit Z + 4-bit dz from hidden RDRAM.
// zMin/zMax (0..1) select a sub-range of the depth: values outside that
// slice are drawn black; values inside are colour-mapped relative to it.
static auto depthToGray(u32* dst, const u8* src, const u8* hidden, u32 hdrSize,
                        u32 w, u32 h, u32 rdramAddr, float zMin, float zMax) -> void {
  u32 pixels = w * h;
  u64 absMax = 1;

  for(u32 i = 0; i < pixels; i++) {
    u32 byteOff = i * 2;
    u16 word = (src[byteOff] << 8) | src[byteOff + 1];
    u32 d = word >> 2;

    u32 hi = (rdramAddr + byteOff) / 2;
    u8 dzLo = 0;
    if(hidden && hi < hdrSize) dzLo = hidden[hi] & 3;
    u32 dz = dzLo | ((word & 3) << 2);

    u64 val = ((u64)zDecompress((u16)d) << 4) | dz;
    if(val > absMax) absMax = val;
    dst[i] = (u32)val;
  }

  float rangeLo = zMin * (float)absMax;
  float rangeHi = zMax * (float)absMax;
  float rangeScale = (rangeHi > rangeLo) ? 1.0f / (rangeHi - rangeLo) : 1.0f;

  for(u32 i = 0; i < pixels; i++) {
    float v = (float)dst[i];
    if(v < rangeLo || v > rangeHi || v == 0) { dst[i] = 0xFF000000; continue; }
    float t = 1.0f - (v - rangeLo) * rangeScale; // 0 (near)..1 (far) within slice
    u8 r, g, b;
    if      (t < 0.25f) { float s = t * 4.0f;        r = 0;           g = 0;           b = (u8)(64 + s * 191); }
    else if (t < 0.50f) { float s = (t - 0.25f)*4.0f; r = 0;           g = (u8)(s * 255);  b = (u8)(255 - s * 255); }
    else if (t < 0.75f) { float s = (t - 0.50f)*4.0f; r = (u8)(s * 255);    g = 255;          b = 0; }
    else                  { float s = (t - 0.75f)*4.0f; r = 255;          g = (u8)(255 - s * 255); b = 0; }
    dst[i] = 0xFF000000 | (b << 16) | (g << 8) | r;
  }
}

auto DrawFramebufferViewer() -> void {
  if(!showFramebufferViewer) return;

  ImGui::SetNextWindowSize(ImVec2(360, 320), ImGuiCond_FirstUseEver);
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

  auto& cap = ares::Nintendo64::rdp.capture;
  u32 committed = cap.committedCount.load(std::memory_order_acquire);
  if(committed > cap.maxCommands) committed = cap.maxCommands;

  // Sticky state: persists across frames until a new Set_*_Image command arrives
  static u32 stickyAddr = 0, stickyFmt = 0, stickySz = 0, stickyW = 0, stickyDepth = 0;
  static u32 stickyColorPos = 0;

  for(u32 i = committed; i > 0; i--) {
    auto& cmd = cap.commands[i - 1];
    if(cmd.opcode == 0x3f) {
      u64 w0 = cmd.word0;
      stickyFmt = (w0 >> 53) & 7;
      stickySz  = (w0 >> 51) & 3;
      stickyW   = ((w0 >> 32) & 0x3FF) + 1;
      stickyAddr = w0 & 0x3FFFFFF;
      stickyColorPos = i - 1;
      break;
    }
  }

  for(u32 i = committed; i > 0; i--) {
    if(cap.commands[i - 1].opcode == 0x3e) {
      stickyDepth = cap.commands[i - 1].word0 & 0x3FFFFFF;
      break;
    }
  }

  u32 addr = stickyAddr, fmt = stickyFmt, sz = stickySz, w = stickyW, depthAddr = stickyDepth;
  u32 colorPos = stickyColorPos;

  // View mode selector
  ImGui::SetNextItemWidth(80);
  const char* modes[] = {"Color", "Coverage", "Depth", "D-Delta"};
  ImGui::Combo("##fbmode", &fbViewMode, modes, 4);
  ImGui::SameLine();
  // Scale mode selector
  ImGui::SetNextItemWidth(100);
  const char* scales[] = {"Integer", "Linear"};
  ImGui::Combo("##fbscale", &fbScaleMode, scales, 2);

  // Depth range sliders — visible only in depth mode.
  if(fbViewMode == 2) {
    ImGui::SameLine();
    ImGui::SetNextItemWidth(200);
    ImGui::DragFloatRange2("##depthRange", &fbDepthMin, &fbDepthMax, 0.1f, 0.0f, 100.0f,
                           "%.2f%%", "%.2f%%", ImGuiSliderFlags_AlwaysClamp);
    if(fbDepthMin < 0.0f)   fbDepthMin = 0.0f;
    if(fbDepthMax > 100.0f) fbDepthMax = 100.0f;
    if(fbDepthMin > fbDepthMax) { float t = fbDepthMin; fbDepthMin = fbDepthMax; fbDepthMax = t; }
  }
  // Delta multiplier — visible only in D-Delta mode.
  if(fbViewMode == 3) {
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120);
    ImGui::SliderInt("##deltaMul", &fbDeltaMul, 1, 128, "x%d");
  }
  ImGui::SameLine();

  u32 readAddr = addr;
  if(fbViewMode == 2 || fbViewMode == 3) readAddr = depthAddr;

  if(readAddr == 0) {
    ImGui::TextUnformatted((fbViewMode == 2 || fbViewMode == 3) ? "No depth buffer configured." : "No framebuffer configured.");
    ImGui::End();
    settings.general.showFramebufferViewer = true;
    return;
  }

  if(w == 0 || w > 4096) {
    ImGui::TextUnformatted("No framebuffer configured.");
    ImGui::End();
    settings.general.showFramebufferViewer = true;
    return;
  }

  u32 bpp = (fbViewMode == 2 || fbViewMode == 3) ? 2 : (sz == 3) ? 4 : (sz == 2) ? 2 : (sz == 1) ? 1 : 0;
  if(bpp == 0) {
    ImGui::TextUnformatted("Unsupported pixel size.");
    ImGui::End();
    settings.general.showFramebufferViewer = true;
    return;
  }

  // Sticky height from scissor commands (persists across buffer resets)
  static u32 stickyH = 0;
  u32 maxY = 0;
  for(u32 i = colorPos; i < committed; i++) {
    if(cap.commands[i].opcode == 0x2d) {
      u32 lry = cap.commands[i].word0 & 0xFFF;
      if(lry > maxY) maxY = lry;
    }
  }
  if(maxY > 0) stickyH = (maxY >> 2) + 1;
  u32 h = stickyH > 0 ? stickyH : w * 3 / 4;
  if(h < 1) h = 1;

  // Read from RDRAM
  static std::vector<u8> rawBuf;
  u32 byteCount = w * h * bpp;
  rawBuf.resize(byteCount);
  for(u32 i = 0; i < byteCount; i += 4) {
    u32 word = (u32)ares::Nintendo64::rdram.ram.read<4>(readAddr + i, ares::Nintendo64::RBusDevice::UNKNOWN);
    rawBuf[i + 0] = (word >> 24) & 0xFF;
    rawBuf[i + 1] = (word >> 16) & 0xFF;
    rawBuf[i + 2] = (word >>  8) & 0xFF;
    rawBuf[i + 3] = (word >>  0) & 0xFF;
  }

  // Convert to RGBA32 texture
  static std::vector<u32> pixelBuf;
  pixelBuf.resize(w * h);
  if(fbViewMode == 1) {
    coverageToGray(pixelBuf.data(), rawBuf.data(), w, h, fmt, sz);
  } else if(fbViewMode == 2) {
    const u8* hidden = nullptr;
    u32 hdrSize = 0;
    ares::Nintendo64::vulkan.mapHiddenRDRAM(hidden, hdrSize);
    depthToGray(pixelBuf.data(), rawBuf.data(), hidden, hdrSize, w, h, readAddr, fbDepthMin * 0.01f, fbDepthMax * 0.01f);
    ares::Nintendo64::vulkan.unmapHiddenRDRAM();
  } else if(fbViewMode == 3) {
    const u8* hidden = nullptr;
    u32 hdrSize = 0;
    ares::Nintendo64::vulkan.mapHiddenRDRAM(hidden, hdrSize);
    depthDelta(pixelBuf.data(), rawBuf.data(), hidden, hdrSize, w, h, readAddr);
    ares::Nintendo64::vulkan.unmapHiddenRDRAM();
  } else {
    n64ToRGBA32(pixelBuf.data(), rawBuf.data(), w, h, fmt, sz);
  }

  // Upload texture
  SDL_GPUTexture* tex = fbUpload(pixelBuf.data(), w, h);

  const char* modeTxt = fbViewMode == 3 ? "D-Delta" : fbViewMode == 2 ? "Depth" : fbViewMode == 1 ? "Cvg" : "Color";
  ImGui::Text("%s  addr=0x%06X  %ux%u  fmt=%u sz=%u", modeTxt, readAddr, w, h, fmt, sz);

  auto avail = ImGui::GetContentRegionAvail();
  float scale;
  if(fbScaleMode == 0) {
    // Integer scale: largest integer multiple that fits
    float maxW = std::floor(avail.x / (float)w);
    float maxH = std::floor(avail.y / (float)h);
    scale = std::max(1.0f, std::min(maxW, maxH));
  } else {
    // Linear: fill window keeping aspect ratio
    scale = std::min(avail.x / (float)w, avail.y / (float)h);
  }
  ImVec2 imgSize(w * scale, h * scale);
  // Center image in available space
  if(avail.x > imgSize.x) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail.x - imgSize.x) * 0.5f);
  if(avail.y > imgSize.y) ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (avail.y - imgSize.y) * 0.5f);
  if(tex) ImGui::Image((ImTextureID)(intptr_t)tex, imgSize);

  ImGui::End();
  settings.general.showFramebufferViewer = true;
}

}  // namespace ares::ui
