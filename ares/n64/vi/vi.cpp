#include <n64/n64.hpp>

namespace ares::Nintendo64 {

VI vi;
#include "io.cpp"
#include "debugger.cpp"
#include "serialization.cpp"

auto VI::step(u32 clocks) -> void {
  auto scaled = (u64)clocks * system.frequency() + clockFraction;
  Thread::clock += scaled / system.videoFrequency();
  clockFraction = scaled % system.videoFrequency();
}

auto VI::load(Node::Object parent) -> void {
  node = parent->append<Node::Object>("VI");

  u32 width = 640;
  u32 height = 576;

  #if defined(VULKAN)
  //Size the canvas for the maximum upscale (UHD = 4x), regardless of which renderer
  //or quality is initially selected. This lets us change upscale quality at runtime
  //(and switch between paraLLEl-RDP and angrylion) without reallocating the screen
  //buffers: lower-quality modes and angrylion simply render into the top-left
  //native-resolution region of the larger canvas.
  width  *= 4;
  height *= 4;
  #endif
  screen = node->append<Node::Video::Screen>("Screen", width, height);
  screen->setRefresh(std::bind_front(&VI::refresh, this));
  screen->refreshRateHint(Region::PAL() ? 50 : 60);
  updateScreenColors();
  configureScreenOutput();

  Region::PAL() ? screen->setAspect(12, 10) : screen->setAspect(120, 120);

  debugger.load(node);
}

auto VI::unload() -> void {
  debugger = {};
  node->remove(screen);
  screen.reset();
  node.reset();
}

auto VI::updateScreenColors() -> void {
  if(!screen) return;
  // GPU/CPU accelerated renderers (parallel-RDP, angrylion) output RGBA directly;
  // no palette conversion needed. The built-in software VI needs a color lookup table.
  bool acceleratedRenderer = false;
  #if defined(VULKAN)
  if(vulkan.enable) acceleratedRenderer = true;
  #endif
  #if defined(ANGRYLION)
  if(angrylion.enable) acceleratedRenderer = true;
  #endif
  if(acceleratedRenderer) {
    screen->colors(0, {});  //direct color: pixels pass through unconverted
    return;
  }
  screen->colors((1 << 24) + (1 << 15), [&](n32 color) -> n64 {
    if(color < (1 << 24)) {
      u64 a = 65535;
      u64 r = image::normalize(color >> 16 & 255, 8, 16);
      u64 g = image::normalize(color >>  8 & 255, 8, 16);
      u64 b = image::normalize(color >>  0 & 255, 8, 16);
      return a << 48 | r << 32 | g << 16 | b << 0;
    } else {
      u64 a = 65535;
      u64 r = image::normalize(color >> 10 & 31, 5, 16);
      u64 g = image::normalize(color >>  5 & 31, 5, 16);
      u64 b = image::normalize(color >>  0 & 31, 5, 16);
      return a << 48 | r << 32 | g << 16 | b << 0;
    }
  });
}

auto VI::configureScreenOutput() -> void {
  if(!screen) return;
  int videoHeight = Region::PAL() ? 576 : 480;

  #if defined(VULKAN)
  if(vulkan.enable) {
    screen->setSize(vulkan.outputUpscale * 640, vulkan.outputUpscale * videoHeight);
    if(!vulkan.supersampleScanout) {
      screen->setScale(1.0 / vulkan.outputUpscale, 1.0 / vulkan.outputUpscale);
    } else {
      screen->setScale(1.0, 1.0);
    }
    return;
  }
  #endif

  //angrylion / software: native resolution, no downscale.
  screen->setSize(640, videoHeight);
  screen->setScale(1.0, 1.0);
}

auto VI::replayRegisters() -> void {
  //Reconstruct each VI register word from the live io state (the inverse of the
  //readWord decode) and push it to the active backend. VI_V_CURRENT_LINE (reg 4) is a
  //write-to-acknowledge register with no persistent state (the field is re-primed each
  //scanout), so it is omitted.
  n32 reg[14] = {};

  reg[0].bit( 0, 1) = io.colorDepth;       //VI_CONTROL
  reg[0].bit( 2)    = io.gammaDither;
  reg[0].bit( 3)    = io.gamma;
  reg[0].bit( 4)    = io.divot;
  reg[0].bit( 5)    = io.reserved.bit(5);
  reg[0].bit( 6)    = io.serrate;
  reg[0].bit( 7)    = io.reserved.bit(7);
  reg[0].bit( 8, 9) = io.antialias;
  reg[0].bit(10,15) = io.reserved.bit(10,15);

  reg[1].bit(0,23)  = io.dramAddress;      //VI_DRAM_ADDRESS
  reg[2].bit(0,11)  = io.width;            //VI_H_WIDTH
  reg[3].bit(0, 9)  = io.coincidence;      //VI_V_INTR

  reg[5].bit( 0, 7) = io.hsyncWidth;       //VI_TIMING
  reg[5].bit( 8,15) = io.colorBurstWidth;
  reg[5].bit(16,19) = io.vsyncWidth;
  reg[5].bit(20,29) = io.colorBurstHsync;

  reg[6].bit(0, 9)  = io.halfLinesPerField; //VI_V_SYNC

  reg[7].bit( 0,11) = io.quarterLineDuration; //VI_H_SYNC
  reg[7].bit(16,20) = io.leapPattern;

  reg[8].bit( 0,11) = io.hsyncLeap[0];     //VI_H_SYNC_LEAP
  reg[8].bit(16,27) = io.hsyncLeap[1];

  reg[9].bit( 0, 9) = io.hend;             //VI_H_VIDEO
  reg[9].bit(16,25) = io.hstart;

  reg[10].bit( 0, 9) = io.vend;            //VI_V_VIDEO
  reg[10].bit(16,25) = io.vstart;

  reg[11].bit( 0, 9) = io.colorBurstEnd;   //VI_V_BURST
  reg[11].bit(16,25) = io.colorBurstStart;

  reg[12].bit( 0,11) = io.xscale;          //VI_X_SCALE
  reg[12].bit(16,27) = io.xsubpixel;

  reg[13].bit( 0,11) = io.yscale;          //VI_Y_SCALE
  reg[13].bit(16,27) = io.ysubpixel;

  for(u32 address : range(14)) {
    if(address == 4) continue;
    #if defined(VULKAN)
    if(vulkan.enable) vulkan.writeWord(address, reg[address]);
    #endif
    #if defined(ANGRYLION)
    if(angrylion.enable) angrylion.writeWord(address, reg[address]);
    #endif
  }
}

auto VI::main() -> void {
  while(Thread::clock < 0) {
    if(active()) {
      ++io.vcounter;
      int halfline = io.vcounter << 1 | io.field;
      if(halfline >= io.halfLinesPerField+1) {
        io.vcounter = 0;
        io.field += !io.halfLinesPerField.bit(0);
        if(++io.leapCounter == 5) io.leapCounter = 0;
      }

      if(io.vcounter == io.vstart >> 1) {
        #if defined(VULKAN)
        if (vulkan.enable) {
          gpuOutputValid = vulkan.scanoutAsync(io.field);
          vulkan.frame();
        }
        #endif
        #if defined(ANGRYLION)
        if (angrylion.enable) {
          gpuOutputValid = angrylion.scanout(io.field);
        }
        #endif
        refreshed = true;
        screen->frame();
        ri.checkRefresh();
      }

      if(io.halfLinesPerField.bit(0)) { // progressive
        if(io.vcounter == io.coincidence >> 1) {
          mi.raise(MI::IRQ::VI);
        }
      } else { // interlaced
        if(io.coincidence.bit(0)) {
          if(io.vcounter == io.coincidence >> 1)
            mi.raise(MI::IRQ::VI);
        }
        if(!io.coincidence.bit(0)) {
          int halfline = io.vcounter << 1 | io.field;
          if(!io.field && halfline == io.coincidence)
            mi.raise(MI::IRQ::VI);
          if(io.field && halfline+1 == io.coincidence)
            mi.raise(MI::IRQ::VI);
          if(!io.field && halfline == io.halfLinesPerField && io.coincidence == 0)
            mi.raise(MI::IRQ::VI);
        }
      }

      u32 lineDuration = io.quarterLineDuration+1;
      if(io.vcounter == 1)
        lineDuration = io.hsyncLeap[io.leapPattern.bit(io.leapCounter)];      
      step(lineDuration);
    } else {
      // Arbitrarily call screen->frame() every once in a while to keep the UI responsive.
      // We do that every 200 simulated lines of 0x800 quarter-clocks. This is just arbitrary,
      // the real VI is not clocking at all when inactive.
      io.vcounter = 0;
      if(++inactiveCounter >= 200) {
        inactiveCounter = 0;
        refreshed = true;
      }
      step(0x800);
    }
  }
}

auto VI::refresh() -> void {
  #if defined(VULKAN)
  if(vulkan.enable && gpuOutputValid) {
    const u8* rgba = nullptr;
    u32 width = 0, height = 0;
    vulkan.mapScanoutRead(rgba, width, height);
    if(rgba) {
      screen->setViewport(0, 0, width, height);
      for(u32 y : range(height)) {
        u32 y_fix = y; 
        // When weave interlacing is active, we need to fix the order of interleaved lines for the image output
        // but only when the VI is set to interlance and we don't use supersampling (causes severe bugs)
        // Otherwise proceed as normal
        if(io.serrate == 1 && vulkan.weaveDeinterlacing && !vulkan.supersampleScanout) y_fix = (y % 2 == 0)? y+1 : y-1; // Swap each even/odd line
        auto source = rgba + width * y_fix * sizeof(u32);
        auto target = screen->pixels(1).data() + (u64)y * screen->canvasWidth();
        for(u32 x : range(width)) {
          target[x] = source[x * 4 + 0] << 16 | source[x * 4 + 1] << 8 | source[x * 4 + 2] << 0;
        }
      }
    } else {
      screen->setViewport(0, 0, 1, 1);
      screen->pixels(1).data()[0] = 0;
    }
    vulkan.unmapScanoutRead();
    vulkan.endScanout();

    if(Model::Aleck64()) aleck64.vdp.render(screen); //aleck64 supports overlay graphics
    return;
  }
  #endif

  #if defined(ANGRYLION)
  if(angrylion.enable && gpuOutputValid) {
    const u8* rgba = nullptr;
    u32 width = 0, height = 0, pitch = 0;
    if(angrylion.frame(rgba, width, height, pitch)) {
      screen->setViewport(0, 0, width, height);
      //the canvas may be allocated larger than native res (sized for Vulkan upscale);
      //angrylion renders at 1x into the top-left region, so stride by the canvas width.
      u32 canvasPitch = screen->canvasWidth();
      for(u32 y : range(height)) {
        auto source = rgba + (u64)y * pitch * sizeof(u32);
        auto target = screen->pixels(1).data() + (u64)y * canvasPitch;
        for(u32 x : range(width)) {
          target[x] = source[x * 4 + 0] << 16 | source[x * 4 + 1] << 8 | source[x * 4 + 2] << 0;
        }
      }
    } else {
      screen->setViewport(0, 0, 1, 1);
      screen->pixels(1).data()[0] = 0;
    }
    if(Model::Aleck64()) aleck64.vdp.render(screen); //aleck64 supports overlay graphics
    return;
  }
  #endif

  if(io.serrate == 0) screen->setProgressive(0);
  if(io.serrate == 1) screen->setInterlace(!io.field);

  u32 hscan_start = Region::NTSC() ? 108 : 128;
  u32 vscan_start = Region::NTSC() ?  34 :  44;
  u32 hscan_len   = Region::NTSC() ? 640 : 640;
  u32 vscan_len   = Region::NTSC() ? 480 : 576;
  u32 hscan_stop  = hscan_start + hscan_len;
  u32 vscan_stop  = vscan_start + vscan_len;
  screen->setViewport(0, 0, hscan_len, vscan_len);

  i32 dy0 = vi.io.vstart;
  i32 dy1 = vi.io.vend;   if (dy1 < dy0) dy1 = vscan_stop;
  i32 dx0 = vi.io.hstart;
  i32 dx1 = vi.io.hend;

  dy0 = max(vscan_start, dy0);
  dy1 = min(vscan_stop,  dy1);
  dx0 = max(hscan_start, dx0);
  dx1 = min(hscan_stop,  dx1);

  // Undocumented VI guard-band "hardware bug" (match parallel-RDP)
  if(dx0 >= hscan_start) dx0 += 8;
  if(dx1 <  hscan_stop)  dx1 -= 7;

  u32 pitch = vi.io.width;
  if(vi.io.colorDepth == 2) {
    //15bpp
    u32 y0 = vi.io.ysubpixel + vi.io.yscale * (dy0 - vi.io.vstart);
    for(i32 dy = dy0; dy < dy1; dy++) {
      if(!io.serrate || (dy & 1) == !io.field) {
        u32 address = vi.io.dramAddress + (y0 >> 11) * pitch * 2;
        auto line = screen->pixels(1).data() + (dy - vscan_start) * hscan_len;
        u32 x0 = vi.io.xsubpixel + vi.io.xscale * (dx0 - vi.io.hstart);
        for(i32 dx = dx0; dx < dx1; dx++) {
          u16 data = rdram.ram.read<Half>(address + (x0 >> 10) * 2, RBusDevice::VI_DMA);
          line[dx - hscan_start] = 1 << 24 | data >> 1;
          x0 += vi.io.xscale;
        }
      }
      y0 += vi.io.yscale;
    }
  }

  if(vi.io.colorDepth == 3) {
    //24bpp
    u32 y0 = vi.io.ysubpixel + vi.io.yscale * (dy0 - vi.io.vstart);
    for(i32 dy = dy0; dy < dy1; dy++) {
      if(!io.serrate || (dy & 1) == !io.field) {
        u32 address = vi.io.dramAddress + (y0 >> 11) * pitch * 4;
        auto line = screen->pixels(1).data() + (dy - vscan_start) * hscan_len;
        u32 x0 = vi.io.xsubpixel + vi.io.xscale * (dx0 - vi.io.hstart);
        for(i32 dx = dx0; dx < dx1; dx++) {
          u32 data = rdram.ram.read<Word>(address + (x0 >> 10) * 4, RBusDevice::VI_DMA);
          line[dx - hscan_start] = data >> 8;
          x0 += vi.io.xscale;
        }
      }
      y0 += vi.io.yscale;
    }
  }
}

auto VI::power(bool reset) -> void {
  Thread::reset();
  screen->power();
  io = {};
  refreshed = false;
  clockFraction = 0;
  gpuOutputValid = false;
}

}
