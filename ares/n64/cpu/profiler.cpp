// In-game CPU cost profiler. Instruments calls (JAL/JALR) and returns (JR $ra)
// while enabled to build per-function call counts + inclusive/exclusive cycle
// costs, and loads function symbols from the ROM's .elf for naming + name-based
// spin/wait classification. See profiler struct in cpu.hpp.

namespace {

// --- Minimal big-endian ELF symbol-table reader (ELF32/ELF64) ---------------
// Mirrors the parser in rsp/rsp-commands.cpp, but collects STT_FUNC symbols
// (name + value + size) in the KSEG0 RDRAM range instead of rsp_* labels.

static auto elfRead64BE(const u8* d, u32 o) -> u64 {
  u64 v = 0; for(int i = 0; i < 8; i++) v = (v << 8) | d[o + i]; return v;
}
static auto elfRead32BE(const u8* d, u32 o) -> u32 {
  return (u32(d[o]) << 24) | (u32(d[o+1]) << 16) | (u32(d[o+2]) << 8) | u32(d[o+3]);
}
static auto elfRead16BE(const u8* d, u32 o) -> u16 {
  return (u16(d[o]) << 8) | u16(d[o+1]);
}

struct ElfFuncSym { string name; u32 value; u32 size; };

static auto elfCollectFuncSymbols(const u8* data, u32 size) -> std::vector<ElfFuncSym> {
  std::vector<ElfFuncSym> out;
  if(!data || size < 64) return out;
  if(data[0] != 0x7f || data[1] != 'E' || data[2] != 'L' || data[3] != 'F') return out;
  u8 elfClass = data[4], elfData2 = data[5];
  if(elfData2 != 2) return out;             //big-endian only
  if(elfClass != 1 && elfClass != 2) return out;
  bool is64 = (elfClass == 2);

  u64 shoff; u32 shentsize, shnum;
  u32 shdrSize, shOffOff, shSizeOff;
  if(is64) { shoff = elfRead64BE(data, 40); shentsize = elfRead16BE(data, 58); shnum = elfRead16BE(data, 60);
             shdrSize = 64; shOffOff = 24; shSizeOff = 32; }
  else     { shoff = elfRead32BE(data, 32); shentsize = elfRead16BE(data, 46); shnum = elfRead16BE(data, 48);
             shdrSize = 40; shOffOff = 16; shSizeOff = 20; }
  if(!shoff || !shnum) return out;

  // Locate .symtab and the linked .strtab via the symtab's sh_link.
  u64 strtabOff = 0, strtabSz = 0, symtabOff = 0, symtabSz = 0, symEnt = 0;
  // First pass: find SHT_SYMTAB (type 2) and its sh_link; collect all section
  // offsets/sizes so we can resolve the linked string table.
  u32 symLink = ~0u;
  for(u32 i = 0; i < shnum; i++) {
    u32 hdrOff = shoff + i * shentsize;
    if(hdrOff + shdrSize > size) break;
    u32 type = elfRead32BE(data, hdrOff + 4);
    if(type == 2) {  //SHT_SYMTAB
      symtabOff = is64 ? elfRead64BE(data, hdrOff + shOffOff)  : elfRead32BE(data, hdrOff + shOffOff);
      symtabSz  = is64 ? elfRead64BE(data, hdrOff + shSizeOff) : elfRead32BE(data, hdrOff + shSizeOff);
      symEnt    = is64 ? elfRead64BE(data, hdrOff + 56)        : elfRead32BE(data, hdrOff + 36);
      symLink   = elfRead32BE(data, hdrOff + (is64 ? 40 : 24));  //sh_link: ELF64 @40, ELF32 @24
    }
  }
  if(symLink != ~0u && symLink < shnum) {
    u32 hdrOff = shoff + symLink * shentsize;
    if(hdrOff + shdrSize <= size) {
      strtabOff = is64 ? elfRead64BE(data, hdrOff + shOffOff)  : elfRead32BE(data, hdrOff + shOffOff);
      strtabSz  = is64 ? elfRead64BE(data, hdrOff + shSizeOff) : elfRead32BE(data, hdrOff + shSizeOff);
    }
  }
  if(!symtabOff || !strtabOff || !symEnt) return out;

  // ELF32 sym: name@0 value@4 size@8 info@12 ; ELF64 sym: name@0 info@4 value@8 size@16
  u32 valOff  = is64 ? 8  : 4;
  u32 sizeOff = is64 ? 16 : 8;
  u32 infoOff = is64 ? 4  : 12;
  for(u64 s = 0; s + symEnt <= symtabSz; s += symEnt) {
    u32 symOff = symtabOff + s;
    if(symOff + symEnt > size) break;
    u8  info    = data[symOff + infoOff];
    if((info & 0xf) != 2) continue;  //STT_FUNC only
    u32 nameIdx = elfRead32BE(data, symOff);
    u64 val     = is64 ? elfRead64BE(data, symOff + valOff)  : elfRead32BE(data, symOff + valOff);
    u64 ssize   = is64 ? elfRead64BE(data, symOff + sizeOff) : elfRead32BE(data, symOff + sizeOff);
    if(val < 0x8000'0000ull || val >= 0x8080'0000ull) continue;  //KSEG0 RDRAM
    if(strtabOff + nameIdx >= size) continue;
    const char* nm = (const char*)(data + strtabOff + nameIdx);
    if(!nm[0]) continue;
    out.push_back({string{nm}, (u32)val, (u32)ssize});
  }
  return out;
}

// Demangle a C++ symbol name (e.g. "_Z6myFuncii" ->  "myFunc(int, int)"). Non-C++ (C / libdragon) names are returned unchanged.
static auto demangle(const string& name) -> string {
#ifdef ARES_HAS_CXA_DEMANGLE
  if(name.beginsWith("_Z")) {
    int status = 0;
    char* out = abi::__cxa_demangle(name.data(), nullptr, nullptr, &status);
    if(out) {
      string result = (status == 0) ? string{out} : name;
      free(out);
      return result;
    }
  }
#endif
  return name;
}

// Functions whose name matches one of the configured substring patterns (from "cpuWaitFunctions" in the JSON config) 
// are treated as spin/wait time rather than active CPU work.
static auto nameLooksSpin(const string& n, const std::vector<string>& patterns) -> bool {
  for(auto& p : patterns) if((bool)n.ifind(p)) return true;
  return false;
}

static auto lastSlash(const string& s) -> s32 {
  s32 idx = -1;
  for(s32 i = 0; i < (s32)s.size(); i++) if(s[i] == '/' || s[i] == '\\') idx = i;
  return idx;
}
static auto dirOf(const string& s) -> string {
  s32 k = lastSlash(s); string d; for(s32 i = 0; i < k; i++) d.append(s[i]); return d;
}
static auto baseOf(const string& s) -> string {
  s32 k = lastSlash(s); string b; for(s32 i = k + 1; i < (s32)s.size(); i++) b.append(s[i]); return b;
}

}  // namespace

auto CPU::Profiler::now() -> u64 {
  // Master-clock timebase (187.5 MHz). synchronize() folds cpu.clock into
  // profile.cpuCycles as (clocks >> 1), i.e. CPU cycles, so scale that back up
  // by 2 and add the still-pending master clocks. A COUNT read (flushCount)
  // folds the pending clocks early without resetting cpu.clock, tracking how
  // much it took in countClock: subtract that or those clocks would be counted
  // twice until the next synchronize, which made timestamps jump ahead and then
  // fall back. This may still step backwards by at most 1 master clock at a
  // sync boundary (the truncated low bit); callers guard their subtractions.
  return (u64)(cpu.profile.cpuCycles * 2 + cpu.clock - cpu.countClock);
}

auto CPU::Profiler::loadSymbols(const string& romPath) -> bool {
  setFunctionMarker(0);
  symbolsLoaded = false; symbolCount = 0;
  syms.clear(); symByAddr.clear();

  // Prefer the ELF the RSP capture already located + cached (avoids re-reading).
  string elfData = rsp.capture.cachedElfData;

  if(!elfData) {
    // Own search: rom.z64 -> rom.elf, rom/build/rom.elf, parent/build/rom.elf.
    string base = romPath;
    s32 dot = -1;
    for(s32 i = 0; i < (s32)base.size(); i++) if(base[i] == '.') dot = i;
    if(dot >= 0) base.resize(dot);
    string candidates[] = {
      {base, ".elf"},
      {base, "/build/", baseOf(base), ".elf"},
      {dirOf(base), "/build/", baseOf(base), ".elf"},
    };
    for(auto& c : candidates) { elfData = string::read(c); if(elfData) break; }
  }
  if(!elfData) return false;

  auto funcs = elfCollectFuncSymbols((const u8*)elfData.data(), elfData.size());
  if(funcs.empty()) return false;

  for(auto& f : funcs) {
    string name = demangle(f.name);
    syms.push_back({f.value, f.size, name, nameLooksSpin(name, rsp.capture.cpuWaitPatterns)});
  }
  std::sort(syms.begin(), syms.end(), [](auto& a, auto& b) { return a.addr < b.addr; });
  for(u32 i = 0; i < syms.size(); i++) symByAddr[syms[i].addr] = i;

  symbolsLoaded = true;
  symbolCount = syms.size();
  return true;
}

auto CPU::Profiler::resolve(u32 addr) -> Sym* {
  if(auto it = symByAddr.find(addr); it != symByAddr.end()) return &syms[it->second];
  if(syms.empty()) return nullptr;
  // Binary search for the symbol whose [addr, addr+size) range encloses addr.
  u32 lo = 0, hi = (u32)syms.size();
  while(lo < hi) { u32 mid = (lo + hi) >> 1; if(syms[mid].addr <= addr) lo = mid + 1; else hi = mid; }
  if(lo == 0) return nullptr;
  auto& s = syms[lo - 1];
  if(addr >= s.addr && (s.size == 0 || addr < s.addr + s.size)) return &s;
  return nullptr;
}

auto CPU::Profiler::refreshWaitFunctions() -> void {
  if(enabled.load(std::memory_order_relaxed)) flushFrames();
  for(auto& sym : syms) sym.isSpin = nameLooksSpin(sym.name, rsp.capture.cpuWaitPatterns);
}

auto CPU::Profiler::setFunctionMarker(u32 addr) -> void {
  if(enabled.load(std::memory_order_relaxed)) flushFrames();
  markerAddr = addr;
  markerActive = false;
  markerReady = false;
  markerStats.clear();
  markerAccum.clear();
  auto reset = [](auto& frames) {
    for(auto& f : frames) f.markerRoot = f.markerPending = false;
  };
  reset(callStack);
  for(auto& s : suspended) reset(s.frames);
}

auto CPU::Profiler::onInstruction(u64 address, u32 instruction) -> void {
  u32 pc32 = (u32)address;

  // Per-thread call-stack tracking. The live $sp identifies which stack we are on.
  // Suppressed while an exception handler is open: its code runs on the kernel/interrupt stack
  // but is meant to nest under the interrupted function, so we keep it on the active
  // stack and let the eret below hand off to the resumed thread on the next step.
  u32 sp = (u32)cpu.ipu.r[29].u64;  //$sp
  if(excActive == 0) {
    if(!haveSp) { haveSp = true; spLo = spHi = sp; }
    else if(sp + regionSlack < spLo || sp > spHi + regionSlack) switchStack(sp);
    else { if(sp < spLo) spLo = sp; if(sp > spHi) spHi = sp; }
  }

  // Stack-pointer backstop for returns the retAddr match can never see (missed `jr` hooks, longjmp, a thread blocking mid-call): 
  // once the live $sp has risen above a frame's recorded caller-$sp, that call and everything nested in it have unwound, so pop them. 
  // Stops at exception frames (carry no $sp, popped by onEret), keeping the interrupted thread's frames beneath a handler intact
  while(!callStack.empty() && !callStack.back().isException && sp > callStack.back().sp) {
    popFrame();
  }

  // Return detection by *arrival* at a call's return address, rather than by
  // catching the `jr $ra` instruction. The JIT does not reliably emit the
  // per-instruction hook for a `jr` reached as the fall-through of a preceding
  // branch (back-to-back branches), so the return itself can be invisible. 
  // but the return *target* is always a normal instruction in the (instrumented)
  // caller. When execution arrives at the top frame's recorded return address,
  // that call has returned: pop it. One pop per arrival so recursion that returns
  // to a shared call site unwinds a single level at a time. Exception frames are
  // popped by onEret, not here, and never matched (their retAddr is 0).
  if(!callStack.empty() && !callStack.back().isException && callStack.back().retAddr == pc32) {
    popFrame();
  }

  // Open the window on arrival, after the caller's branch delay slot.
  if(!callStack.empty() && callStack.back().markerPending && callStack.back().funcAddr == pc32) {
    flushFrames();
    auto& f = callStack.back();
    f.markerPending = false;
    if(!markerActive) {
      markerAccum.clear();
      markerActive = f.markerRoot = true;
      auto& st = markerAccum[f.funcAddr];
      st.addr = f.funcAddr;
      st.callCount = 1;
    }
  }

  u32 op = instruction >> 26;
  bool isJAL  = (op == 0x03);
  bool isJALR = (op == 0x00) && ((instruction & 0x3f) == 0x09);

  if(isJAL || isJALR) {
    // Only standard calls that link into $ra (rd=31 for JALR) form a call/return
    // pair we can balance; a JALR linking elsewhere returns via a register we
    // don't track, so treating it as a call would just leak a frame.
    if(isJALR && ((instruction >> 11) & 31) != 31) return;
    if(callStack.size() >= maxStackDepth) return;  //runaway guard
    u32 target = isJAL
      ? ((pc32 & 0xf000'0000) | ((instruction & 0x03ff'ffff) << 2))
      : (u32)cpu.ipu.r[(instruction >> 21) & 31].u64;  //JALR rs holds the target
    Frame f;
    f.funcAddr = target;
    f.retAddr = pc32 + 8;  //where the callee returns to (after the delay slot)
    f.sp = sp;             //caller's $sp at the call site (see the unwind backstop above)
    f.entryCycle = f.segmentCycle = now();
    if(markerAddr) {
      if(auto sym = resolve(target)) f.markerPending = sym->addr == markerAddr;
    }
    callStack.push_back(f);
    syncOpenFrames();
  }
}

// Re-publish the open call stack for the flame chart (see Profiler::openFrames).
// Cheap enough to call from every callStack mutation: stacks are shallow in
// practice and popFrame() already does two hash-map updates per call.
auto CPU::Profiler::syncOpenFrames() -> void {
  if(!recordTimeline.load(std::memory_order_relaxed)) {
    openDepth.store(0, std::memory_order_release);
    return;
  }
  u32 n = (u32)min<size_t>(callStack.size(), maxOpenFrames);
  for(u32 i : range(n)) {
    auto& f = callStack[i];
    openFrames[i] = {f.entryCycle, f.funcAddr, f.isException};
  }
  openDepth.store(n, std::memory_order_release);
}

// Memory-bus bytes committed to RDRAM during the current frame's execution.
// Attributed in full to the function on top of the call stack at the time of the
// access, mirroring how exclusive cycles are charged. The inclusive total is
// rolled up into the caller on popFrame.
auto CPU::Profiler::onBusAccess(bool toRDRAM, u64 bytes) -> void {
  if(callStack.empty()) return;  //bytes outside any tracked frame are dropped
  if(toRDRAM) callStack.back().bytesOutOwn += bytes;
  else        callStack.back().bytesInOwn  += bytes;
}

// Cache line transfer (icache fill, dcache fill or dcache write-back), attributed
// to the function on top of the call stack like onBusAccess (the burst itself is
// also counted there as RDRAM in/out bytes; this keeps the cache share separate).
// Must be called before the transfer's step(), so `time` is its start. Also
// appended to the event ring for the flame chart.
auto CPU::Profiler::onCacheFill(u8 kind, u32 paddr) -> u64 {
  u32 funcAddr = 0;
  bool isException = false;
  if(!callStack.empty()) {
    auto& top = callStack.back();
    top.cacheOwn[kind] += cacheBytes[kind];
    funcAddr = top.funcAddr;
    isException = top.isException;
  }
  if(!recordTimeline.load(std::memory_order_relaxed) || cacheEvents.size() != maxCacheEvents) return 0;
  u64 w = cacheEventWrite.load(std::memory_order_relaxed);
  auto& e = cacheEvents[w % maxCacheEvents];
  e = {};
  e.time = now();
  e.paddr = paddr;
  e.funcAddr = funcAddr;
  e.causePc = (u32)cpu.ipu.pc;
  e.kind = kind;
  e.isException = isException;
  //reuse distance: fills into this cache since this line was last evicted
  if(kind != CacheDWrite) {
    u32 c = kind == CacheIFill ? 0 : 1;
    u64 seq = ++fillSeq[c];
    u32 index = paddr >> (c == 0 ? 5 : 4);
    if(index < lineHistory[c].size()) {
      auto& h = lineHistory[c][index];
      if(h.fillSeq) {
        e.reuseFills = (u32)std::min<u64>(seq - h.fillSeq, 0xffff'ffff);
        e.reuseTime = e.time > h.time ? e.time - h.time : 0;
        e.prevEvictorPc = h.evictorPc;
        e.prevEvictorFuncAddr = h.evictorFuncAddr;
      }
    }
  }
  cacheEventWrite.store(w + 1, std::memory_order_release);
  return w + 1;  //id 0 is "no event"
}

auto CPU::Profiler::onCacheContent(u64 eventId, const u32* words, u32 count) -> void {
  if(!eventId) return;
  u64 w = cacheEventWrite.load(std::memory_order_relaxed);
  if(w - (eventId - 1) > maxCacheEvents || cacheEvents.size() != maxCacheEvents) return;
  auto& e = cacheEvents[(eventId - 1) % maxCacheEvents];
  for(u32 i = 0; i < count && i < 8; i++) e.words[i] = words[i];
  e.wordCount = count;
}

// The line a fill event describes has been replaced (or invalidated): record
// how much of it was used while it was resident, and who pushed it out. The
// event may already have been overwritten in the ring; then it is simply lost.
auto CPU::Profiler::onCacheEvict(u64 eventId, u16 readMask, u16 writeMask) -> void {
  if(!eventId) return;
  u64 w = cacheEventWrite.load(std::memory_order_relaxed);
  if(w - (eventId - 1) > maxCacheEvents || cacheEvents.size() != maxCacheEvents) return;
  auto& e = cacheEvents[(eventId - 1) % maxCacheEvents];
  e.readMask = readMask;
  e.writeMask = writeMask;
  e.evictTime = now();
  e.evictorPc = (u32)cpu.ipu.pc;
  e.evictorFuncAddr = callStack.empty() ? 0 : callStack.back().funcAddr;
  //remember the eviction for the reuse distance of the line's next fill
  if(e.kind != CacheDWrite) {
    u32 c = e.kind == CacheIFill ? 0 : 1;
    u32 index = e.paddr >> (c == 0 ? 5 : 4);
    if(index < lineHistory[c].size()) {
      lineHistory[c][index] = {fillSeq[c], e.evictTime, e.evictorPc, e.evictorFuncAddr};
    }
  }
}

// Cache-line utilisation. Every instruction passes through here while profiling,
// so mark the icache word being executed and, for loads, the bytes about to be
// read from their dcache line. Only KSEG0 (the cached, directly mapped segment
// all homebrew runs in) is followed; TLB-mapped accesses are not attributed.
// The register values seen here are the instruction's inputs, so the effective
// address can be computed before it executes. Stores need nothing: the line's
// dirty mask already records the bytes written.
auto CPU::Profiler::onCacheTouch(u64 address, u32 instruction) -> void {
  u32 pc32 = (u32)address;
  pendingTouchAddr = ~0u;
  pendingExecAddr = ~0u;
  if((pc32 >> 29) == 4) {  //0x80000000-0x9fffffff
    u32 paddr = pc32 & 0x1fff'ffff;
    auto& line = cpu.icache.line(pc32);
    u8 word = 1u << (paddr >> 2 & 7);
    if(line.hit(paddr)) {
      line.executed |= word;
    } else {
      pendingExecAddr = paddr & ~0x1fu;
      pendingExecMask = word;
    }
  }
  //bytes read by each load opcode (0 = not a load; LWL/LWR count as their word)
  static constexpr u8 loadSize[64] = {
    0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,
    0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,
    1,2,4,4, 1,2,4,4, 0,0,0,0, 0,0,0,0,  //LB LH LWL LW LBU LHU LWR LWU
    4,4,0,0, 8,8,0,8, 0,0,0,0, 0,0,0,0,  //LL LWC1 . . LLD LDC1 . LD
  };
  u32 size = loadSize[instruction >> 26];
  if(!size) return;
  u32 vaddr = (u32)(cpu.ipu.r[instruction >> 21 & 31].u64 + (s16)instruction);
  if((vaddr >> 29) != 4) return;
  u32 paddr = (vaddr & 0x1fff'ffff) & ~(size - 1);
  u16 mask = ((1u << size) - 1) << (paddr & 0xf);
  auto& line = cpu.dcache.line(vaddr);
  if(line.hit(paddr)) {
    line.touched |= mask;
  } else {
    pendingTouchAddr = paddr & ~0xfu;
    pendingTouchMask = mask;
  }
}

// Commit only the portion since the last boundary, then rebase the live call.
auto CPU::Profiler::commitFrame(Frame& frame, Frame* parent, u64 time) -> void {
  u64 incl = time > frame.segmentCycle ? time - frame.segmentCycle : 0;
  u64 excl = incl > frame.childCycles ? incl - frame.childCycles : 0;
  // Exception/interrupt frames are never spin/wait; only real functions can be.
  bool spin = false;
  if(!frame.isException) { if(auto sym = resolve(frame.funcAddr)) spin = sym->isSpin; }
  // Wait time within this call's whole subtree: this function's own exclusive
  // time if it is a spin/wait function, plus whatever its callees waited on.
  u64 subtreeWait = frame.childWait + (spin ? excl : 0);
  // Bytes: own = exclusive (this function's code), incl = own + returned callees.
  u64 inclBytesIn  = frame.bytesInOwn  + frame.childBytesIn;
  u64 inclBytesOut = frame.bytesOutOwn + frame.childBytesOut;
  u64 inclCache[CacheKinds];
  for(u32 k : range(CacheKinds)) inclCache[k] = frame.cacheOwn[k] + frame.childCache[k];
  auto add = [&](std::unordered_map<u32, FuncStat>& m) {
    auto& st = m[frame.funcAddr];
    st.addr = frame.funcAddr;
    st.isSpin = spin;
    st.callCount += frame.newCall;
    st.inclCycles += incl;
    st.exclCycles += excl;
    st.waitCycles += subtreeWait;
    st.inclBytesIn  += inclBytesIn;
    st.inclBytesOut += inclBytesOut;
    st.exclBytesIn  += frame.bytesInOwn;
    st.exclBytesOut += frame.bytesOutOwn;
    for(u32 k : range(CacheKinds)) {
      st.inclCacheBytes[k] += inclCache[k];
      st.exclCacheBytes[k] += frame.cacheOwn[k];
    }
  };
  bool hasData = frame.newCall || incl || inclBytesIn || inclBytesOut;
  for(u32 k : range(CacheKinds)) hasData |= inclCache[k] != 0;
  if(hasData) {
    if(frameCount < maxFrames) add(stats);
    add(frameAccum);
    if(markerActive) add(markerAccum);
  }
  if(parent) {
    parent->childCycles += incl;
    parent->childWait += subtreeWait;
    parent->childBytesIn += inclBytesIn;
    parent->childBytesOut += inclBytesOut;
    for(u32 k : range(CacheKinds)) parent->childCache[k] += inclCache[k];
  }
  frame.segmentCycle = time;
  frame.newCall = false;
  frame.childCycles = frame.childWait = 0;
  frame.bytesInOwn = frame.bytesOutOwn = frame.childBytesIn = frame.childBytesOut = 0;
  for(u32 k : range(CacheKinds)) frame.cacheOwn[k] = frame.childCache[k] = 0;
}

auto CPU::Profiler::flushFrames() -> void {
  u64 time = now();
  for(size_t i = callStack.size(); i > 0; i--) {
    commitFrame(callStack[i - 1], i > 1 ? &callStack[i - 2] : nullptr, time);
  }
}

auto CPU::Profiler::popFrame() -> bool {
  if(callStack.back().markerRoot) {
    flushFrames();
    markerStats.swap(markerAccum);
    markerAccum.clear();
    markerActive = false;
    markerReady = true;
  }
  u64 nowT = now();
  commitFrame(callStack.back(), callStack.size() > 1 ? &callStack[callStack.size() - 2] : nullptr, nowT);
  Frame frame = callStack.back();
  callStack.pop_back();
  syncOpenFrames();
  if(frame.isException && excActive) excActive--;
  // Flame-chart span: this call's [entry,now) at its call-stack depth. callStack
  // was just popped, so its current size is exactly this frame's nesting depth.
  // Timestamps are now() (the CPU is primary, so now() is the wall clock the RSP
  // and RDP lanes are mapped onto). Appended to the sliding-window ring; oldest
  // entries are overwritten as the window advances. setEnabled() sizes the ring
  // before any append, so the size check just skips the brief window before that.
  if(recordTimeline.load(std::memory_order_relaxed) && timeline.size() == maxSpans) {
    u64 w = timelineWrite.load(std::memory_order_relaxed);
    timeline[w % maxSpans] = {frame.entryCycle, nowT, frame.funcAddr,
                              (u16)min<u32>(callStack.size(), 0xffff), frame.isException};
    timelineWrite.store(w + 1, std::memory_order_release);
  }
  return frame.isException;
}

// Discontinuous $sp jump (bigger than any single instruction's stack adjustment)
auto CPU::Profiler::switchStack(u32 sp) -> void {
  flushFrames();
  suspended.push_back({spLo, spHi, ++stackClock, std::move(callStack)});
  callStack.clear();  //callStack was moved-from; make it valid+empty

  s32 found = -1;
  for(u32 i = 0; i + 1 < suspended.size(); i++) {  //skip the entry just parked (last)
    auto& s = suspended[i];
    if(sp + regionSlack >= s.lo && sp <= s.hi + regionSlack) { found = (s32)i; break; }
  }
  if(found >= 0) {
    auto& s = suspended[(u32)found];
    callStack = std::move(s.frames);
    // Suspended threads consume no CPU cycles.
    for(auto& f : callStack) f.segmentCycle = now();
    spLo = s.lo; spHi = s.hi;
    suspended.erase(suspended.begin() + found);
  } else {
    spLo = spHi = sp;  //unseen thread: observe its calls fresh
  }
  syncOpenFrames();

  // Bound memory: drop the least-recently-active parked thread's frames.
  while(suspended.size() > maxStacks) {
    u32 oldest = 0;
    for(u32 i = 1; i < suspended.size(); i++) if(suspended[i].used < suspended[oldest].used) oldest = i;
    for(auto& f : suspended[oldest].frames) {
      if(f.markerRoot) { markerActive = false; markerAccum.clear(); }
    }
    suspended.erase(suspended.begin() + oldest);
  }
}

auto CPU::Profiler::exceptionEntryAddr(u32 code) -> u32 {
  if(code != 0) return kExcBase | (0x40 + (code & 0x3f));  //synchronous exception
  // Interrupt: identify the firing CPU interrupt source (and RCP sub-source).
  u32 pend = (u32)(cpu.scc.cause.interruptPending & cpu.scc.status.interruptMask);
  for(u32 bit = 0; bit < 8; bit++) {
    if(!(pend & (1u << bit))) continue;
    if(bit == CPU::Interrupt::RCP) {
      u32 irqs = mi.activeIRQs();
      for(u32 i = 0; i < 6; i++) if(irqs & (1u << i)) return kExcBase | (0x10 + i);
      return kExcBase | 0x20;  //RCP but sub-source already cleared
    }
    return kExcBase | (0x20 + bit);  //non-RCP CPU interrupt (timer / sw / cart)
  }
  return kExcBase | 0x30;  //no specific source found
}

auto CPU::Profiler::labelFor(u32 addr) -> string {
  if(!isExceptionAddr(addr)) {
    if(auto sym = resolve(addr)) return sym->name;
    return string{"0x", hex(addr, 8L)};
  }
  u32 id = addr & 0xffff;
  if(id >= 0x10 && id < 0x16) {
    static const char* irqNames[6] = {"SP", "SI", "AI", "VI", "PI", "DP"};
    return string{"[IRQ: ", irqNames[id - 0x10], "]"};
  }
  if(id >= 0x20 && id < 0x28) {
    static const char* cpuIrq[8] = {"SW0", "SW1", "RCP", "cart", "reset", "RDB", "RDB", "timer"};
    return string{"[IRQ: ", cpuIrq[id - 0x20], "]"};
  }
  if(id >= 0x40 && id < 0x80) return string{"[exception ", id - 0x40, "]"};
  return string{"[interrupt]"};
}

auto CPU::Profiler::onException(u32 code) -> void {
  // Push a synthetic frame for the handler so its time nests under (and is thus
  // subtracted from) the interrupted function, and shows as its own entry.
  if(callStack.size() >= maxStackDepth) return;
  Frame f;
  f.funcAddr = exceptionEntryAddr(code);
  f.entryCycle = f.segmentCycle = now();
  f.isException = true;
  callStack.push_back(f);
  syncOpenFrames();
  excActive++;  //freeze stack-switching until the matching eret (see onInstruction)
}

auto CPU::Profiler::onEret() -> void {
  // Unwind back to and including the most recent exception frame. (Normally the
  // handler has balanced its own calls, so that frame is already on top.)
  bool hasExc = false;
  for(auto& f : callStack) if(f.isException) { hasExc = true; break; }
  if(!hasExc) return;
  while(!callStack.empty()) {
    if(popFrame()) break;  //stop after the exception frame is popped
  }
}

// Publish the in-progress frame as the "last completed frame" snapshot. Called
// on each presented framebuffer swap.
auto CPU::Profiler::onFrame() -> void {
  if(enabled.load(std::memory_order_relaxed)) {
    flushFrames();
    frameStats.swap(frameAccum);
    if(frameCount < maxFrames) {
      for(auto& [addr, frame] : frameStats) {
        auto& total = swapTotals[addr];
        total.addr = addr;
        total.isSpin = frame.isSpin;
        total.callCount += frame.callCount;
        total.inclCycles += frame.inclCycles;
        total.exclCycles += frame.exclCycles;
        total.waitCycles += frame.waitCycles;
        total.inclBytesIn += frame.inclBytesIn;
        total.inclBytesOut += frame.inclBytesOut;
        total.exclBytesIn += frame.exclBytesIn;
        total.exclBytesOut += frame.exclBytesOut;
        for(u32 k : range(CacheKinds)) {
          total.inclCacheBytes[k] += frame.inclCacheBytes[k];
          total.exclCacheBytes[k] += frame.exclCacheBytes[k];
        }
      }
      frameCount++;
    }
  }
  frameAccum.clear();

  {
    u64 w = viMarkWrite.load(std::memory_order_relaxed);
    viMarks[w % maxViMarks] = now();
    viMarkWrite.store(w + 1, std::memory_order_release);
  }
}

auto CPU::Profiler::setEnabled(bool value) -> void {
  if(value == enabled.load(std::memory_order_relaxed)) return;
  if(!value) flushFrames();
  else refreshWaitFunctions();
  enabled.store(value, std::memory_order_relaxed);
  markerActive = false;
  markerAccum.clear();
  callStack.clear();
  suspended.clear();
  haveSp = false; excActive = 0; spLo = spHi = 0; stackClock = 0;
  syncOpenFrames();
  if(value && timeline.size() != maxSpans) timeline.resize(maxSpans);
  if(value && cacheEvents.size() != maxCacheEvents) cacheEvents.resize(maxCacheEvents);
  if(value && lineHistory[0].empty()) {
    lineHistory[0].resize(0x80'0000 >> 5);  //8 MB RDRAM of 32-byte icache lines
    lineHistory[1].resize(0x80'0000 >> 4);  //... and 16-byte dcache lines
  }
  cpu.updatePrologueHook();
}

// Machine reset / new game. now() is derived from cpu.profile.cpuCycles, which is
// not reset by CPU::power(), so timestamps keep climbing across a reset and stale
// spans would never scroll out of the flame chart's window on their own. Worse,
// frames left on callStack when the CPU was reset can never be popped, so their
// "ongoing" bars would stay pinned to the right edge forever. Symbols are keyed to
// the ROM and reloaded separately on game load, so they survive.
auto CPU::Profiler::power() -> void {
  callStack.clear();
  suspended.clear();
  haveSp = false; excActive = 0; spLo = spHi = 0; stackClock = 0;
  clearStats();
  viMarkWrite.store(0, std::memory_order_release);
  cacheEventWrite.store(0, std::memory_order_release);
  for(auto& h : lineHistory) std::fill(h.begin(), h.end(), LineHistory{});
  fillSeq[0] = fillSeq[1] = 0;
}

auto CPU::Profiler::clearStats() -> void {
  stats.clear();
  swapTotals.clear();
  frameStats.clear();
  frameAccum.clear();
  markerStats.clear();
  markerAccum.clear();
  markerActive = false;
  markerReady = false;
  auto reset = [&](auto& frames) {
    for(auto& f : frames) {
      Frame clean;
      clean.funcAddr = f.funcAddr;
      clean.retAddr = f.retAddr;
      clean.sp = f.sp;
      clean.entryCycle = clean.segmentCycle = now();
      clean.isException = f.isException;
      clean.newCall = false;
      f = clean;
    }
  };
  reset(callStack);
  for(auto& s : suspended) reset(s.frames);
  syncOpenFrames();
  frameCount = 0;
  timelineWrite.store(0, std::memory_order_release);
}

auto CPU::profileCacheFill(u8 kind, u32 address) -> u64 {
#if ARES_DEBUG_TOOLS
  if(kind == Profiler::CacheIFill && unlikely(execTrace.active())) execTrace.onIcacheFill(address);
  if(!profiler.enabled.load(std::memory_order_relaxed)) return 0;
  return profiler.onCacheFill(kind, address);
#else
  return 0;
#endif
}

auto CPU::profileCacheContent(u64 eventId, const u32* words, u32 count) -> void {
#if ARES_DEBUG_TOOLS
  if(!eventId || !profiler.enabled.load(std::memory_order_relaxed)) return;
  profiler.onCacheContent(eventId, words, count);
#endif
}

auto CPU::profileCacheEvict(u64 eventId, u16 readMask, u16 writeMask) -> void {
#if ARES_DEBUG_TOOLS
  if(!eventId || !profiler.enabled.load(std::memory_order_relaxed)) return;
  profiler.onCacheEvict(eventId, readMask, writeMask);
#endif
}

auto CPU::updatePrologueHook() -> void {
#if ARES_DEBUG_TOOLS
  if constexpr(Accuracy::CPU::Recompiler) {
    bool want = debugger.tracer.instruction->enabled() || profiler.enabled.load(std::memory_order_relaxed)
             || execTrace.active();
    if(recompiler.callInstructionPrologue != want) {
      recompiler.callInstructionPrologue = want;
      recompiler.reset();
    }
  }
#endif
}
