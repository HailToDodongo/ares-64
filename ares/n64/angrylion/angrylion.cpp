#include <n64/n64.hpp>

#include <nall/platform.hpp>  //usleep (step-mode spin)

extern "C" {
  #include <n64video.h>
}

namespace ares::Nintendo64 {

Angrylion angrylion;

//angrylion's DP_STATUS XBUS bit (DMEM DMA mode); kept in sync with src/core/n64video/rdp.c.
static constexpr u32 DP_STATUS_XBUS_DMA = 0x001;

//angrylion calls this when it processes a SyncFull command. We route it through
//RDP::syncFull() so MI DP interrupt + busy-flag handling matches the Vulkan path.
static auto angrylionInterrupt() -> void {
  rdp.syncFull();
}

//Command length in 64-bit words, indexed by opcode (matches the paraLLEl-RDP table).
//Used to walk the command stream for the RDP/framebuffer debug viewers.
static constexpr u8 commandLength[64] = {
  1, 1, 1, 1, 1, 1, 1, 1, 4, 6,12,14,12,14,20,22,
  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
};

#if ARES_DEBUG_TOOLS
//Walk the command stream and push each command to rdp.capture so the RDP command
//viewer and framebuffer viewer (which derives the FB address from Set_Color_Image)
//have data, mirroring what Vulkan::render() does inline. angrylion reads the same
//stream internally; this re-walk only runs when capture is enabled.
static auto captureCommands(u32 current, u32 end, bool source) -> void {
  auto& memory = !source ? (Memory::Writable&)rdram.ram : (Memory::Writable&)rsp.dmem;
  u32 addr = current;
  // Flame-chart: this re-walk is one DP flush at a single wall-clock instant.
  u64 tlStart = cpu.profiler.now();
  u32 tlCount = 0;
  while(addr + 8 <= end) {
    u64 word0 = memory.readUnaligned<Dual>(addr);
    u32 code = word0 >> 56 & 0x3f;
    u8 length = commandLength[code];
    rdp.capture.push(0, 0, (u8)code, word0, 0, length);
    addr += (u32)length * 8;
    tlCount++;
  }
  if(tlCount) rdp.capture.pushTimeline(tlStart, tlCount);
}

//Commands the RDP viewer pauses on in step mode (same set Vulkan::render() uses):
//triangles, tex rects, sync full, fill rect, set color image.
static auto stepInteresting(u32 code) -> bool {
  return (code >= 0x08 && code <= 0x0f) || code == 0x24 || code == 0x25
      || code == 0x29 || code == 0x36 || code == 0x3f;
}
#endif  // ARES_DEBUG_TOOLS

struct Angrylion::Implementation {
  Implementation(u8* rdram, u32 rdramSize, u8* dmem);
  ~Implementation();

  //register mirrors handed to angrylion as pointer-to-pointer arrays.
  u32  dpRegStore[DP_NUM_REG] = {};
  u32  viRegStore[VI_NUM_REG] = {};
  u32* dpReg[DP_NUM_REG] = {};
  u32* viReg[VI_NUM_REG] = {};
  u32  miIntrReg = 0;

  n64video_config config = {};
  n64video_frame_buffer fb = {};
  AngrylionMetrics lastMetrics = {};
  bool initialized = false;
  bool pendingFrameClear = true; // set by scanout(), executed once by the next render()
};

Angrylion::Implementation::Implementation(u8* rdram, u32 rdramSize, u8* dmem) {
  for(u32 i : range(DP_NUM_REG)) dpReg[i] = &dpRegStore[i];
  for(u32 i : range(VI_NUM_REG)) viReg[i] = &viRegStore[i];

  n64video_config_init(&config);
  config.gfx.rdram        = rdram;
  config.gfx.rdram_size   = rdramSize;
  config.gfx.dmem         = dmem;
  config.gfx.vi_reg       = viReg;
  config.gfx.dp_reg       = dpReg;
  config.gfx.mi_intr_reg  = &miIntrReg;
  config.gfx.mi_intr_cb   = &angrylionInterrupt;

  config.vi.mode   = VI_MODE_NORMAL;
  config.vi.interp = VI_INTERP_HYBRID;
  //multithreaded rendering: workers partition the framebuffer by scanline, so this is
  //the same configuration real angrylion frontends ship. num_workers = 0 auto-selects
  //based on core count. n64video_process_list still blocks until the frame is rendered.
  config.parallel    = true;
  config.num_workers = 0;
  config.busyloop    = false;
  config.dp.compat   = DP_COMPAT_HIGH;

  n64video_init(&config);
  initialized = true;
}

Angrylion::Implementation::~Implementation() {
  if(initialized) n64video_close();
}

auto Angrylion::load(Node::Object) -> bool {
  if(!enable) return true;
  delete implementation;
  implementation = new Angrylion::Implementation(rdram.ram.data, rdram.ram.size, rsp.dmem.data);
  platform->status("angrylion-rdp-plus enabled (CPU renderer)");
  return true;
}

auto Angrylion::unload() -> void {
  delete implementation;
  implementation = nullptr;
}

auto Angrylion::render() -> bool {
  if(!implementation) return false;

  //scanout sets this flag once per field; we snapshot + clear on the next render()
  //(which aligns with the VI-address-swap "frame" the debug viewers use). Repeated
  //scanouts on the same frame just re-set the flag without clearing.
#if ARES_DEBUG_TOOLS
  if(implementation->pendingFrameClear) {
    n64video_metrics raw = {};
    n64video_metrics_get(&raw);
    n64video_metrics_reset();
    n64video_fb_heatmap_clear();

    AngrylionMetrics m;
    m.pixelsDrawn  = raw.pixels_drawn;
    m.pixelsFilled = raw.pixels_filled;
    for(u32 op = 0x08; op <= 0x0f; op++) m.triangles += raw.cmd_count[op];
    m.rectangles  = raw.cmd_count[0x24] + raw.cmd_count[0x25] + raw.cmd_count[0x36];
    m.textureLoads = raw.cmd_count[0x30] + raw.cmd_count[0x33] + raw.cmd_count[0x34];
    for(u32 op = 0; op < 64; op++) m.totalCommands += raw.cmd_count[op];
    m.fbWidth  = implementation->fb.width;
    m.fbHeight = implementation->fb.height;
    implementation->lastMetrics = m;
    implementation->pendingFrameClear = false;
  }
#endif

  auto& command = rdp.command;
  u32 current = command.current & ~7;
  u32 end = command.end & ~7;
  if(current >= end) {
    command.current = command.end;
    return true;
  }

  implementation->dpRegStore[DP_STATUS] = command.source ? DP_STATUS_XBUS_DMA : 0;

#if ARES_DEBUG_TOOLS
  if(rdp.capture.stepMode.load(std::memory_order_relaxed)) {
    //step mode: feed one command at a time so the framebuffer builds up per command.
    renderStepped(current, end, command.source);
  } else {
    implementation->dpRegStore[DP_CURRENT] = current;
    implementation->dpRegStore[DP_END]     = end;
    if(rdp.capture.enabled.load(std::memory_order_relaxed)) {
      captureCommands(current, end, command.source);
    }
    n64video_process_list();
    n64video_flush();
  }
#else
  implementation->dpRegStore[DP_CURRENT] = current;
  implementation->dpRegStore[DP_END]     = end;
  n64video_process_list();
  n64video_flush();
#endif

  command.current = command.end;
  return true;
}

auto Angrylion::renderStepped(u32 current, u32 end, bool source) -> void {
#if ARES_DEBUG_TOOLS
  auto& memory = !source ? (Memory::Writable&)rdram.ram : (Memory::Writable&)rsp.dmem;
  auto& cap = rdp.capture;

  u32 addr = current;
  while(addr + 8 <= end) {
    u64 word0 = memory.readUnaligned<Dual>(addr);
    u32 code = word0 >> 56 & 0x3f;
    u8 length = commandLength[code];
    u32 next = addr + (u32)length * 8;
    if(next > end) next = end;

    implementation->dpRegStore[DP_CURRENT] = addr;
    implementation->dpRegStore[DP_END]     = next;
    n64video_process_list();
    n64video_flush();                     //dispatch to workers so the command actually renders
    cap.push(0, 0, (u8)code, word0, 0, length);

    if(stepInteresting(code) && cap.stepMode.load(std::memory_order_relaxed)) {
      cap.committedCount.store(cap.writePos.load(std::memory_order_acquire), std::memory_order_release);
      cap.stepPending.store(false, std::memory_order_release);

      while(cap.stepMode.load() && cap.enabled.load() && !cap.stepPending.load()) {
        if(rsp.capture.requestClear.exchange(false, std::memory_order_acq_rel)) {
          rsp.capture.committedCount.store(0, std::memory_order_release);
          rsp.capture.writePos.store(0, std::memory_order_release);
        }

        usleep(2000);
      }
    }
    addr = next;
  }
#endif  // ARES_DEBUG_TOOLS
}

auto Angrylion::scanout(bool field) -> bool {
  if(!implementation) return false;
  implementation->viRegStore[VI_V_CURRENT_LINE] = field;
  implementation->fb = {};
  n64video_update_screen(&implementation->fb);

  //Flag the heatmap + metrics to be snapshot-and-cleared on the next render().
  //scanout fires every field (60 Hz), but render() only fires when there's a new
  //RDP list — which aligns with the VI-address-swap "frame" the debug viewers use.
  //Repeated scanouts on the same frame just re-set the flag; only one clear per frame.
  implementation->pendingFrameClear = true;

  return implementation->fb.valid && implementation->fb.pixels
      && implementation->fb.width && implementation->fb.height;
}

auto Angrylion::writeWord(u32 address, u32 data) -> void {
  if(!implementation) return;
  if(address < VI_NUM_REG) implementation->viRegStore[address] = data;
}

auto Angrylion::frame(const u8*& rgba, u32& width, u32& height, u32& pitch) -> bool {
  if(!implementation || !implementation->fb.valid || !implementation->fb.pixels) {
    rgba = nullptr; width = height = pitch = 0;
    return false;
  }
  rgba   = (const u8*)implementation->fb.pixels;
  width  = implementation->fb.width;
  height = implementation->fb.height;
  pitch  = implementation->fb.pitch;
  return true;
}

auto Angrylion::metrics() -> AngrylionMetrics {
  if(!implementation) return {};
  return implementation->lastMetrics;
}

auto Angrylion::heatmap(const u8*& writes, const u8*& reads, u32& entries) -> bool {
  if(!implementation) { writes = reads = nullptr; entries = 0; return false; }
  n64video_fb_heatmap raw = {};
  n64video_fb_heatmap_get(&raw);
  writes  = raw.writes;
  reads   = raw.reads;
  entries = raw.entries;
  return writes && reads && entries;
}

auto Angrylion::mapHiddenRDRAM(const u8*& data, u32& size) -> void {
  if(!implementation) { data = nullptr; size = 0; return; }
  const uint8_t* buf = nullptr;
  uint32_t entries = 0;
  n64video_hidden_rdram_get(&buf, &entries);
  data = (const u8*)buf;
  size = entries;
}

auto Angrylion::unmapHiddenRDRAM() -> void {
  //angrylion's hidden RDRAM is a plain in-process buffer; nothing to unmap.
}

auto Angrylion::tmemSnapshot() -> TmemSnapshot {
  TmemSnapshot out;
  if(!implementation) return out;
  struct n64video_tmem_snapshot raw;
  n64video_tmem_snapshot(&raw);
  out.tmem = raw.tmem;
  for(int i = 0; i < 8; i++) {
    auto& d = out.tiles[i];
    auto& s = raw.tiles[i];
    d.format   = s.format;
    d.size     = s.size;
    d.tmem     = s.tmem;
    d.line     = s.line;
    d.palette  = s.palette;
    d.sl = s.sl; d.tl = s.tl; d.sh = s.sh; d.th = s.th;
    d.clampS = s.clamp_s;   d.clampT  = s.clamp_t;
    d.mirrorS = s.mirror_s; d.mirrorT = s.mirror_t;
    d.maskS = s.mask_s;     d.maskT   = s.mask_t;
    d.shiftS = s.shift_s;   d.shiftT  = s.shift_t;
  }
  return out;
}

auto Angrylion::decodeTile(u32 tilenum, u32* dst, u32 x0, u32 y0, u32 w, u32 h) -> void {
  if(!implementation) return;
  n64video_decode_tile_region(tilenum, dst, x0, y0, w, h);
}

}
