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
  // by 2 and add the still-pending master clocks. This may step backwards by at
  // most 1 master clock at a sync boundary (the truncated low bit); callers
  // guard their subtractions against that.
  return (u64)(cpu.profile.cpuCycles * 2 + cpu.clock);
}

auto CPU::Profiler::loadSymbols(const string& romPath) -> bool {
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

auto CPU::Profiler::onInstruction(u64 address, u32 instruction) -> void {
  u32 op = instruction >> 26;
  bool isJAL  = (op == 0x03);
  bool isJALR = (op == 0x00) && ((instruction & 0x3f) == 0x09);
  bool isJR   = (op == 0x00) && ((instruction & 0x3f) == 0x08);

  if(isJAL || isJALR) {
    if(callStack.size() >= maxStackDepth) return;  //runaway guard
    u32 pc32 = (u32)address;
    u32 target = isJAL
      ? ((pc32 & 0xf000'0000) | ((instruction & 0x03ff'ffff) << 2))
      : (u32)cpu.ipu.r[(instruction >> 21) & 31].u64;  //JALR rs holds the target
    callStack.push_back({target, now(), 0});
    return;
  }

  if(isJR) {
    if(((instruction >> 21) & 31) != 31) return;  //only "jr $ra" is a return
    if(callStack.empty()) return;                 //underflow guard
    if(callStack.back().isException) return;       //a normal return never crosses
                                                   //an exception boundary (ERET does)
    popFrame();
  }
}

// Record the top frame's timing into the stats and propagate its inclusive time
// and subtree-wait into its caller (which subtracts it from the caller's
// exclusive). Returns whether the popped frame was a synthetic exception frame.
auto CPU::Profiler::popFrame() -> bool {
  Frame frame = callStack.back();
  callStack.pop_back();
  u64 nowT = now();
  u64 incl = nowT > frame.entryCycle ? nowT - frame.entryCycle : 0;  //guard sync-boundary -1
  u64 excl = incl > frame.childCycles ? incl - frame.childCycles : 0;
  // Exception/interrupt frames are never spin/wait; only real functions can be.
  bool spin = false;
  if(!frame.isException) { if(auto sym = resolve(frame.funcAddr)) spin = sym->isSpin; }
  // Wait time within this call's whole subtree: this function's own exclusive
  // time if it is a spin/wait function, plus whatever its callees waited on.
  u64 subtreeWait = frame.childWait + (spin ? excl : 0);
  auto add = [&](std::unordered_map<u32, FuncStat>& m) {
    auto& st = m[frame.funcAddr];
    st.addr = frame.funcAddr;
    st.isSpin = spin;
    st.callCount++;
    st.inclCycles += incl;
    st.exclCycles += excl;
    st.waitCycles += subtreeWait;
  };
  if(frameCount < maxFrames) add(stats);  //freeze continuous totals at the cap
  add(frameAccum);                         //per-frame snapshot is always live
  if(!callStack.empty()) {
    callStack.back().childCycles += incl;
    callStack.back().childWait   += subtreeWait;
  }
  return frame.isException;
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
  f.entryCycle = now();
  f.isException = true;
  callStack.push_back(f);
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
  frameStats = frameAccum;
  frameAccum.clear();
  //number of presented frames the continuous totals span, capped at maxFrames so
  //the accumulation stops growing once the window is full (Clear restarts it).
  if(frameCount < maxFrames) frameCount++;
}

auto CPU::Profiler::setEnabled(bool value) -> void {
  enabled.store(value, std::memory_order_relaxed);
  callStack.clear();
  cpu.updatePrologueHook();
}

auto CPU::Profiler::clearStats() -> void {
  stats.clear();
  frameStats.clear();
  frameAccum.clear();
  callStack.clear();
  frameCount = 0;
}

auto CPU::updatePrologueHook() -> void {
  if constexpr(Accuracy::CPU::Recompiler) {
    bool want = debugger.tracer.instruction->enabled() || profiler.enabled.load(std::memory_order_relaxed);
    if(recompiler.callInstructionPrologue != want) {
      recompiler.callInstructionPrologue = want;
      recompiler.reset();
    }
  }
}
