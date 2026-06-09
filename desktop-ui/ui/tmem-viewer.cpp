#include "ui.hpp"

#include "../desktop-ui.hpp"
#include "../application/application.hpp"
#include <n64/n64.hpp>
#include <SDL3/SDL_gpu.h>
#include <cstdio>

namespace ares::ui {

bool showTmemViewer = false;

// ── helpers ──────────────────────────────────────────────────────────────────

// TMEM uses WORD_ADDR_XOR = 1 (byte swap each 16-bit word vs host order).
// Byte address XOR is 3 (LSB-first). See thirdparty/angrylion-rdp-plus/src/core/common.h.

static constexpr u32 TMEM_BYTES = 0x1000;

// TMEM uses WORD_ADDR_XOR=1: adjacent 16-bit word pairs are swapped in the byte array.
// tmem16 writes go to tmem16[word_idx ^ 1], so to read word w we fetch the byte pair at
// physical offset (w ^ 1) * 2. 8-bit accesses are direct (no XOR).
static inline u8  tmemRead8 (const u8* tmem, u32 byte_addr) { return tmem[byte_addr]; }
static inline u16 tmemRead16(const u8* tmem, u32 word_idx)  { u32 p = (word_idx ^ 1) * 2; return (u16)tmem[p] | ((u16)tmem[p + 1] << 8); }

static const char* formatName(u8 fmt) {
  switch(fmt) { case 0: return "RGBA"; case 1: return "YUV"; case 2: return "CI"; case 3: return "IA"; case 4: return "I"; }
  return "?";
}
static const char* sizeName(u8 sz) {
  switch(sz) { case 0: return "4bpp"; case 1: return "8bpp"; case 2: return "16bpp"; case 3: return "32bpp"; }
  return "?";
}

// ── GPU texture upload (shared with framebuffer-viewer pattern) ──────────────

static SDL_GPUTexture*      tmemTex = nullptr;
static SDL_GPUTransferBuffer* tmemXfer = nullptr;
static u32 tmemTexW = 0, tmemTexH = 0;

static auto tmemUpload(const u32* pixels, u32 w, u32 h) -> SDL_GPUTexture* {
  SDL_GPUDevice* gpu = AresApp::gpu;
  if(!gpu || w == 0 || h == 0) return nullptr;
  if(!tmemTex || tmemTexW != w || tmemTexH != h) {
    if(tmemXfer) { SDL_ReleaseGPUTransferBuffer(gpu, tmemXfer); tmemXfer = nullptr; }
    if(tmemTex) { SDL_ReleaseGPUTexture(gpu, tmemTex); tmemTex = nullptr; }
    SDL_GPUTextureCreateInfo ti = {};
    ti.type = SDL_GPU_TEXTURETYPE_2D; ti.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    ti.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER; ti.width = w; ti.height = h;
    ti.layer_count_or_depth = 1; ti.num_levels = 1; ti.sample_count = SDL_GPU_SAMPLECOUNT_1;
    tmemTex = SDL_CreateGPUTexture(gpu, &ti);
    if(!tmemTex) return nullptr;
    SDL_GPUTransferBufferCreateInfo xi = {};
    xi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD; xi.size = w * h * sizeof(u32);
    tmemXfer = SDL_CreateGPUTransferBuffer(gpu, &xi);
    if(!tmemXfer) { SDL_ReleaseGPUTexture(gpu, tmemTex); tmemTex = nullptr; return nullptr; }
    tmemTexW = w; tmemTexH = h;
  }
  void* m = SDL_MapGPUTransferBuffer(gpu, tmemXfer, true);
  if(!m) return nullptr;
  memcpy(m, pixels, w * h * sizeof(u32));
  SDL_UnmapGPUTransferBuffer(gpu, tmemXfer);
  SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(gpu);
  SDL_GPUCopyPass* cp  = SDL_BeginGPUCopyPass(cmd);
  SDL_GPUTextureTransferInfo src = {}; src.transfer_buffer = tmemXfer; src.pixels_per_row = w; src.rows_per_layer = h;
  SDL_GPUTextureRegion dst = {}; dst.texture = tmemTex; dst.w = w; dst.h = h; dst.d = 1;
  SDL_UploadToGPUTexture(cp, &src, &dst, true);
  SDL_EndGPUCopyPass(cp); SDL_SubmitGPUCommandBuffer(cmd);
  return tmemTex;
}

// ── pixel decoders (TMEM → RGBA8888) ────────────────────────────────────────

// Decode one pixel at a byte offset within TMEM. Returns the RGBA8888 pixel and
// the number of bytes consumed. Format/size match N64 hardware enums.
static auto tmemDecode(const u8* tmem, u32 byteOff, u8 fmt, u8 sz, u32& consumed) -> u32 {
  consumed = (sz == 0 ? 0 : sz == 1 ? 1 : sz == 2 ? 2 : 4);
  u32 wordOff = byteOff >> 1;
  switch(sz) {
  case 0: { // 4bpp — two pixels per byte; caller selects nibble
    u8 b = tmemRead8(tmem, byteOff);
    consumed = 1; // nibble; caller may reuse the byte for the second pixel
    return 0;     // actual value chosen by caller
  }
  case 1: { // 8bpp
    u8 v = tmemRead8(tmem, byteOff);
    if(fmt == 2 || fmt == 0) return 0xFF000000 | (v << 16) | (v << 8) | v;
    if(fmt == 3) { u8 i = (v >> 4) * 17, a = (v & 0xF) * 17; return (a << 24) | (i << 16) | (i << 8) | i; }
    return 0xFF000000 | (v << 16) | (v << 8) | v;
  }
  case 2: { // 16bpp
    u16 w = tmemRead16(tmem, wordOff);
    if(fmt == 0) { // RGBA 5551
      u8 r = ((w >> 11) & 0x1F) * 255 / 31, g = ((w >> 6) & 0x1F) * 255 / 31;
      u8 b = ((w >>  1) & 0x1F) * 255 / 31, a = (w & 1) ? 255 : 0;
      return (a << 24) | (b << 16) | (g << 8) | r;
    }
    if(fmt == 3) { u8 i = (w >> 8) & 0xFF, a = w & 0xFF; return (a << 24) | (i << 16) | (i << 8) | i; }
    { u8 y = (w >> 8) & 0xFF; return 0xFF000000 | (y << 16) | (y << 8) | y; }
  }
  case 3: { // 32bpp — two 16-bit words
    u32 w = (u32)tmemRead16(tmem, wordOff) | ((u32)tmemRead16(tmem, wordOff + 1) << 16);
    if(fmt == 0) { u8 r = (w >> 24) & 0xFF, g = (w >> 16) & 0xFF, b = (w >> 8) & 0xFF, a = w & 0xFF; return (a << 24) | (b << 16) | (g << 8) | r; }
    return w;
  }
  }
  return 0xFF000000;
}

// ── UI ───────────────────────────────────────────────────────────────────────

static int  tmemMode = 1;      // 0=raw, 1-8=tile 0-7
static int  tmemRawFmt = 0;    // format for raw TMEM view
static int  tmemRawSz  = 2;    // size for raw TMEM view
static int  tmemRawWidth = 64; // row width (in pixels) for raw TMEM view

auto DrawTmemViewer() -> void {
  if(!showTmemViewer) return;

  ImGui::SetNextWindowSize(ImVec2(420_px, 340_px), ImGuiCond_FirstUseEver);
  if(!ImGui::Begin("TMEM", &showTmemViewer)) {
    ImGui::End(); settings.general.showTmemViewer = showTmemViewer; return;
  }

  if(!emulator || emulator->name != "Nintendo 64") {
    ImGui::TextUnformatted("TMEM viewer only available for Nintendo 64.");
    ImGui::End(); settings.general.showTmemViewer = true; return;
  }

#if defined(ANGRYLION)
  if(!ares::Nintendo64::angrylion.enable) {
    ImGui::TextUnformatted("TMEM viewer requires the angrylion renderer.");
    ImGui::End(); settings.general.showTmemViewer = true; return;
  }

  auto snap = ares::Nintendo64::angrylion.tmemSnapshot();
  if(!snap.tmem) {
    ImGui::TextUnformatted("TMEM not available.");
    ImGui::End(); settings.general.showTmemViewer = true; return;
  }

  u32 texW = 0, texH = 0;
  static std::vector<u32> pixelBuf;

  // Decode the selected texture into pixelBuf (always runs first so columns can
  // reference texW/texH).
  if(tmemMode == 0) {
    u32 bpp = tmemRawSz == 0 ? 0 : tmemRawSz == 1 ? 1 : tmemRawSz == 2 ? 2 : 4;
    if(bpp == 0) bpp = 1;
    u32 rawW = tmemRawWidth < 1 ? 1 : (u32)tmemRawWidth, rawH = TMEM_BYTES / (rawW * bpp);
    if(rawH < 1) rawH = 1;
    if(rawW * rawH * bpp > TMEM_BYTES) rawH = TMEM_BYTES / (rawW * bpp);
    texW = rawW; texH = rawH;
    pixelBuf.resize(texW * texH);
    for(u32 y = 0; y < texH; y++) {
      for(u32 x = 0; x < texW; x++) {
        u32 byteOff = y * texW * bpp + x * bpp;
        if(byteOff + bpp > TMEM_BYTES) { pixelBuf[y * texW + x] = 0xFF101010; continue; }
        if(tmemRawSz == 0) {
          u8 b = tmemRead8(snap.tmem, byteOff);
          u8 v = ((x & 1) ? (b & 0xF) : (b >> 4)) * 255 / 15;
          pixelBuf[y * texW + x] = 0xFF000000 | (v << 16) | (v << 8) | v;
        } else {
          u32 consumed;
          pixelBuf[y * texW + x] = tmemDecode(snap.tmem, byteOff, (u8)tmemRawFmt, (u8)tmemRawSz, consumed);
        }
      }
    }
  } else {
    int ti = tmemMode - 1;
    auto& t = snap.tiles[ti];
    if(t.sh < t.sl || t.th < t.tl || t.line == 0) {
      texW = texH = 0;
    } else {
      texW = ((t.sh >> 2) - (t.sl >> 2) + 1) & 0x3ff;
      texH = ((t.th >> 2) - (t.tl >> 2) + 1) & 0x3ff;
      if(texW > 256) texW = 256; if(texH > 256) texH = 256;
      pixelBuf.resize(texW * texH);
      ares::Nintendo64::angrylion.decodeTile((u32)ti, pixelBuf.data(), 0, 0, texW, texH);
    }
  }

  // Split: left = texture, right = tile details / raw settings.
  float leftW = ImGui::GetContentRegionAvail().x * 0.58f;
  ImGui::BeginChild("##tmemLeft", ImVec2(leftW, 0), false);

  if(texW && texH) {
    SDL_GPUTexture* gtex = tmemUpload(pixelBuf.data(), texW, texH);
    if(gtex) {
      auto avail = ImGui::GetContentRegionAvail();
      float scale = std::min(avail.y / (float)texH, avail.x / (float)texW);
      ImVec2 sz(texW * scale, texH * scale);
      if(avail.x > sz.x) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail.x - sz.x) * 0.5f);
      if(avail.y > sz.y) ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (avail.y - sz.y) * 0.5f);
      ImGui::Image((ImTextureID)(intptr_t)gtex, sz);
    }
  } else if(tmemMode != 0) {
    ImGui::Text("Tile %d: not configured", tmemMode - 1);
  }
  ImGui::EndChild();

  ImGui::SameLine();
  ImGui::BeginChild("##tmemRight", ImVec2(0, 0), false);

  // Selector row: a button per tile (0-7), plus "All" for the entire-TMEM (raw) view.
  // The active selection is highlighted. Buttons wrap to the next line when the panel
  // is too narrow (manual wrap — ImGui's SameLine() would otherwise force one row).
  const ImVec4 selColor(0.20f, 0.55f, 0.85f, 0.9f);
  const float tileW = 24.0f, allW = 38.0f;
  float rightX2 = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
  // Tighter horizontal gap between the selector buttons than the default item spacing.
  const float btnSpacing = 3.0f;
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(btnSpacing, ImGui::GetStyle().ItemSpacing.y));

  for(int i = 0; i < 8; i++) {
    auto& r = snap.tiles[i];
    bool has = r.line && r.sh >= r.sl && r.th >= r.tl;
    bool sel = (tmemMode == i + 1);
    ImVec4 col = sel ? selColor
               : has ? ImVec4(0.35f,0.35f,0.35f,0.6f)
                     : ImVec4(0.25f,0.25f,0.25f,0.4f);
    ImGui::PushStyleColor(ImGuiCol_Button, col);
    char lbl[4], dimTip[16]; snprintf(lbl, sizeof(lbl), "%d", i);
    if(has) snprintf(dimTip, sizeof(dimTip), "%ux%u", (r.sh>>2)-(r.sl>>2)+1, (r.th>>2)-(r.tl>>2)+1);
    if(ImGui::Button(lbl, ImVec2(tileW, 0))) tmemMode = i + 1;
    if(ImGui::IsItemHovered() && has) ImGui::SetTooltip("%s %s %s", formatName(r.format), sizeName(r.size), dimTip);
    ImGui::PopStyleColor();
    // Keep the next button on this line only if it still fits.
    float nextW = (i + 1 < 8) ? tileW : allW;
    if(ImGui::GetItemRectMax().x + btnSpacing + nextW < rightX2) ImGui::SameLine();
  }
  ImGui::PushStyleColor(ImGuiCol_Button, tmemMode == 0 ? selColor : ImVec4(0.25f,0.25f,0.25f,0.4f));
  if(ImGui::Button("Raw", ImVec2(allW, 0))) tmemMode = 0;
  if(ImGui::IsItemHovered()) ImGui::SetTooltip("Entire TMEM (4096 bytes)");
  ImGui::PopStyleColor();
  ImGui::PopStyleVar();

  ImGui::Separator();

  if(tmemMode == 0) {
    ImGui::Text("Raw TMEM decode");

    // Only formats that make sense for a raw byte view (YUV/CI need conversion/palette
    // state that isn't meaningful here). Codes are the real N64 format enum values.
    const char* fmtNames[] = {"RGBA","IA","I"};
    const int   fmtCodes[] = {0,     3,   4};
    // Valid sizes per format, matching N64 hardware (and what tmemDecode renders):
    // RGBA = 16/32bpp, IA = 4/8/16bpp, I = 4/8bpp.
    auto sizeValid = [](int fmt, int sz) -> bool {
      if(fmt == 0) return sz == 2 || sz == 3;
      if(fmt == 3) return sz == 0 || sz == 1 || sz == 2;
      return sz == 0 || sz == 1;  // I
    };

    int fmtIdx = tmemRawFmt == 3 ? 1 : tmemRawFmt == 4 ? 2 : 0;
    ImGui::SetNextItemWidth(110_px);
    if(ImGui::Combo("Format##rawFmt", &fmtIdx, fmtNames, 3)) tmemRawFmt = fmtCodes[fmtIdx];

    // Snap size to a valid one whenever the current format/size pair isn't allowed.
    if(!sizeValid(tmemRawFmt, tmemRawSz)) {
      for(int sz = 0; sz < 4; sz++) if(sizeValid(tmemRawFmt, sz)) { tmemRawSz = sz; break; }
    }

    const char* szNames[] = {"4bpp","8bpp","16bpp","32bpp"};
    ImGui::SetNextItemWidth(110_px);
    if(ImGui::BeginCombo("Size##rawSz", szNames[tmemRawSz])) {
      for(int sz = 0; sz < 4; sz++) {
        if(!sizeValid(tmemRawFmt, sz)) continue;
        if(ImGui::Selectable(szNames[sz], sz == tmemRawSz)) tmemRawSz = sz;
      }
      ImGui::EndCombo();
    }

    // Row width (in pixels): lets you slice TMEM until the texture lines up.
    // Ctrl+click (or click) the slider to type an exact value; clamped to [1,256].
    ImGui::SetNextItemWidth(160_px);
    ImGui::SliderInt("Width##rawW", &tmemRawWidth, 1, 128, "%d", ImGuiSliderFlags_AlwaysClamp);

    ImGui::Text("4096 bytes, %ux%u px", texW, texH);
  } else {
    int ti = tmemMode - 1;
    auto& t = snap.tiles[ti];
    if(!t.line || t.sh < t.sl || t.th < t.tl) {
      ImGui::Text("Tile %d not configured.", ti);
    } else if(ImGui::BeginTable("##tileInfo", 2, ImGuiTableFlags_Borders)) {
      ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthFixed, 50);
      ImGui::TableSetupColumn("Value");
      #define ROW(k, fmt, ...) do { ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::Text("%s", k); ImGui::TableNextColumn(); ImGui::Text(fmt, ##__VA_ARGS__); } while(0)
      ROW("Fmt",   "%s (%u)",  formatName(t.format), t.format);
      ROW("Size",  "%s (%u)",  sizeName(t.size), t.size);
      ROW("TMEM",  "0x%03X",  t.tmem * 8);
      ROW("Line",  "%u (%u B)", t.line, t.line * 8);
      ROW("Pal.",  "%u",       t.palette);
      ROW("Dims",  "%ux%u",    texW, texH);
      ROW("ST",    "[%u,%u] - [%u,%u]", t.sl, t.tl, t.sh, t.th);
      ROW("Clamp", "%c,%c",    t.clampS?'S':'-', t.clampT?'T':'-');
      ROW("Mirror","%c,%c",    t.mirrorS?'S':'-', t.mirrorT?'T':'-');
      ROW("Mask",  "%u,%u",    t.maskS, t.maskT);
      ROW("Shift", "%u,%u",    t.shiftS, t.shiftT);
      #undef ROW
      ImGui::EndTable();
    }
  }
  ImGui::EndChild();

  ImGui::End();
  settings.general.showTmemViewer = true;
#else
  ImGui::TextUnformatted("TMEM viewer requires the angrylion renderer (ANGRYLION not compiled).");
  ImGui::End(); settings.general.showTmemViewer = true;
#endif
}

}  // namespace ares::ui
