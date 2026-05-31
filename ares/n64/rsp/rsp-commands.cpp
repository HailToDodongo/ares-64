// RSP command viewer — JSON config loading (nlohmann), ELF auto-detection, and capture

namespace {

struct Elf64Header {
  u8  ident[16];
  u16 type;
  u16 machine;
  u32 version;
  u64 entry;
  u64 phoff;
  u64 shoff;
  u32 flags;
  u16 ehsize;
  u16 phentsize;
  u16 phnum;
  u16 shentsize;
  u16 shnum;
  u16 shstrndx;
};

struct Elf64Shdr {
  u32 name;
  u32 type;
  u64 flags;
  u64 addr;
  u64 offset;
  u64 size;
  u32 link;
  u32 info;
  u64 addralign;
  u64 entsize;
};

struct Elf64Sym {
  u32 name;
  u8  info;
  u8  other;
  u16 shndx;
  u64 value;
  u64 size;
};

static auto elfRead64BE(const u8* data, u32 offset) -> u64 {
  u64 val = 0;
  for(int i = 0; i < 8; i++) val = (val << 8) | data[offset + i];
  return val;
}

static auto elfRead32BE(const u8* data, u32 offset) -> u32 {
  return (u32(data[offset]) << 24) | (u32(data[offset+1]) << 16)
       | (u32(data[offset+2]) << 8) | u32(data[offset+3]);
}

static auto elfRead16BE(const u8* data, u32 offset) -> u16 {
  return (u16(data[offset]) << 8) | u16(data[offset+1]);
}

// Try to read a symbol's value from a pre-loaded ELF data buffer.
// Handles both ELF32 and ELF64 big-endian.
static auto elfTryReadSymbolFrom(const u8* data, u32 size, const string& symName, u64& value) -> bool {
  if(!data || size < 64) return false;

  // Validate ELF magic
  if(data[0] != 0x7f || data[1] != 'E' || data[2] != 'L' || data[3] != 'F') return false;
  u8 elfClass = data[4];  // 1=ELF32, 2=ELF64
  u8 elfData2 = data[5];  // 1=LE, 2=BE
  if(elfData2 != 2) return false; // only big-endian
  if(elfClass != 1 && elfClass != 2) return false;

  bool is64 = (elfClass == 2);
  
  // Read section header info (different layout for ELF32 vs ELF64)
  u64 shoff;
  u32 shentsize, shnum, shstrndx;
  u32 shdrSize;        // size of one section header
  u32 shOffOff, shSizeOff, shAddrOff; // offsets within section header

  if(is64) {
    shoff     = elfRead64BE(data, 40);
    shentsize = elfRead16BE(data, 58);
    shnum     = elfRead16BE(data, 60);
    shstrndx  = elfRead16BE(data, 62);
    shdrSize  = 64;
    shOffOff  = 24;  // sh_offset at +24
    shSizeOff = 32;  // sh_size at +32
    shAddrOff = 16;  // sh_addr at +16
  } else {
    shoff     = elfRead32BE(data, 32);
    shentsize = elfRead16BE(data, 46);
    shnum     = elfRead16BE(data, 48);
    shstrndx  = elfRead16BE(data, 50);
    shdrSize  = 40;
    shOffOff  = 16;  // sh_offset at +16
    shSizeOff = 20;  // sh_size at +20
    shAddrOff = 12;  // sh_addr at +12
  }

  if(shoff == 0 || shnum == 0) return false;

  // Find shstrtab base (for section name lookup)
  u64 shstrBase = 0;
  {
    u32 shstrHdrOff = shoff + shstrndx * shentsize;
    if(shstrHdrOff + shdrSize <= (u64)size) {
      shstrBase = is64 ? elfRead64BE(data, shstrHdrOff + shOffOff)
                       : elfRead32BE(data, shstrHdrOff + shOffOff);
    }
  }

  // Find .strtab and .symtab sections
  u64 strtabOffset = 0, strtabSize = 0;
  u64 symtabOffset = 0, symtabSize = 0, symtabEntsize = 0;

  for(u32 i = 0; i < shnum; i++) {
    u32 shdrOff = shoff + i * shentsize;
    if(shdrOff + shdrSize > size) break;
    u32 nameIdx = elfRead32BE(data, shdrOff);
    u32 type    = elfRead32BE(data, shdrOff + 4);
    u64 secOff  = is64 ? elfRead64BE(data, shdrOff + shOffOff)
                       : elfRead32BE(data, shdrOff + shOffOff);
    u64 secSize = is64 ? elfRead64BE(data, shdrOff + shSizeOff)
                       : elfRead32BE(data, shdrOff + shSizeOff);
    u64 esize   = is64 ? elfRead64BE(data, shdrOff + 56)
                       : elfRead32BE(data, shdrOff + 36);

    // Get section name
    if(shstrBase && shstrBase + nameIdx < (u64)size) {
      const char* secName = (const char*)(data + shstrBase + nameIdx);
      nall::string sname{secName};

      if(type == 3 && sname == ".strtab") { // SHT_STRTAB
        strtabOffset = secOff;
        strtabSize = secSize;
      } else if(type == 2) { // SHT_SYMTAB
        symtabOffset = secOff;
        symtabSize = secSize;
        symtabEntsize = esize;
      }
    }
  }

  if(!symtabOffset || !strtabOffset || !symtabEntsize) return false;

  // Search symbol table
  u32 symValueOff = is64 ? 8 : 4;

  for(u64 s = 0; s < symtabSize; s += symtabEntsize) {
    u32 symOff = symtabOffset + s;
    if(symOff + symtabEntsize > (u64)size) break;
    u32 nameIdx = elfRead32BE(data, symOff);
    u64 symVal  = is64 ? elfRead64BE(data, symOff + symValueOff)
                       : elfRead32BE(data, symOff + symValueOff);

    if(strtabOffset + nameIdx >= (u64)size) continue;
    const char* name = (const char*)(data + strtabOffset + nameIdx);
    if(nall::string{name} == symName) {
      value = symVal;
      return true;
    }
  }

  return false;
}

// Parse one command's optional "args" + "fmt" into a descriptor. JSON shape:
//   "0x02": { "name": "Matrix Stack", "fmt": "{mul?mul:set} advance {advance}", "args": [
//     { "name":"mul", "word":0, "bits":[0,0], "type":"bool" },
//     { "name":"advance", "word":0, "bits":[8,23], "signed":true },
//     { "name":"matrix", "word":1, "type":"addr" }
//   ]}
// Supported per-field keys: word, bits[hi,lo], signed, mul, div, add, enum{value:label}.
// A "type" shortcut sets bits/format conventions (bool, addr, hex, rgba, uint, int).
static auto parseArgsJson(const nlohmann::json& cmdData, RSPCapture::CmdArgInfo& out) -> void {
  if(cmdData.contains("fmt")) out.fmt = cmdData["fmt"].get<std::string>().c_str();
  if(!cmdData.contains("args") || !cmdData["args"].is_array()) return;

  for(auto& a : cmdData["args"]) {
    RSPCapture::ArgField f;
    f.name = a.value("name", std::string{}).c_str();
    f.word = (u8)a.value("word", 0);
    // Default bit range: word 0 carries the command id in its top byte, so its
    // argument payload is bits [0..23]; other words use the whole 32 bits.
    f.loBit = 0;
    f.hiBit = f.word == 0 ? 23 : 31;
    if(a.contains("bits") && a["bits"].is_array() && a["bits"].size() == 2) {
      f.hiBit = (u8)a["bits"][0].get<int>();
      f.loBit = (u8)a["bits"][1].get<int>();
    }
    f.isSigned = a.value("signed", false);
    f.mul = a.value("mul", (s64)1);
    f.div = a.value("div", (s64)1);
    f.add = a.value("add", (s64)0);
    if(f.div == 0) f.div = 1;
    // Fixed-point display: "fdiv" divides, "fmul"/"fscale" multiplies, to render
    // the integer value as a float. Any of them flips the field into float mode.
    if(a.contains("fdiv"))   f.fscale = 1.0 / a["fdiv"].get<double>();
    if(a.contains("fmul"))   f.fscale = (f.fscale != 0.0 ? f.fscale : 1.0) * a["fmul"].get<double>();
    if(a.contains("fscale")) f.fscale = a["fscale"].get<double>();
    if(a.contains("enum") && a["enum"].is_object()) {
      for(auto& [k, v] : a["enum"].items()) {
        f.enums.push_back({(s64)std::strtoll(k.c_str(), nullptr, 0), v.get<std::string>().c_str()});
      }
    }
    out.fields.push_back(f);
  }
}

  // Collect all rsp_* symbols from the ELF whose value falls in the KSEG0
  // range (0x80000000–0x81000000) — these are rsp_ucode_t struct pointers.
  struct RspElfSymbol { string name; u64 value; };
  static auto elfCollectRspSymbols(const u8* data, u32 size) -> std::vector<RspElfSymbol> {
    std::vector<RspElfSymbol> out;
    if(!data || size < 64) return out;

    // Validate ELF magic
    if(data[0] != 0x7f || data[1] != 'E' || data[2] != 'L' || data[3] != 'F') return out;
    u8 elfClass = data[4];
    u8 elfData2 = data[5];
    if(elfData2 != 2) return out;
    if(elfClass != 1 && elfClass != 2) return out;
    bool is64 = (elfClass == 2);

    u64 shoff; u32 shentsize, shnum, shstrndx;
    u32 shdrSize, shOffOff, shSizeOff, shAddrOff;
    if(is64) {
      shoff     = elfRead64BE(data, 40); shentsize = elfRead16BE(data, 58);
      shnum     = elfRead16BE(data, 60); shstrndx  = elfRead16BE(data, 62);
      shdrSize  = 64; shOffOff = 24; shSizeOff = 32; shAddrOff = 16;
    } else {
      shoff     = elfRead32BE(data, 32); shentsize = elfRead16BE(data, 46);
      shnum     = elfRead16BE(data, 48); shstrndx  = elfRead16BE(data, 50);
      shdrSize  = 40; shOffOff = 16; shSizeOff = 20; shAddrOff = 12;
    }
    if(shoff == 0 || shnum == 0) return out;

    u64 shstrBase = 0;
    { u32 hdrOff = shoff + shstrndx * shentsize;
      if(hdrOff + shdrSize <= (u64)size)
        shstrBase = is64 ? elfRead64BE(data, hdrOff + shOffOff)
                         : elfRead32BE(data, hdrOff + shOffOff); }

    u64 strtabOff = 0, strtabSz = 0, symtabOff = 0, symtabSz = 0, symEnt = 0;
    for(u32 i = 0; i < shnum; i++) {
      u32 hdrOff = shoff + i * shentsize;
      if(hdrOff + shdrSize > size) break;
      u32 nameIdx = elfRead32BE(data, hdrOff);
      u32 type = elfRead32BE(data, hdrOff + 4);
      u64 secOff = is64 ? elfRead64BE(data, hdrOff + shOffOff)
                        : elfRead32BE(data, hdrOff + shOffOff);
      u64 secSz  = is64 ? elfRead64BE(data, hdrOff + shSizeOff)
                        : elfRead32BE(data, hdrOff + shSizeOff);
      u64 esize  = is64 ? elfRead64BE(data, hdrOff + 56)
                        : elfRead32BE(data, hdrOff + 36);
      if(shstrBase && shstrBase + nameIdx < (u64)size) {
        nall::string sname{(const char*)(data + shstrBase + nameIdx)};
        if(type == 3 && sname == ".strtab") { strtabOff = secOff; strtabSz = secSz; }
        else if(type == 2) { symtabOff = secOff; symtabSz = secSz; symEnt = esize; }
      }
    }
    if(!symtabOff || !strtabOff || !symEnt) return out;

    u32 symValOff = is64 ? 8 : 4;
    for(u64 s = 0; s < symtabSz; s += symEnt) {
      u32 symOff = symtabOff + s;
      if(symOff + symEnt > (u64)size) break;
      u32 nameIdx = elfRead32BE(data, symOff);
      u64 symVal  = is64 ? elfRead64BE(data, symOff + symValOff)
                         : elfRead32BE(data, symOff + symValOff);
      if(strtabOff + nameIdx >= (u64)size) continue;
      const char* name = (const char*)(data + strtabOff + nameIdx);
      nall::string sname{name};
      // Only collect symbols starting with "rsp_" whose value is in KSEG0.
      if(!sname.beginsWith("rsp_")) continue;
      if(symVal < 0x80000000 || symVal >= 0x81000000) continue;
      out.push_back({sname, symVal});
    }
    return out;
  }

} // anonymous namespace

auto RSPCapture::loadConfig(const string& jsonPath) -> bool {
  string data = string::read(jsonPath);
  if(!data) return false;

  try {
    auto j = nlohmann::json::parse(data.data());

    // Reset state
    hookCount = 0;
    pcLoop = pcExecCommand = pcLoadOverlay = pcFetchBuffer = pcFetchBufferPtr = ~0u;
    dmemCmdsOffset = 0;
    dmemCmdsSize = 0;
    dmemDramAddrOffset = 0;
    dmemOvlTableOffset = 0;
    dmemOvlIdmapOffset = 0;
    banner = {};

    if(j.contains("banner")) banner = j["banner"].get<std::string>().c_str();

    // Parse IMEM hook addresses
    if(j.contains("imem")) {
      for(auto& [key, val] : j["imem"].items()) {
        if(hookCount >= maxHookAddresses) break;
        auto hexStr = val.get<std::string>();
        u32 addr = 0;
        for(char c : hexStr) {
          if(c == 'x' || c == 'X') { addr = 0; continue; }
          u32 nib = 0;
          if(c >= '0' && c <= '9') nib = c - '0';
          else if(c >= 'a' && c <= 'f') nib = c - 'a' + 10;
          else if(c >= 'A' && c <= 'F') nib = c - 'A' + 10;
          else break;
          addr = (addr << 4) | nib;
        }
        hookAddresses[hookCount++] = addr;

        // Resolve the well-known dispatch points by key name so the capture
        // logic can tell them apart (see captureCommandHook).
        nall::string k{key.c_str()};
        if(k == "RSPQ_Loop")                  pcLoop = addr;
        else if(k == "rspq_execute_command")  pcExecCommand = addr;
        else if(k == "rspq_load_overlay")     pcLoadOverlay = addr;
        else if(k == "rspq_fetch_buffer")     pcFetchBuffer = addr;
        else if(k == "rspq_fetch_buffer_with_ptr") pcFetchBufferPtr = addr;
      }
    }

    // Parse DMEM layout
    if(j.contains("dmem")) {
      auto& d = j["dmem"];
      if(d.contains("cmds_offset"))      dmemCmdsOffset     = (u32)std::stoul(d["cmds_offset"].get<std::string>(), nullptr, 16);
      if(d.contains("cmds_size"))        dmemCmdsSize       = (u32)std::stoul(d["cmds_size"].get<std::string>(), nullptr, 16);
      if(d.contains("dram_addr_offset")) dmemDramAddrOffset = (u32)std::stoul(d["dram_addr_offset"].get<std::string>(), nullptr, 16);
      if(d.contains("ovl_table_offset")) dmemOvlTableOffset = (u32)std::stoul(d["ovl_table_offset"].get<std::string>(), nullptr, 16);
      if(d.contains("ovl_idmap_offset")) dmemOvlIdmapOffset = (u32)std::stoul(d["ovl_idmap_offset"].get<std::string>(), nullptr, 16);
    }

    // Parse overlay command names (array format: [{"id": N, "name": "...", "commands": {...}}])
    jsonOvlDataCount = 0;
    if(j.contains("overlays") && j["overlays"].is_array()) {
      for(auto& ovl : j["overlays"]) {
        u32 ovlId = ovl.value("id", ~0u);
        auto ovlName = ovl.value("name", std::string{});
        // Store by name for runtime matching
        if(!ovlName.empty() && jsonOvlDataCount < 16) {
          auto& jd = jsonOvlData[jsonOvlDataCount++];
          jd.name = ovlName.c_str();
          if(ovl.contains("commands")) {
            for(auto& [cmdKey, cmdData] : ovl["commands"].items()) {
              u32 cmdId = (u32)std::stoul(cmdKey, nullptr, 16);
              if(cmdId >= 256) continue;
              if(cmdData.contains("name")) {
                jd.commandNames[cmdId] = cmdData["name"].get<std::string>().c_str();
              }
              parseArgsJson(cmdData, jd.commandArgs[cmdId]);
            }
          }
        }
        // Also store by id for overlays with known fixed ids
        if(ovlId < 16) {
          if(!ovlName.empty()) overlayNameMap[ovlId] = ovlName.c_str();
          if(ovl.contains("commands")) {
            for(auto& [cmdKey, cmdData] : ovl["commands"].items()) {
              u32 cmdId = (u32)std::stoul(cmdKey, nullptr, 16);
              if(cmdId >= 256) continue;
              if(cmdData.contains("name")) {
                commandNameMap[ovlId][cmdId] = cmdData["name"].get<std::string>().c_str();
              }
              parseArgsJson(cmdData, cmdArgs[ovlId][cmdId]);
            }
          }
        }
      }
    }

  } catch(const nlohmann::json::exception& e) {
    print("RSP: loadConfig: JSON parse error: ", e.what(), "\n");
    return false;
  }

  configLoaded = (hookCount > 0);
  return configLoaded;
}

// Helper: find last occurrence of a character in a nall string
static auto stringFindLast(const nall::string& s, char c) -> s32 {
  for(s32 i = s32(s.size()) - 1; i >= 0; i--) {
    if(s[i] == c) return i;
  }
  return -1;
}

// Helper: extract substring from after last '/' to the end
static auto stringBasename(const nall::string& s) -> nall::string {
  s32 slash = stringFindLast(s, '/');
  nall::string name;
  for(s32 i = slash + 1; i < (s32)s.size(); i++) name.append(s[i]);
  return name;
}

// Helper: extract substring from start to last '/'
static auto stringDirname(const nall::string& s) -> nall::string {
  s32 slash = stringFindLast(s, '/');
  nall::string dir;
  for(s32 i = 0; i < slash; i++) dir.append(s[i]);
  return dir;
}

// Try to auto-detect RSPQ config from an ELF file and built-in defaults.
// Returns true if detection was successful.
auto RSPCapture::autoDetect(const string& romPath) -> bool {
  // Derive ELF path: strip extension, look for .elf in various locations
  string base = romPath;
  s32 dot = stringFindLast(base, '.');
  if(dot >= 0) { base.resize(dot); }

  // Try several ELF paths relative to the ROM
  string foundElfPath;

  // 1) Direct replacement: rom.z64 -> rom.elf
  string directPath = {base, ".elf"};
  if(string::read(directPath)) { foundElfPath = directPath; }

  // 2) build/ subdirectory of the ROM's own directory
  if(!foundElfPath) {
    string buildPath = {base, "/build/", stringBasename(base), ".elf"};
    if(string::read(buildPath)) { foundElfPath = buildPath; }
  }

  // 3) Parent dir's build/ subdirectory (e.g., examples/00_quad/build/name.elf)
  if(!foundElfPath) {
    string parentPath = {stringDirname(base), "/build/", stringBasename(base), ".elf"};
    if(string::read(parentPath)) { foundElfPath = parentPath; }
  }

  if(!foundElfPath) return false;
  elfPath = foundElfPath;

  // Cache the entire ELF in memory so subsequent symbol lookups don't hit disk.
  cachedElfData = string::read(elfPath);
  const u8* elfData = (const u8*)cachedElfData.data();
  u32 elfDataSize = cachedElfData ? cachedElfData.size() : 0;

  // rsp_queue_text_start is in KSEG0 (0x80000000). The RSPQ code is at offset 0
  // within IMEM when loaded via rsp_load().
  // The DMEM layout is standard per rsp_queue_t.

  // Read rspq_overlay_ucodes address from ELF for runtime overlay name detection
  u64 ovlUcodesVal = 0;
  elfTryReadSymbolFrom(elfData, elfDataSize, "rspq_overlay_ucodes", ovlUcodesVal);
  ovlUcodesAddr = ovlUcodesVal;

  // Try to load JSON config for hooks, DMEM layout, and overlay names
  bool jsonLoaded = false;
  {
    string jsonPaths[] = {
      {stringDirname(elfPath), "/rspq-libdragon.json"},
      {Path::program(), "rspq-libdragon.json"},
      {stringDirname(Path::program()), "rspq-libdragon.json"},
    };
    for(auto& jp : jsonPaths) {
      if(loadConfig(jp)) { jsonLoaded = true; break; }
    }
  }

  if(!jsonLoaded) {
    print("RSP: autoDetect: ELF found but JSON config not loaded\n");
    return false;
  }

  configLoaded = true;
  enabled.store(true, std::memory_order_relaxed);
  return true;
}

auto RSPCapture::refreshOverlayNames() -> void {
  // Called every frame to pick up runtime overlay registrations.
  if(elfPath && cachedElfData) {
    const u8* elfData = (const u8*)cachedElfData.data();
    u32 elfDataSize = cachedElfData.size();
    auto& r = ares::Nintendo64::rsp;
    auto rdPhys = [](u32 phys) -> u32 {
      return (u32)Nintendo64::rdram.ram.read<Word>(phys, RBusDevice::ARES_DEBUGGER);
    };
    // Read rspq_data_size from ELF: DMEM offset to add to overlay data pointers
    u64 rspqDataSizeVal = 0;
    if(!elfTryReadSymbolFrom(elfData, elfDataSize, "rsp_queue_data_size", rspqDataSizeVal)) return;
    u32 rspqDataSize = (u32)rspqDataSizeVal;
    // For each non-zero DMEM overlay entry, find matching ELF symbol
    for(u32 slot = 0; slot < 16; slot++) {
      u32 entry = r.dmem.read<Word>(dmemOvlTableOffset + slot * 4);
      u32 dataPhys = entry & 0x00FFFFFF;
      if(!dataPhys) continue;
      // Collect all rsp_* symbols from the cached ELF — no hardcoded list.
      static std::vector<RspElfSymbol> rspSymbols;
      static bool symbolsBuilt = false;
      if(!symbolsBuilt) { rspSymbols = elfCollectRspSymbols(elfData, elfDataSize); symbolsBuilt = true; }
      for(auto& sym : rspSymbols) {
        u64 addr = sym.value;
        u32 physBase = (u32)addr - 0x80000000u;
        u32 dataPtr = rdPhys(physBase + 8);
        u32 namePtr = rdPhys(physBase + 24);
        if(!dataPtr || !namePtr) continue;
        u32 elfDataPhys = (dataPtr + rspqDataSize) - 0x80000000u;
        if(elfDataPhys != dataPhys) continue;
        // Match! Read name
        u32 namePhys = namePtr - 0x80000000u;
        char buf[64] = {};
        for(u32 j = 0; j < 63; j++) {
          buf[j] = (char)Nintendo64::rdram.ram.read<Byte>(namePhys + j, RBusDevice::ARES_DEBUGGER);
          if(!buf[j]) break;
        }
        // Strip "rsp_" prefix and resolve
        nall::string finalName{buf};
        if(finalName.beginsWith("rsp_")) {
          nall::string stripped;
          for(u32 c = 4; c < finalName.size(); c++) stripped.append(finalName[c]);
          finalName = stripped;
        }
        // Read idmap to determine which command block this slot covers
        // idmap[slot] = base_id << 2; command offset = (slot - base_id) * 16
        u8 idmapEntry = r.dmem.read<Byte>(dmemOvlIdmapOffset + slot);
        u32 baseId = idmapEntry >> 2;
        u32 cmdOffset = (slot - baseId) * 16;

        if(!overlayNameMap[slot]) {
          print("RSP: overlay ", slot, " = '", finalName, "'\n");
        }
        overlayNameMap[slot] = finalName;
        // Populate command names + arg descriptors from JSON, shifted by cmdOffset
        for(u32 j = 0; j < jsonOvlDataCount; j++) {
          if(jsonOvlData[j].name == finalName) {
            for(u32 c = 0; c < 256; c++) {
              if(c >= cmdOffset && jsonOvlData[j].commandNames[c]) {
                commandNameMap[slot][c - cmdOffset] = jsonOvlData[j].commandNames[c];
              }
              if(c >= cmdOffset && jsonOvlData[j].commandArgs[c].valid()) {
                cmdArgs[slot][c - cmdOffset] = jsonOvlData[j].commandArgs[c];
              }
            }
            break;
          }
        }
        break;
      }
    }
  }
}

auto RSPCapture::detectRspq() -> bool {
  if(!configLoaded) return false;
  return true;
}

// Extract a single field's value from the command words: mask the bit range,
// optionally sign-extend, then apply the mul/div/add transform.
static auto rspExtractField(const RSPCapture::ArgField& f, const u32* words, u8 wc) -> s64 {
  u32 raw = (f.word < wc) ? words[f.word] : 0u;
  u32 width = (u32)f.hiBit - (u32)f.loBit + 1;
  if(width == 0 || width > 32) width = 32;
  u32 mask = (width >= 32) ? 0xFFFFFFFFu : ((1u << width) - 1u);
  u32 bits = (raw >> f.loBit) & mask;
  s64 v = (s64)bits;
  if(f.isSigned && width < 32 && (bits & (1u << (width - 1)))) {
    v = (s64)bits - ((s64)1 << width);
  } else if(f.isSigned && width == 32) {
    v = (s32)bits;
  }
  return v * f.mul / f.div + f.add;
}

auto RSPCapture::formatArgs(u16 overlayId, u8 commandId, const u32* words, u8 wordCount) const -> string {
  if(overlayId >= 16 || commandId >= 256) return {};
  auto& info = cmdArgs[overlayId][commandId];
  if(!info.valid()) return {};

  auto valueText = [&](const ArgField& f, char spec) -> std::string {
    s64 v = rspExtractField(f, words, wordCount);
    char buf[32];
    if(spec == 'x') {
      snprintf(buf, sizeof(buf), "0x%llX", (unsigned long long)(v & 0xFFFFFFFFull));
      return buf;
    }
    // Fixed-point/float display: "{name:f}" forces it, or a field with fscale set
    // defaults to it. Trailing zeros trimmed via %g.
    if(spec == 'f' || (spec == 0 && f.fscale != 0.0)) {
      double fv = (double)v * (f.fscale != 0.0 ? f.fscale : 1.0);
      snprintf(buf, sizeof(buf), "%.4g", fv);
      return buf;
    }
    if(spec != 'd') {  // allow an enum label unless decimal was explicitly requested
      for(auto& e : f.enums) if(e.value == v) return std::string(e.label.data());
    }
    snprintf(buf, sizeof(buf), "%lld", (long long)v);
    return buf;
  };

  std::string out;

  // No template: auto-list the fields as "name=value".
  if(!info.fmt) {
    for(auto& f : info.fields) {
      if(!out.empty()) out += ' ';
      out += std::string(f.name.data()) + "=" + valueText(f, 0);
    }
    return string{out.c_str()};
  }

  // Template: substitute {name}, {name:x} (hex), {name:d} (force decimal).
  std::string t(info.fmt.data());
  for(size_t i = 0; i < t.size();) {
    if(t[i] != '{') { out += t[i++]; continue; }
    size_t j = t.find('}', i);
    if(j == std::string::npos) { out += t[i++]; continue; }
    std::string tok = t.substr(i + 1, j - i - 1);
    i = j + 1;
    std::string spec;
    size_t colon = tok.find(':');
    if(colon != std::string::npos) {
      spec = tok.substr(colon + 1);
      tok = tok.substr(0, colon);
    }
    // Special placeholder "{rdp:XX}" reuses the RDP command-log decoder (the same
    // code the RDP viewer uses) to render an RDP command (e.g. the color combiner
    // 0x3C). The 56-bit RDP payload is reconstructed: word0 holds payload[55:32],
    // word1 holds payload[31:0].
    if(tok == "rdp") {
      u8 op = (u8)strtol(spec.c_str(), nullptr, 16);
      u64 w0 = ((u64)(words[0] & 0xFFFFFF) << 32) | (wordCount > 1 ? (u64)words[1] : 0);
      std::string d = rdpCommandDescription(op, w0).data();
      for(char& c : d) if(c == '\n') c = ' ';  // keep the cell single-line
      out += d;
      continue;
    }
    const ArgField* field = nullptr;
    for(auto& f : info.fields) if(f.name == tok.c_str()) { field = &f; break; }
    if(field) out += valueText(*field, spec.empty() ? 0 : spec[0]);
    else { out += '{'; out += tok; out += '}'; }  // unknown name: leave visible
  }
  return string{out.c_str()};
}

// Read the command currently pointed to by gp (rspq_dmem_buf_ptr) into the
// pending-command slot of the capture state.
static auto rspReadPendingCommand(RSP& rsp) -> void {
  auto& cap = rsp.capture;
  u32 gp = rsp.ipu.r[28].u32;          // rspq_dmem_buf_ptr: offset into the cmd buffer
  u32 base = cap.dmemCmdsOffset + gp;
  if(base > 0xFFF) {
    cap.segOverlay = 0;
    cap.segCommand = 0;
    cap.segWordCount = 0;
    return;
  }
  u32 firstWord = rsp.dmem.read<Word>(base);
  cap.segOverlay = (firstWord >> 28) & 0xF;
  cap.segCommand = (firstWord >> 24) & 0xF;
  // The real command size lives in the descriptor (t6/$14) the dispatcher just
  // loaded: bits 8.. hold the size in bytes (masked by RSPQ_DESCRIPTOR_SIZE_MASK).
  // Use it so we capture exactly the words this command uses, not the buffer max.
  u32 cmdDesc = rsp.ipu.r[14].u32;
  u32 words = ((cmdDesc >> 8) & 0xFC) / 4;       // 0xFC = RSPQ_DESCRIPTOR_SIZE_MASK
  if(words == 0) words = 1;                       // always include the command word
  u32 avail = (cap.dmemCmdsSize > gp) ? (cap.dmemCmdsSize - gp) / 4 : words;
  if(avail && words > avail) words = avail;
  if(words > RSPCapture::maxCommandWords) words = RSPCapture::maxCommandWords;
  cap.segWordCount = (u8)words;
  for(u32 i = 0; i < cap.segWordCount; i++) cap.segWords[i] = rsp.dmem.read<Word>(base + i * 4);
}

// Called from both the interpreter and the recompiler at every hooked RSPQ
// dispatch PC. Rather than capturing a command at every hook (which produced
// duplicates), we model the RSP as moving through a series of timed segments:
//
//   RSPQ_Loop --decode--> [load overlay] --> execute_command --run--> RSPQ_Loop
//
// Each hook marks a transition: we close the previous segment (emitting a row
// timestamped with the cycle delta since it began) and open the next one. This
// yields exactly one row per real command, plus synthetic "overhead" rows for
// the time spent dispatching in the loop, switching overlays, and refetching
// the command buffer.
auto RSP::captureCommandHook(u32 pc) -> void {
  auto& cap = capture;
  if(!cap.enabled.load(std::memory_order_relaxed)) {
    cap.segType = RSPCapture::SegNone;  // re-arm on next enable, avoids a stale delta
    return;
  }

  // Classify which segment is *beginning* at this PC.
  u8 newType;
  if(pc == cap.pcExecCommand)      newType = RSPCapture::SegCommand;
  else if(pc == cap.pcLoadOverlay) newType = RSPCapture::SegOverlayLoad;
  else if(pc == cap.pcFetchBuffer || pc == cap.pcFetchBufferPtr) newType = RSPCapture::SegFetch;
  else if(pc == cap.pcLoop)        newType = RSPCapture::SegLoop;
  else return;  // unknown hook, ignore

  // rspq_fetch_buffer falls through into rspq_fetch_buffer_with_ptr, so two
  // fetch hooks fire back-to-back. Treat a same-type transition as a
  // continuation of the current segment rather than starting a new row.
  if(newType == cap.segType) return;

  u64 now = pipeline.clocksTotal;

  // Close the previous segment, emitting its row with the elapsed cycle delta.
  if(cap.segType != RSPCapture::SegNone) {
    u64 delta = now - cap.segStart;
    if(cap.segType == RSPCapture::SegCommand) {
      cap.push(delta, cap.frameNumber, cap.segOverlay, cap.segCommand, cap.segWordCount, cap.segWords);

      // Step mode: flush GPU so the framebuffer is current, then spin until the
      // UI advances. Only meaningful at real command boundaries.
      if(cap.stepMode.load(std::memory_order_relaxed)) {
        vulkan.flush();
        rdp.capture.committedCount.store(rdp.capture.writePos.load(std::memory_order_acquire), std::memory_order_release);
        while(!cap.stepPending.load(std::memory_order_acquire)
              && cap.stepMode.load(std::memory_order_relaxed)) {
          usleep(1000);
        }
        cap.stepPending.store(false, std::memory_order_release);
      }
    } else {
      u8 overhead = cap.segType == RSPCapture::SegLoop        ? RSPCapture::OverheadLoop
                  : cap.segType == RSPCapture::SegOverlayLoad ? RSPCapture::OverheadOvlLoad
                  :                                             RSPCapture::OverheadFetch;
      cap.pushOverhead(delta, overhead);
    }
  }

  // Open the new segment.
  cap.segType = newType;
  cap.segStart = now;
  if(newType == RSPCapture::SegCommand) rspReadPendingCommand(*this);
}
