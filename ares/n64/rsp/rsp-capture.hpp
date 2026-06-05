// RSP command capture — buffers and config for the RSP/RDP command viewers and
// the headless --dump-log feature. Included from rsp.hpp (inside the
// ares::Nintendo64 namespace). Implementation lives in rsp-commands.cpp.

// Counter shared by the RSP and RDP command captures.
// Only ever touched from the emulation thread.
inline u64 captureSequence = 0;

struct RSPCapture {
  static constexpr u32 maxCommands = 65536;
  static constexpr u32 maxCommandWords = 16;
  static constexpr u32 maxHookAddresses = 16;

  // Internal "overhead" categories (rows that are not real commands).
  enum OverheadType : u8 {
    OverheadNone    = 0,
    OverheadLoop    = 1,  // time spent in RSPQ_Loop dispatch, outside of any command
    OverheadOvlLoad = 2,  // time spent saving/loading overlay ucode + state
    OverheadFetch   = 3,  // time spent DMAing the next command buffer from RDRAM
  };

  // What the RSP is currently doing, tracked between hook transitions.
  enum SegType : u8 {
    SegNone = 0,
    SegCommand,      // executing a real command handler
    SegLoop,         // inside RSPQ_Loop dispatch (decode/idle)
    SegOverlayLoad,  // saving/loading overlay ucode
    SegFetch,        // DMAing command buffer from RDRAM
  };

  struct Command {
    u64 cycle = 0;        // cycle delta to the previous event (time this row took)
    u64 seq = 0;          // global capture order (see captureSequence)
    u32 frame = 0;        // frame number
    u16 overlayId = 0;
    u8  commandId = 0;
    u8  wordCount = 0;
    u32 words[maxCommandWords] = {};
    bool isOverhead = false;
    u8  overheadType = 0;
    u64 bytesIn = 0;      // RDRAM -> DMEM/IMEM DMA bytes attributed to this row
    u64 bytesOut = 0;     // DMEM/IMEM -> RDRAM DMA bytes attributed to this row
  };

  Command commands[maxCommands];
  std::atomic<u32> writePos{0};
  std::atomic<u32> committedCount{0};
  std::atomic<bool> enabled{false};
  std::atomic<bool> stepMode{false};
  std::atomic<bool> stepPending{false};
  // Set by the RSP viewer to request a manual clear. Picked up by both
  // spin-waits; clears the RSP buffer immediately while stepping.
  std::atomic<bool> requestClear{false};

  // Hook addresses from JSON config (IMEM offsets)
  u32 hookAddresses[maxHookAddresses];
  u32 hookCount = 0;

  // Named hook PCs (resolved by key name from the JSON "imem" block). A value of
  // ~0u means "not configured". These let the capture logic tell apart the
  // different RSPQ dispatch points so it can emit one row per command plus the
  // synthetic overhead rows (loop/overlay-load/fetch).
  u32 pcLoop = ~0u;            // RSPQ_Loop                    (command dispatch loop)
  u32 pcExecCommand = ~0u;    // rspq_execute_command         (a command is about to run)
  u32 pcLoadOverlay = ~0u;    // rspq_load_overlay            (overlay ucode save/load)
  u32 pcFetchBuffer = ~0u;    // rspq_fetch_buffer            (DMA next command buffer)
  u32 pcFetchBufferPtr = ~0u; // rspq_fetch_buffer_with_ptr   (DMA next command buffer)

  // Segment tracking (written only from the RSP thread). We emit a row when a
  // segment ends, timestamped with the delta since the segment began.
  u8  segType = SegNone;
  u64 segStart = 0;
  u64 segSeq = 0;
  // Memory-bus bytes DMAed during the current segment (RSP only touches the bus
  // via DMA). Assigned in full at DMA start and flushed into the row on segment
  // close. "In" = RDRAM -> RSP, "Out" = RSP -> RDRAM.
  u64 segBytesIn = 0;
  u64 segBytesOut = 0;
  // Pending command descriptor for SegCommand (captured at dispatch time).
  u16 segOverlay = 0;
  u8  segCommand = 0;
  u8  segWordCount = 0;
  u32 segWords[maxCommandWords] = {};

  // DMEM layout from JSON config
  u32 dmemCmdsOffset = 0;
  u32 dmemCmdsSize = 0;
  u32 dmemDramAddrOffset = 0;
  u32 dmemOvlTableOffset = 0;
  u32 dmemOvlIdmapOffset = 0;
  // Set once the overlay-table offset has been confirmed against live DMEM
  bool ovlOffsetLocked = false;

  // Field offsets within the libdragon rsp_ucode_t struct.
  // default to some value, re-resolved from DWARF.
  u32 ucodeDataOffset = 8;
  u32 ucodeDataEndOffset = 12;
  u32 ucodeNameOffset = 24;

  // RSPQ banner for validation (from JSON)
  string banner;

  // Argument descriptors: a command extracts named fields from its words, then a
  // free-text "fmt" template composes a custom human-readable string from them
  // (e.g. "Load {count} verts"). All of this comes from the JSON; ares stays a
  // generic viewer. A command with no descriptor just shows raw hex.
  struct ArgField {
    string name;
    u8  word = 0;          // word index (0-based)
    u8  loBit = 0, hiBit = 31;  // inclusive bit range within that word
    bool isSigned = false; // sign-extend from (hiBit-loBit+1) bits
    s64 mul = 1, div = 1, add = 0;  // value = extracted*mul/div + add
    // optional fixed-point: when fscale != 0 the field is shown as a float,
    // value_f = intValue * fscale (e.g. fscale = 1/65535 for a 0..0xFFFF fraction)
    f64 fscale = 0.0;
    // optional enum: maps a computed value to a label
    struct EnumEntry { s64 value; string label; };
    std::vector<EnumEntry> enums;
  };
  struct CmdArgInfo {
    std::vector<ArgField> fields;
    string fmt;            // template; empty => auto "name=value" list
    bool valid() const { return !fields.empty() || (bool)fmt; }
  };

  // Command name lookup: commandNameMap[overlayId][commandId]
  string commandNameMap[16][256];
  // Argument descriptor lookup: cmdArgs[overlayId][commandId]
  CmdArgInfo cmdArgs[16][256];
  // Overlay name lookup: overlayNameMap[overlayId]
  string overlayNameMap[16];

  // JSON overlay data keyed by name for runtime matching
  struct JsonOvlData {
    string name;
    string commandNames[256];
    CmdArgInfo commandArgs[256];
  };
  JsonOvlData jsonOvlData[16];
  u32 jsonOvlDataCount = 0;

  // Per-frame cycle tracking
  u32 frameNumber = 0;

  // JSON config loaded flag
  bool configLoaded = false;

  auto push(u64 cycle, u64 seq, u32 frame, u16 overlayId, u8 commandId, u8 wordCount, const u32* words,
            u64 bytesIn, u64 bytesOut) -> void {
    if(!enabled.load(std::memory_order_relaxed)) return;
    auto pos = writePos.load(std::memory_order_relaxed);
    auto& cmd = commands[pos % maxCommands];
    cmd.cycle = cycle;
    cmd.seq = seq;
    cmd.frame = frame;
    cmd.overlayId = overlayId;
    cmd.commandId = commandId;
    cmd.wordCount = wordCount;
    cmd.isOverhead = false;
    cmd.overheadType = 0;
    cmd.bytesIn = bytesIn;
    cmd.bytesOut = bytesOut;
    u32 wc = min(wordCount, maxCommandWords);
    for(u32 i = 0; i < wc; i++) cmd.words[i] = words[i];
    writePos.store(pos + 1, std::memory_order_release);
  }

  auto pushOverhead(u64 cycle, u64 seq, u8 overheadType, u64 bytesIn, u64 bytesOut) -> void {
    if(!enabled.load(std::memory_order_relaxed)) return;
    auto pos = writePos.load(std::memory_order_relaxed);
    auto& cmd = commands[pos % maxCommands];
    cmd.cycle = cycle;
    cmd.seq = seq;
    cmd.frame = frameNumber;
    cmd.overlayId = 0;
    cmd.commandId = 0;
    cmd.wordCount = 0;
    cmd.isOverhead = true;
    cmd.overheadType = overheadType;
    cmd.bytesIn = bytesIn;
    cmd.bytesOut = bytesOut;
    writePos.store(pos + 1, std::memory_order_release);
  }

  // Called by the SP DMA engine when a transfer starts. The RSP only reaches the
  // memory bus via DMA, so attributing each transfer's full size to the active
  // segment gives the per-command memory bandwidth. toRDRAM=true is an outgoing
  // (RSP -> RDRAM) transfer; false is incoming (RDRAM -> RSP).
  auto onDMA(bool toRDRAM, u64 bytes) -> void {
    if(!enabled.load(std::memory_order_relaxed)) return;
    if(toRDRAM) segBytesOut += bytes;
    else        segBytesIn  += bytes;
  }

  auto hasHook(u32 imemAddress) const -> bool {
    for(u32 i = 0; i < hookCount; i++) {
      if(hookAddresses[i] == imemAddress) return true;
    }
    return false;
  }

  auto loadConfig(const string& jsonPath) -> bool;
  auto autoDetect(const string& romPath) -> bool;
  auto detectRspq() -> bool;
  auto refreshOverlayNames() -> void;

  // Compose the human-readable argument string for a captured command, using the
  // JSON descriptor. Returns {} if the command has no descriptor.
  auto formatArgs(u16 overlayId, u8 commandId, const u32* words, u8 wordCount) const -> string;

  struct DmemLabel {
    u32 offset = 0;   // DMEM byte offset of this field
    u32 size = 0;     // size in bytes the field/array occupies
    u32 stride = 0;   // element size when this entry is an array (0 => scalar)
    string name;      // field path, e.g. "rdp_mode.combiner"
  };
  std::vector<DmemLabel> dmemLabels;
  bool dmemLabelsBuilt = false;
  // Parse the cached game ELF's DWARF info, locate rsp_queue_t, and flatten its
  // members into dmemLabels.
  auto buildDmemLabels() -> void;
  // Map a DMEM byte offset to a field label, "" if it falls outside the struct.
  auto resolveDmemLabel(u32 offset) const -> string;

  // RDRAM address of the rspq_overlay_ucodes array (found via ELF)
  u64 ovlUcodesAddr = 0;

  // ELF path for lazy overlay name resolution
  string elfPath;
  // Cached ELF data — loaded once to avoid re-reading the file every frame
  // during refreshOverlayNames.
  string cachedElfData;

  // CPU profiler: substring patterns (case-insensitive) that classify a function
  // as spin/wait time. Loaded from "cpuWaitFunctions" in the JSON config.
  std::vector<string> cpuWaitPatterns;
};
