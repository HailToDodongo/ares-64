// N64 telemetry server implementation. #included from program.cpp (so it shares
// that translation unit's includes: desktop-ui.hpp, <n64/n64.hpp>, the global
// `program` / `emulator`). See telemetry-server.hpp for the design.

#include <nlohmann/json.hpp>
#include <algorithm>
#include <vector>
#include <string>
#include <cstdio>

namespace nall::N64Telemetry {

using json = nlohmann::json;
namespace N64 = ares::Nintendo64;

Server server;

// Controller-override state applied in Program::input() (platform.cpp).
InputOverride inputOverride;

// Saved rsp.recompiler.enabled while the MCP RSP debugger holds the interpreter.
// Restored when debug mode is turned off.
static bool telRspSavedRecompiler = false;

// Audio capture window over Program::audioCapture's ring buffer.
static struct {
  bool active = false;
  u32  start  = 0;  // ring write position when the capture started
  u32  frames = 0;  // requested length in frames
} audioCap;

static auto audioRate() -> u32 {
  auto rate = (u32)ruby::audio.frequency;
  return rate ? rate : 48000;
}

// Frames pushed since audio.capture, clamped to the requested length.
static auto audioCapturedFrames() -> u32 {
  if(!audioCap.active) return 0;
  u32 pos = program.audioCapture.writePos.load(std::memory_order_acquire);
  u32 n = (pos + AudioCapture::bufferSize - audioCap.start) % AudioCapture::bufferSize;
  return std::min(n, audioCap.frames);
}

// --- small helpers -----------------------------------------------------------

static auto hex(u64 v) -> std::string {
  char buf[24];
  snprintf(buf, sizeof(buf), "0x%llX", (unsigned long long)v);
  return buf;
}

static auto sstr(const nall::string& s) -> std::string {
  return std::string(s.data(), s.size());
}

// True only when an N64 game is actually running; most commands need this.
static auto n64Ready() -> bool {
  return (bool)emulator && emulator->name == "Nintendo 64";
}

static auto jInput() -> json {
  auto& ov = inputOverride;
  json j;
  j["active"]  = ov.active;
  j["buttons"] = hex(ov.buttons);
  j["x"] = ov.x;
  j["y"] = ov.y;
  return j;
}

// --- register serializers (mirror desktop-ui/ui/register-viewer.cpp) ---------

static auto jCpu() -> json {
  auto& ipu = N64::cpu.ipu;
  auto& scc = N64::cpu.scc;
  auto& fpu = N64::cpu.fpu;
  json j;

  json gpr = json::array();
  for(int i = 0; i < 32; i++) gpr.push_back(hex(ipu.r[i].u64));
  j["gpr"] = gpr;
  j["lo"] = hex(ipu.lo.u64);
  j["hi"] = hex(ipu.hi.u64);
  j["pc"] = hex(ipu.pc);

  json c;
  c["Index"]    = hex((u32)scc.index.tlbEntry | ((u32)scc.index.probeFailure << 31));
  c["BadVAddr"] = hex(scc.badVirtualAddress);
  c["Count"]    = hex(scc.count >> 1);
  c["Compare"]  = hex(scc.compare);
  c["EPC"]      = hex(scc.epc);
  c["ErrorEPC"] = hex(scc.epcError);
  c["LLAddr"]   = hex(scc.ll);
  c["Wired"]    = (u32)scc.wired.index;
  c["Status.IE"]  = (u32)scc.status.interruptEnable;
  c["Status.EXL"] = (u32)scc.status.exceptionLevel;
  c["Status.ERL"] = (u32)scc.status.errorLevel;
  c["Status.KSU"] = (u32)scc.status.privilegeMode;
  c["Status.IM"]  = (u32)scc.status.interruptMask;
  c["Status.FR"]  = (u32)scc.status.floatingPointMode;
  c["Cause.ExcCode"] = (u32)scc.cause.exceptionCode;
  c["Cause.IP"]      = (u32)scc.cause.interruptPending;
  c["Cause.BD"]      = (u32)scc.cause.branchDelay;
  j["cop0"] = c;

  json f = json::array();
  for(int i = 0; i < 32; i++) {
    json r;
    r["hex"] = hex(fpu.r[i].u64);
    r["f64"] = fpu.r[i].f64;
    f.push_back(r);
  }
  j["fpu"] = f;
  json fcr;
  fcr["RM"]      = (u32)fpu.csr.roundMode;
  fcr["Flags"]   = (u32)fpu.csr.flag.data;
  fcr["Enable"]  = (u32)fpu.csr.enable.data;
  fcr["Cause"]   = (u32)fpu.csr.cause.data;
  fcr["Compare"] = (u32)fpu.csr.compare;
  j["fcr31"] = fcr;
  return j;
}

static auto jRsp() -> json {
  auto& rsp = N64::rsp;
  json j;
  json gpr = json::array();
  for(int i = 0; i < 32; i++) gpr.push_back(hex(rsp.ipu.r[i].u32));
  j["gpr"] = gpr;
  j["pc"] = hex(rsp.ipu.pc);

  auto& s = rsp.status;
  json st;
  st["halted"]          = (u32)s.halted;
  st["broken"]          = (u32)s.broken;
  st["full"]            = (u32)s.full;
  st["singleStep"]      = (u32)s.singleStep;
  st["interruptOnBreak"]= (u32)s.interruptOnBreak;
  st["semaphore"]       = (u32)s.semaphore;
  json sig = json::array();
  for(int i = 0; i < 8; i++) sig.push_back((u32)s.signal[i]);
  st["signal"] = sig;
  j["status"] = st;

  auto& vpu = rsp.vpu;
  json vu = json::array();
  for(int i = 0; i < 32; i++) {
    json e = json::array();
    for(int k = 0; k < 8; k++) e.push_back(vpu.r[i].element(k));
    vu.push_back(e);
  }
  j["vu"] = vu;
  auto accj = [](N64::RSP::r128& r) {
    json e = json::array();
    for(int k = 0; k < 8; k++) e.push_back(r.element(k));
    return e;
  };
  j["acch"] = accj(vpu.acch);
  j["accm"] = accj(vpu.accm);
  j["accl"] = accj(vpu.accl);

  // MCP debugger state + the instruction currently at PC (about to execute).
  auto& cap = rsp.capture;
  json dbg;
  dbg["active"]         = (bool)cap.dbgActive.load();
  dbg["stopped"]        = (bool)cap.dbgStopped.load();
  dbg["curPc"]          = hex(cap.dbgCurPc.load());
  dbg["instrCount"]     = (u64)cap.dbgInstrCount.load();
  dbg["stepsRemaining"] = (u32)cap.dbgStepsRemaining.load();
  dbg["runToBp"]        = (bool)cap.dbgRunToBp.load();
  json bps = json::array();
  for(u32 i = 0; i < cap.dbgBreakpointCount; i++) bps.push_back(hex(cap.dbgBreakpoints[i]));
  dbg["breakpoints"] = bps;
  j["debug"] = dbg;

  // RSP DMA engine state — diagnoses the SP_DMA_FULL-wait spin (a queued DMA that
  // never gets promoted strands the ucode's dma-full-wait loop).
  json dm;
  dm["busy"]     = (bool)rsp.dma.busy.any();
  dm["full"]     = (bool)rsp.dma.full.any();
  dm["busyR"]    = (u32)rsp.dma.busy.read;
  dm["busyW"]    = (u32)rsp.dma.busy.write;
  dm["fullR"]    = (u32)rsp.dma.full.read;
  dm["fullW"]    = (u32)rsp.dma.full.write;
  dm["clock"]    = (s64)rsp.dma.clock;
  dm["curLen"]   = (u32)rsp.dma.current.length;
  dm["curCount"] = (u32)rsp.dma.current.count;
  dm["curDram"]  = hex((u32)rsp.dma.current.dramAddress);
  dm["curPbus"]  = hex((u32)rsp.dma.current.pbusAddress);
  dm["curRegion"]= (u32)rsp.dma.current.pbusRegion;
  dm["pendDram"] = hex((u32)rsp.dma.pending.dramAddress);
  dm["pendPbus"] = hex((u32)rsp.dma.pending.pbusAddress);
  dm["pendRegion"]=(u32)rsp.dma.pending.pbusRegion;
  dm["pendLen"]  = (u32)rsp.dma.pending.length;
  dm["curOriginCpu"] = (bool)rsp.dma.current.originCpu;
  dm["curOriginPc"]  = hex((u32)rsp.dma.current.originPc);
  j["dma"] = dm;

  u32 pcv = rsp.ipu.pc & 0xFFF;
  u32 iw  = rsp.imem.read<N64::Word>(pcv);
  j["curInstrHex"] = hex(iw);
  bool sc = rsp.disassembler.showColors;
  rsp.disassembler.showColors = false;
  j["curInstr"] = sstr(rsp.disassembler.disassemble(pcv, iw));
  rsp.disassembler.showColors = sc;
  return j;
}

static auto jRdp() -> json {
  auto& c = N64::rdp.command;
  json j;
  j["DPC_START"]   = hex(c.start);
  j["DPC_END"]     = hex(c.end);
  j["DPC_CURRENT"] = hex(c.current);
  j["DPC_CLOCK"]   = hex(c.clock);
  j["PIPE_BUSY"]   = hex(c.pipeBusy);
  j["TMEM_BUSY"]   = hex(c.tmemBusy);
  j["BUFFER_BUSY"] = hex(c.bufferBusy);
  j["ready"]   = (u32)c.ready;
  j["freeze"]  = (u32)c.freeze;
  j["crashed"] = (u32)c.crashed;
  j["flush"]   = (u32)c.flush;
  j["source"]  = c.source ? "DMEM" : "RDRAM";
  return j;
}

static auto jPeripherals() -> json {
  json j;
  {
    auto& io = N64::vi.io; json v;
    v["VI_CTRL.colorDepth"] = (u32)io.colorDepth;
    v["VI_CTRL.serrate"]    = (u32)io.serrate;
    v["VI_CTRL.antialias"]  = (u32)io.antialias;
    v["VI_ORIGIN"]  = hex(io.dramAddress);
    v["VI_WIDTH"]   = (u32)io.width;
    v["VI_V_CURRENT"] = (u32)io.vcounter;
    v["VI_X_SCALE"] = (u32)io.xscale;
    v["VI_Y_SCALE"] = (u32)io.yscale;
    v["VI_H_START"] = (u32)io.hstart;
    v["VI_H_END"]   = (u32)io.hend;
    v["VI_V_START"] = (u32)io.vstart;
    v["VI_V_END"]   = (u32)io.vend;
    j["vi"] = v;
  }
  {
    auto& io = N64::ai.io; json a;
    a["AI_DRAM_ADDR"] = hex(io.dmaAddress[0]);
    a["AI_LENGTH"]    = (u32)io.dmaLength[0];
    a["AI_DMA_COUNT"] = (u32)io.dmaCount;
    a["AI_DACRATE"]   = (u32)io.dacRate;
    a["AI_BITRATE"]   = (u32)io.bitRate;
    a["AI_ENABLE"]    = (u32)io.dmaEnable;
    j["ai"] = a;
  }
  {
    auto& io = N64::pi.io; json p;
    p["PI_DRAM_ADDR"] = hex(io.dramAddress);
    p["PI_CART_ADDR"] = hex(io.pbusAddress);
    p["PI_RD_LEN"]    = (u32)io.readLength;
    p["PI_WR_LEN"]    = (u32)io.writeLength;
    p["PI_STATUS.dmaBusy"]   = (u32)io.dmaBusy;
    p["PI_STATUS.ioBusy"]    = (u32)io.ioBusy;
    p["PI_STATUS.error"]     = (u32)io.error;
    p["PI_STATUS.interrupt"] = (u32)io.interrupt;
    j["pi"] = p;
  }
  {
    auto& io = N64::si.io; json s;
    s["SI_DRAM_ADDR"]    = hex(io.dramAddress);
    s["SI_PIF_RD_ADDR"]  = hex(io.readAddress);
    s["SI_PIF_WR_ADDR"]  = hex(io.writeAddress);
    s["SI_STATUS.dmaBusy"]   = (u32)io.dmaBusy;
    s["SI_STATUS.ioBusy"]    = (u32)io.ioBusy;
    s["SI_STATUS.interrupt"] = (u32)io.interrupt;
    j["si"] = s;
  }
  {
    auto& io = N64::ri.io; json r;
    r["RI_MODE"]    = hex(io.mode);
    r["RI_CONFIG"]  = hex(io.config);
    r["RI_SELECT"]  = hex(io.select);
    r["RI_REFRESH"] = hex(io.refresh);
    r["RI_LATENCY"] = hex(io.latency);
    j["ri"] = r;
  }
  return j;
}

// --- memory (Node::Debugger::Memory, same accessors as the Memory Editor) -----

static auto findMemoryNode(const std::string& want) -> ares::Node::Debugger::Memory {
  ares::Node::Debugger::Memory match;
  if(!emulator || !emulator->root) return match;
  nall::string w = want.c_str();
  for(auto node : ares::Node::enumerate<ares::Node::Debugger::Memory>(emulator->root)) {
    auto name = node->name();
    if(!want.empty() && name.iequals(w)) return node;          // exact (case-insensitive)
    if(!want.empty() && (bool)name.ifind(w) && !match) match = node;  // substring fallback
  }
  return match;
}

static auto jMemRegions() -> json {
  json arr = json::array();
  if(emulator && emulator->root) {
    for(auto node : ares::Node::enumerate<ares::Node::Debugger::Memory>(emulator->root)) {
      json r;
      r["name"] = sstr(node->name());
      r["size"] = node->size();
      arr.push_back(r);
    }
  }
  return arr;
}

// --- CPU profiler -------------------------------------------------------------

static auto jProfilerTop(const std::string& window, u32 limit) -> json {
  auto& prof = N64::cpu.profiler;
  auto& src = (window == "frame") ? prof.frameStats : prof.stats;

  std::vector<const N64::CPU::Profiler::FuncStat*> v;
  v.reserve(src.size());
  for(auto& kv : src) v.push_back(&kv.second);
  std::sort(v.begin(), v.end(), [](auto* a, auto* b) { return a->exclCycles > b->exclCycles; });

  json arr = json::array();
  u32 n = 0;
  for(auto* s : v) {
    if(n++ >= limit) break;
    json r;
    r["addr"]      = hex(s->addr);
    r["name"]      = sstr(prof.labelFor(s->addr));
    r["callCount"] = s->callCount;
    r["inclCycles"]= s->inclCycles;
    r["exclCycles"]= s->exclCycles;
    r["waitCycles"]= s->waitCycles;
    r["exclBytesIn"]  = s->exclBytesIn;
    r["exclBytesOut"] = s->exclBytesOut;
    arr.push_back(r);
  }
  json j;
  j["window"]  = window;
  j["entries"] = arr;
  return j;
}

// --- RSP / RDP command capture (mirror desktop-ui/ui/log-dump.cpp) ------------

static auto jRspCapture(u32 limit) -> json {
  auto& cap = N64::rsp.capture;
  u32 count = cap.committedCount.load(std::memory_order_acquire);
  count = std::min(count, limit);
  json arr = json::array();
  for(u32 k = 0; k < count; k++) {
    auto& c = cap.commands[k];
    json e;
    e["seq"]   = c.seq;
    e["cycle"] = c.cycle;
    e["frame"] = c.frame;
    e["bytesIn"]  = c.bytesIn;
    e["bytesOut"] = c.bytesOut;
    if(c.isOverhead) {
      static const char* oh[] = {"?", "RSPQ_Loop", "DMA ucode", "DMA cmd."};
      e["overlay"] = "RSPQ";
      e["command"] = oh[c.overheadType < 4 ? c.overheadType : 0];
      e["overhead"] = true;
    } else {
      u8 slot = c.overlayId & 15;
      auto& on = cap.overlayNameMap[slot];
      e["overlay"] = on ? sstr(on) : sstr(nall::hex(slot));
      auto& cn = cap.commandNameMap[slot][c.commandId];
      e["command"] = cn ? sstr(cn) : sstr(nall::hex(c.commandId, 2L));
      e["args"] = sstr(cap.formatArgs(c.overlayId, c.commandId, c.words, c.wordCount));
    }
    arr.push_back(e);
  }
  json j;
  j["count"]   = count;
  j["entries"] = arr;
  return j;
}

static auto jRdpCapture(u32 limit) -> json {
  auto& cap = N64::rdp.capture;
  u32 count = cap.committedCount.load(std::memory_order_acquire);
  count = std::min(count, limit);
  json arr = json::array();
  for(u32 k = 0; k < count; k++) {
    auto& c = cap.commands[k];
    json e;
    e["seq"]    = c.seq;
    e["frame"]  = c.frame;
    e["opcode"] = hex(c.opcode);
    e["name"]   = N64::rdpCommandName(c.opcode);
    e["word0"]  = hex(c.word0);
    e["word1"]  = hex(c.word1);
    arr.push_back(e);
  }
  json j;
  j["count"]   = count;
  j["entries"] = arr;
  return j;
}

// --- dispatch ----------------------------------------------------------------

auto Server::handleLine(const std::string& line) -> void {
  json out;
  json in;
  try {
    in = json::parse(line);
  } catch(...) {
    send(json{{"ok", false}, {"error", "invalid JSON request"}}.dump());
    return;
  }

  out["id"] = in.value("id", 0);
  std::string cmd = in.value("cmd", std::string());
  json args = in.value("args", json::object());

  auto fail = [&](const std::string& msg) {
    out["ok"] = false; out["error"] = msg; send(out.dump());
  };
  auto ok = [&](json data) {
    out["ok"] = true; out["data"] = std::move(data); send(out.dump());
  };

  // Always-available status; everything else needs a running N64.
  if(cmd == "status") {
    json d;
    d["loaded"] = (bool)emulator;
    d["system"] = emulator ? sstr(emulator->name) : std::string();
    d["paused"] = program.paused;
    d["vps"]    = (u64)program.vblanksPerSecond.load();
    d["gdbHalted"] = nall::GDB::server.isHalted();
    if(n64Ready()) d["frame"] = N64::rsp.capture.frameNumber;
    return ok(d);
  }

  if(!n64Ready()) return fail("no Nintendo 64 game loaded");

  if(cmd == "cpu.regs")    return ok(jCpu());
  if(cmd == "rsp.regs")    return ok(jRsp());
  if(cmd == "rdp.state")   return ok(jRdp());
  if(cmd == "periph.regs") return ok(jPeripherals());
  if(cmd == "mem.regions") return ok(jMemRegions());

  if(cmd == "profiler.top")
    return ok(jProfilerTop(args.value("window", std::string("continuous")),
                           args.value("limit", 32u)));
  if(cmd == "rsp.capture") return ok(jRspCapture(args.value("limit", 4096u)));
  if(cmd == "rdp.capture") return ok(jRdpCapture(args.value("limit", 4096u)));

  // Encode the last presented core frame (exact emulated output) to a PNG file.
  // Works while running or paused; captures whatever Program::video() last saw.
  if(cmd == "video.screenshot") {
    std::string path = args.value("path", std::string());
    if(path.empty()) return fail("path required");
    if(program.lastFrame.empty() || !program.lastFrameWidth || !program.lastFrameHeight)
      return fail("no frame captured yet");
    u32 w = program.lastFrameWidth, h = program.lastFrameHeight;
    bool okp = nall::Encode::PNG::RGB8(nall::string(path.c_str()), program.lastFrame.data(),
                                       program.lastFramePitch, w, h);
    if(!okp) return fail("PNG encode failed (bad path?)");
    json d; d["path"] = path; d["width"] = w; d["height"] = h;
    return ok(d);
  }

  // Reconstruct the N64 framebuffer straight from RDRAM (VI_ORIGIN + VI regs) and
  // encode it to PNG. Unlike video.screenshot (which captures the post-Vulkan
  // presented frame and collapses to 1x1 when the GPU scanout is invalid, e.g. a
  // headless/background launch), this reads the raw framebuffer the game rendered,
  // so it always works regardless of the host GPU/window state.
  if(cmd == "video.framebuffer") {
    std::string path = args.value("path", std::string());
    if(path.empty()) return fail("path required");
    auto& io = N64::vi.io;
    u32 origin = io.dramAddress & 0x7FFFFF;   // RDRAM byte offset
    u32 width  = io.width;                     // framebuffer stride in pixels
    u32 depth  = io.colorDepth;                // 0/1=blank, 2=16bpp(5551), 3=32bpp(8888)
    if(!width || (depth != 2 && depth != 3))
      return fail("VI not displaying a framebuffer (blank or invalid colorDepth)");
    // Active source height from VI vertical timing scaled by yscale (2.10 fixed).
    u32 vs = io.vstart, ve = io.vend;
    u32 lines = ve > vs ? (ve - vs) >> 1 : 0;
    u32 yscale = (u32)io.yscale;
    u32 height = (u32)(((u64)lines * (yscale ? yscale : 0x400u)) >> 10);
    if(!height) height = lines;
    if(!height) return fail("VI vertical timing empty");
    if(height > 640) height = 640;
    auto node = findMemoryNode("RDRAM");
    if(!node) return fail("no RDRAM region");
    u32 rsize = node->size();
    auto rd8 = [&](u32 off) -> u8 { return rsize ? (u8)node->read(off % rsize) : 0; };
    std::vector<u32> pix((size_t)width * height);
    if(depth == 2) {  // RGBA5551, big-endian
      for(u32 y = 0; y < height; y++) for(u32 x = 0; x < width; x++) {
        u32 off = origin + ((u64)y * width + x) * 2;
        u16 p = (rd8(off) << 8) | rd8(off + 1);
        u8 r5 = (p >> 11) & 0x1F, g5 = (p >> 6) & 0x1F, b5 = (p >> 1) & 0x1F;
        u8 r = (r5 << 3) | (r5 >> 2), g = (g5 << 3) | (g5 >> 2), b = (b5 << 3) | (b5 >> 2);
        pix[(size_t)y * width + x] = (u32)r << 16 | (u32)g << 8 | b;
      }
    } else {          // RGBA8888
      for(u32 y = 0; y < height; y++) for(u32 x = 0; x < width; x++) {
        u32 off = origin + ((u64)y * width + x) * 4;
        pix[(size_t)y * width + x] = (u32)rd8(off) << 16 | (u32)rd8(off + 1) << 8 | rd8(off + 2);
      }
    }
    bool okp = nall::Encode::PNG::RGB8(nall::string(path.c_str()), pix.data(),
                                       width * 4, width, height);
    if(!okp) return fail("PNG encode failed (bad path?)");
    json d; d["path"] = path; d["width"] = width; d["height"] = height;
    d["origin"] = hex(io.dramAddress); d["colorDepth"] = depth;
    return ok(d);
  }

  // Audio capture: reads the frontend's post-mix tap (Program::audioCapture, the
  // same ring buffer the audio viewer uses), so it records exactly what is sent to
  // the host audio device. The ring holds ~4s; a capture marks the current write
  // position, then audio.save writes the samples produced since, as a 16-bit PCM
  // stereo WAV at the host output rate.
  if(cmd == "audio.capture") {
    auto& c = program.audioCapture;
    f64 maxSeconds = (f64)AudioCapture::bufferSize / audioRate();
    f64 seconds = std::min<f64>(args.value("seconds", 3.0), maxSeconds);
    audioCap.start = c.writePos.load(std::memory_order_acquire);
    audioCap.frames = (u32)(seconds * audioRate());
    audioCap.active = true;
    json d;
    d["capturing"] = true;
    d["seconds"]   = seconds;
    d["rate"]      = audioRate();
    d["cap"]       = audioCap.frames;
    return ok(d);
  }

  if(cmd == "audio.status") {
    json d;
    d["active"] = audioCap.active;
    d["count"]  = audioCapturedFrames();
    d["cap"]    = audioCap.frames;
    d["rate"]   = audioRate();
    d["done"]   = !audioCap.active || audioCapturedFrames() >= audioCap.frames;
    return ok(d);
  }

  if(cmd == "audio.save") {
    std::string path = args.value("path", std::string());
    if(path.empty()) return fail("path required");
    if(!audioCap.active) return fail("no capture started (call audio.capture first)");
    auto& c = program.audioCapture;
    u32 frames = audioCapturedFrames();
    u32 rate   = audioRate();
    u32 dataBytes = frames * 2 * 2;  // stereo * 16-bit
    FILE* f = fopen(path.c_str(), "wb");
    if(!f) return fail("cannot open output path");
    auto w32 = [&](u32 v){ u8 b[4]={(u8)v,(u8)(v>>8),(u8)(v>>16),(u8)(v>>24)}; fwrite(b,1,4,f); };
    auto w16 = [&](u16 v){ u8 b[2]={(u8)v,(u8)(v>>8)}; fwrite(b,1,2,f); };
    fwrite("RIFF",1,4,f); w32(36 + dataBytes); fwrite("WAVE",1,4,f);
    fwrite("fmt ",1,4,f); w32(16); w16(1); w16(2);          // PCM, 2ch
    w32(rate); w32(rate * 2 * 2); w16(4); w16(16);          // byterate, block align, bits
    fwrite("data",1,4,f); w32(dataBytes);
    auto pcm = [](f32 s) -> u16 { s = std::clamp(s, -1.0f, 1.0f); return (u16)(s16)(s * 32767.0f); };
    for(u32 i = 0; i < frames; i++) {
      u32 pos = (audioCap.start + i) % AudioCapture::bufferSize;
      w16(pcm(c.left[pos]));
      w16(pcm(c.right[pos]));
    }
    fclose(f);
    audioCap.active = false;
    json d; d["path"] = path; d["frames"] = frames; d["rate"] = rate;
    d["seconds"] = frames / (double)rate;
    return ok(d);
  }

  if(cmd == "mem.read") {
    auto node = findMemoryNode(args.value("region", std::string()));
    if(!node) return fail("unknown memory region");
    u32 addr = args.value("addr", 0u);
    u32 len  = std::min<u32>(args.value("len", 16u), 65536u);
    u32 size = node->size();
    std::string hexbytes;
    hexbytes.reserve(len * 2);
    char b[3];
    for(u32 i = 0; i < len; i++) {
      u8 v = size ? (u8)node->read((addr + i) % size) : (u8)0;
      snprintf(b, sizeof(b), "%02X", v);
      hexbytes += b;
    }
    json d;
    d["region"] = sstr(node->name());
    d["addr"]   = hex(addr);
    d["len"]    = len;
    d["hex"]    = hexbytes;
    return ok(d);
  }

  if(cmd == "mem.write") {
    auto node = findMemoryNode(args.value("region", std::string()));
    if(!node) return fail("unknown memory region");
    std::string hexbytes = args.value("hex", std::string());
    if(hexbytes.size() % 2) return fail("hex must have an even length");
    u32 addr = args.value("addr", 0u);
    u32 size = node->size();
    u32 n = 0;
    for(size_t i = 0; i + 1 < hexbytes.size(); i += 2) {
      u8 v = (u8)strtoul(hexbytes.substr(i, 2).c_str(), nullptr, 16);
      if(size) node->write((addr + n) % size, v);
      n++;
    }
    json d; d["region"] = sstr(node->name()); d["addr"] = hex(addr); d["written"] = n;
    return ok(d);
  }

  if(cmd == "ctrl.pause")  { program.pause(true);  return ok(json{{"paused", true}}); }
  if(cmd == "ctrl.resume") { program.pause(false); return ok(json{{"paused", false}}); }
  if(cmd == "ctrl.step") {
    // One presented frame; the run loop consumes requestFrameAdvance even while paused.
    program.requestFrameAdvance = true;
    return ok(json{{"stepped", 1}});
  }

  // --- controller injection (drive the pad headless from the MCP) ------------
  if(cmd == "input.set") {
    auto& ov = inputOverride;
    u32 b = 0;
    auto B = [&](const char* k, u32 bit) { if(args.value(k, false)) b |= bit; };
    B("a",Pad_A); B("b",Pad_B); B("z",Pad_Z); B("start",Pad_Start);
    B("up",Pad_Up); B("down",Pad_Down); B("left",Pad_Left); B("right",Pad_Right);
    B("cup",Pad_CUp); B("cdown",Pad_CDown); B("cleft",Pad_CLeft); B("cright",Pad_CRight);
    B("l",Pad_L); B("r",Pad_R);
    ov.buttons = b;
    s32 x = args.value("x", 0), y = args.value("y", 0);
    ov.x = x < -32767 ? -32767 : x > 32767 ? 32767 : x;
    ov.y = y < -32767 ? -32767 : y > 32767 ? 32767 : y;
    ov.active = true;
    return ok(jInput());
  }
  if(cmd == "input.release") { inputOverride.active = false; return ok(jInput()); }
  if(cmd == "input.state")   { return ok(jInput()); }

  // --- one-shot render/liveness probe (collapses the "is it rendering?" dance) -
  if(cmd == "render.probe") {
    auto& vio = N64::vi.io;
    u32 origin = vio.dramAddress;
    s64 dpc    = (s64)N64::rdp.command.clock;
    u64 frame  = N64::rsp.capture.frameNumber;
    bool viActive = vio.hstart != 0 && vio.vstart != 0;

    static bool have = false;
    static u32 lastOrigin = 0; static s64 lastDpc = 0; static u64 lastFrame = 0;

    json d;
    d["viActive"] = viActive;
    d["viOrigin"] = hex(origin);
    d["dpcClock"] = hex((u64)dpc);
    d["frame"]    = frame;
    d["vps"]      = (u64)program.vblanksPerSecond.load();
    d["paused"]   = program.paused;
    if(have) {
      bool oc = origin != lastOrigin, dc = dpc != lastDpc, fc = frame != lastFrame;
      d["originChanged"] = oc;
      d["dpcAdvanced"]   = dc;
      d["frameAdvanced"] = fc;
      d["rendering"]     = viActive && oc && dc && fc;
    } else {
      // First call has no baseline for deltas; call again to get them.
      d["originChanged"] = nullptr;
      d["dpcAdvanced"]   = nullptr;
      d["frameAdvanced"] = nullptr;
      d["rendering"]     = nullptr;
    }
    lastOrigin = origin; lastDpc = dpc; lastFrame = frame; have = true;
    return ok(d);
  }

  if(cmd == "profiler.enable") {
    N64::cpu.profiler.setEnabled(args.value("on", true));
    return ok(json{{"enabled", args.value("on", true)}});
  }
  if(cmd == "profiler.clear") { N64::cpu.profiler.clearStats(); return ok(json{{"cleared", true}}); }

  // --- RSP instruction-level debugger (single-step + breakpoints) ------------
  // Engaging debug mode forces the RSP interpreter so the per-instruction gate in
  // instructionPrologue fires; the RSP then freezes at instruction boundaries and
  // this server is pumped from inside that freeze (see RSP::dbgStopSpin). Typical
  // flow: rsp.debug {on:true} -> rsp.break {addr} -> rsp.run -> rsp.regs -> rsp.step.
  if(cmd == "rsp.debug") {
    auto& cap = N64::rsp.capture;
    bool on = args.value("on", true);
    if(on && !cap.dbgActive.load()) {
      telRspSavedRecompiler = N64::rsp.recompiler.enabled;
      N64::rsp.recompiler.enabled = false;   // recompiled blocks skip the prologue hook
      cap.dbgResumeSkipBp = false;
      cap.dbgRunToBp.store(false);
      cap.dbgStepsRemaining.store(0);
      cap.dbgActive.store(true);             // stops at the next RSP instruction
    } else if(!on && cap.dbgActive.load()) {
      cap.dbgActive.store(false);            // releases the freeze; RSP resumes normally
      cap.dbgRunToBp.store(false);
      cap.dbgStepsRemaining.store(0);
      N64::rsp.recompiler.enabled = telRspSavedRecompiler;
    }
    json d;
    d["active"]  = (bool)cap.dbgActive.load();
    d["stopped"] = (bool)cap.dbgStopped.load();
    d["curPc"]   = hex(cap.dbgCurPc.load());
    return ok(d);
  }

  if(cmd == "rsp.break") {
    auto& cap = N64::rsp.capture;
    std::string act = args.value("action", std::string("add"));
    if(act == "add") {
      if(!cap.dbgAddBreakpoint(args.value("addr", 0u))) return fail("breakpoint table full");
    } else if(act == "remove") {
      cap.dbgRemoveBreakpoint(args.value("addr", 0u));
    } else if(act == "clear") {
      cap.dbgClearBreakpoints();
    } else if(act != "list") {
      return fail("rsp.break action must be add|remove|clear|list");
    }
    json bps = json::array();
    for(u32 i = 0; i < cap.dbgBreakpointCount; i++) bps.push_back(hex(cap.dbgBreakpoints[i]));
    return ok(json{{"breakpoints", bps}});
  }

  if(cmd == "rsp.step") {
    auto& cap = N64::rsp.capture;
    if(!cap.dbgActive.load()) return fail("rsp.debug not active");
    u32 n = args.value("count", 1u); if(n == 0) n = 1;
    // Ack only; the step executes as the freeze unwinds. Poll rsp.regs for the
    // resulting PC (a synchronous client will see it on its next request).
    cap.dbgResumeSkipBp = true;                 // step off a breakpoint we're sitting on
    cap.dbgStepsRemaining.store(n, std::memory_order_release);
    return ok(json{{"stepping", n}, {"fromPc", hex(cap.dbgCurPc.load())}});
  }

  if(cmd == "rsp.run") {
    auto& cap = N64::rsp.capture;
    if(!cap.dbgActive.load()) return fail("rsp.debug not active");
    cap.dbgResumeSkipBp = true;                 // don't immediately re-trigger the current bp
    cap.dbgRunToBp.store(true, std::memory_order_release);
    return ok(json{{"running", true}});
  }

  if(cmd == "rsp.stop") {
    // Ask the RSP to freeze at the next instruction boundary. Takes effect once the
    // gate next runs (immediately if already stepping; otherwise next frame).
    auto& cap = N64::rsp.capture;
    if(!cap.dbgActive.load()) return fail("rsp.debug not active");
    cap.dbgRunToBp.store(false, std::memory_order_release);
    cap.dbgStepsRemaining.store(0, std::memory_order_release);
    return ok(json{{"stopRequested", true}});
  }

  if(cmd == "capture.enable") {
    if(args.contains("rsp")) N64::rsp.capture.enabled.store((bool)args["rsp"], std::memory_order_release);
    if(args.contains("rdp")) N64::rdp.capture.enabled.store((bool)args["rdp"], std::memory_order_release);
    json d;
    d["rsp"] = N64::rsp.capture.enabled.load();
    d["rdp"] = N64::rdp.capture.enabled.load();
    return ok(d);
  }

  return fail("unknown cmd: " + cmd);
}

// --- framing -----------------------------------------------------------------

auto Server::send(const std::string& text) -> void {
  std::string line = text;
  line += '\n';
  sendData((const u8*)line.data(), (u32)line.size());
}

auto Server::onData(const std::vector<u8>& data) -> void {
  rxBuffer.append((const char*)data.data(), data.size());
  size_t nl;
  while((nl = rxBuffer.find('\n')) != std::string::npos) {
    std::string line = rxBuffer.substr(0, nl);
    rxBuffer.erase(0, nl + 1);
    if(!line.empty() && line.back() == '\r') line.pop_back();
    if(!line.empty()) handleLine(line);
  }
}

}
