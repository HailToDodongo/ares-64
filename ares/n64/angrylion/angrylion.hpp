namespace ares::Nintendo64 {

//Per-frame RDP rendering metrics derived from angrylion's counters.
struct AngrylionMetrics {
  u64 pixelsDrawn = 0;    //framebuffer pixels written by primitives
  u64 pixelsFilled = 0;   //framebuffer pixels written by fill-mode rectangles
  u64 triangles = 0;      //triangle commands (opcodes 0x08..0x0f)
  u64 rectangles = 0;     //texture/fill rectangle commands (0x24,0x25,0x36)
  u64 textureLoads = 0;   //load block/tile/tlut (0x30,0x33,0x34)
  u64 totalCommands = 0;  //all executed RDP commands
  u32 fbWidth = 0;        //last scanned-out framebuffer dimensions
  u32 fbHeight = 0;
};

//angrylion-rdp-plus: synchronous CPU RDP renderer.
//Mirrors the Vulkan adapter's role, but renders on the CPU and produces a
//host-side framebuffer that VI::refresh blits into the screen. See
//docs/angrylion-renderer-integration-plan.md.
struct Angrylion {
  auto load(Node::Object) -> bool;
  auto unload() -> void;

  //feed the current RDP command list (DP_CURRENT..DP_END) and render it.
  //returns true when angrylion handled rendering (so the software stubs are skipped).
  auto render() -> bool;

  //run the VI pipeline and capture a host-side framebuffer for this field.
  //returns true when a valid frame was produced.
  auto scanout(bool field) -> bool;

  //mirror a VI register write (address is the 0..13 register index).
  auto writeWord(u32 address, u32 data) -> void;

  //access the framebuffer produced by the most recent scanout().
  //pixels are 8-bit RGBA; pitch is in pixels.
  auto frame(const u8*& rgba, u32& width, u32& height, u32& pitch) -> bool;

  //metrics accumulated over the most recently displayed frame (for the RDP viewer).
  auto metrics() -> AngrylionMetrics;

  //framebuffer access heatmap buffers, one saturating u8 counter per 16-bit RDRAM word,
  //indexed by (byte address >> 1). writes = overdraw; reads = blend/read-modify-write
  //traffic. Accumulated over the current frame. Returns false if unavailable.
  auto heatmap(const u8*& writes, const u8*& reads, u32& entries) -> bool;

  //hidden RDRAM (dz/coverage bits), one byte per 16-bit RDRAM word. Mirrors the Vulkan
  //interface so the framebuffer viewer's depth modes work under angrylion too.
  //unmap is a no-op (no GPU buffer to release).
  auto mapHiddenRDRAM(const u8*& data, u32& size) -> void;
  auto unmapHiddenRDRAM() -> void;

  //TMEM viewer: snapshot of the 4096-byte texture memory and 8 tile configurations.
  //tmem is a live pointer (WORD_ADDR_XOR byte order); read between frames.
  struct TmemSnapshot {
    const u8* tmem = nullptr;
    struct Tile {
      u8  format, size;
      u16 tmem, line, palette;
      u16 sl, tl, sh, th;
      u8  clampS, clampT, mirrorS, mirrorT, maskS, maskT, shiftS, shiftT;
    } tiles[8] = {};
  };
  auto tmemSnapshot() -> TmemSnapshot;

  //Decode a rectangular region of a tile into host RGBA8888 pixels using
  //angrylion's internal fetch_texel (handles all formats, TLUT, correct byte
  //order). dst must hold w*h u32s; coordinates are texel indices.
  auto decodeTile(u32 tilenum, u32* dst, u32 x0, u32 y0, u32 w, u32 h) -> void;

  //feed the list one command at a time, pausing on interesting commands (RDP viewer step mode).
  auto renderStepped(u32 current, u32 end, bool source) -> void;

  struct Implementation;
  Implementation* implementation = nullptr;

  std::atomic<bool> enable = false;
};

extern Angrylion angrylion;

}
