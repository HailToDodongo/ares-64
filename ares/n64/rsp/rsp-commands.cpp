// RSP command viewer — JSON config loading, ELF auto-detection, and capture

namespace {

// Minimal JSON string unescape
auto jsonUnescapeString(const char*& p) -> string {
  if(*p != '"') return {};
  p++;
  string result;
  while(*p && *p != '"') {
    if(*p == '\\' && p[1]) { p++; }
    result.append(*p++);
  }
  if(*p == '"') p++;
  return result;
}

auto jsonSkipWhitespace(const char*& p) -> void {
  while(*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
}

auto jsonExpect(const char*& p, char c) -> bool {
  jsonSkipWhitespace(p);
  if(*p != c) return false;
  p++;
  return true;
}

auto jsonReadHex(const char*& p, u32& out) -> bool {
  jsonSkipWhitespace(p);
  if(*p != '"') return false;
  p++;
  out = 0;
  while(*p && *p != '"') {
    char c = *p++;
    u32 nib = 0;
    if(c >= '0' && c <= '9') nib = c - '0';
    else if(c >= 'a' && c <= 'f') nib = c - 'a' + 10;
    else if(c >= 'A' && c <= 'F') nib = c - 'A' + 10;
    else if(c == 'x' || c == 'X') continue;
    else return false;
    out = (out << 4) | nib;
  }
  if(*p == '"') p++;
  return true;
}

auto jsonReadString(const char*& p, string& out) -> bool {
  jsonSkipWhitespace(p);
  out = jsonUnescapeString(p);
  return (bool)out;
}

auto jsonSkipValue(const char*& p) -> void {
  jsonSkipWhitespace(p);
  if(*p == '"') { p++; while(*p && *p != '"') { if(*p == '\\') p++; p++; } if(*p == '"') p++; }
  else if(*p == '{') { p++; int depth = 1; while(*p && depth) { if(*p == '{') depth++; if(*p == '}') depth--; p++; } }
  else if(*p == '[') { p++; int depth = 1; while(*p && depth) { if(*p == '[') depth++; if(*p == ']') depth--; p++; } }
  else { while(*p && *p != ',' && *p != '}' && *p != ']') p++; }
}

// ---- ELF64 big-endian symbol extraction ----

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

// Try to read a symbol's value from an ELF file. Handles both ELF32 and ELF64 big-endian.
static auto elfTryReadSymbol(const string& elfPath, const string& symName, u64& value) -> bool {
  string elfData = string::read(elfPath);
  if(!elfData || elfData.size() < 64) return false;

  const u8* data = (const u8*)elfData.data();
  u32 size = elfData.size();

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

} // anonymous namespace

auto RSPCapture::loadConfig(const string& jsonPath) -> bool {
  string data = string::read(jsonPath);
  if(!data) return false;

  const char* p = data.data();
  jsonSkipWhitespace(p);
  if(!jsonExpect(p, '{')) return false;

  // Reset state
  hookCount = 0;
  dmemCmdsOffset = 0;
  dmemCmdsSize = 0;
  dmemDramAddrOffset = 0;
  dmemOvlTableOffset = 0;
  dmemOvlIdmapOffset = 0;
  banner = {};

  while(*p && *p != '}') {
    jsonSkipWhitespace(p);
    if(*p == '}') break;
    if(*p == ',') { p++; continue; }

    string key = jsonUnescapeString(p);
    if(!key) break;
    if(!jsonExpect(p, ':')) break;

    if(key == "banner") {
      jsonReadString(p, banner);
    } else if(key == "imem") {
      if(!jsonExpect(p, '{')) break;
      while(*p && *p != '}') {
        jsonSkipWhitespace(p);
        if(*p == '}') break;
        if(*p == ',') { p++; continue; }
        string addrKey = jsonUnescapeString(p);
        if(!addrKey) break;
        if(!jsonExpect(p, ':')) break;
        u32 addr = 0;
        if(jsonReadHex(p, addr) && hookCount < maxHookAddresses) {
          hookAddresses[hookCount++] = addr;
        }
      }
      if(!jsonExpect(p, '}')) break;
    } else if(key == "dmem") {
      if(!jsonExpect(p, '{')) break;
      while(*p && *p != '}') {
        jsonSkipWhitespace(p);
        if(*p == '}') break;
        if(*p == ',') { p++; continue; }
        string dmemKey = jsonUnescapeString(p);
        if(!dmemKey) break;
        if(!jsonExpect(p, ':')) break;
        u32 val = 0;
        if(jsonReadHex(p, val)) {
          if(dmemKey == "cmds_offset") dmemCmdsOffset = val;
          else if(dmemKey == "cmds_size") dmemCmdsSize = val;
          else if(dmemKey == "dram_addr_offset") dmemDramAddrOffset = val;
          else if(dmemKey == "ovl_table_offset") dmemOvlTableOffset = val;
          else if(dmemKey == "ovl_idmap_offset") dmemOvlIdmapOffset = val;
        }
      }
      if(!jsonExpect(p, '}')) break;
    } else if(key == "overlays" || key == "overlay_id_map") {
      jsonSkipValue(p);
    } else {
      jsonSkipValue(p);
    }
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
  string elfPath;

  // 1) Direct replacement: rom.z64 -> rom.elf
  string directPath = {base, ".elf"};
  if(string::read(directPath)) { elfPath = directPath; }

  // 2) build/ subdirectory of the ROM's own directory
  if(!elfPath) {
    string buildPath = {base, "/build/", stringBasename(base), ".elf"};
    if(string::read(buildPath)) { elfPath = buildPath; }
  }

  // 3) Parent dir's build/ subdirectory (e.g., examples/00_quad/build/name.elf)
  if(!elfPath) {
    string parentPath = {stringDirname(base), "/build/", stringBasename(base), ".elf"};
    if(string::read(parentPath)) { elfPath = parentPath; }
  }

  if(!elfPath) return false;

  // Read rsp_queue_text_start from ELF — this gives us RDRAM address of IMEM code
  u64 textStart = 0;
  if(!elfTryReadSymbol(elfPath, "rsp_queue_text_start", textStart)) return false;
  u64 dataStart = 0;
  if(!elfTryReadSymbol(elfPath, "rsp_queue_data_start", dataStart)) return false;

  // rsp_queue_text_start is in KSEG0 (0x80000000). The RSPQ code is at offset 0
  // within IMEM when loaded via rsp_load().
  // The DMEM layout is standard per rsp_queue_t.

  // Set up known DMEM layout (same for all libdragon versions using rsp_queue.inc)
  dmemCmdsOffset     = 0x0A0;
  dmemCmdsSize       = 0x100;
  dmemDramAddrOffset = 0x1DC;
  dmemOvlTableOffset = 0x008;
  dmemOvlIdmapOffset = 0x048;

  // Set up known IMEM hook addresses (offsets within IMEM, i.e., 12-bit RSP PC values)
  // These are stable per libdragon's rsp_queue.inc assembly.
  // Values: RSP PC = (symbol_addr & 0xFFF), stripping the IMEM bit (0x1000)
  hookCount = 0;
  hookAddresses[hookCount++] = 0x0B4; // rspq_execute_command

  configLoaded = true;
  enabled.store(true, std::memory_order_relaxed);
  return true;
}

auto RSPCapture::detectRspq() -> bool {
  if(!configLoaded) return false;
  return true;
}

// Called from the interpreter hook when the RSP is about to dispatch a command.
auto rspCaptureCommand(u64 cycle, u32 cmdDmemOffset) -> void {
  auto& rsp = ares::Nintendo64::rsp;
  auto& cap = rsp.capture;
  if(!cap.enabled.load(std::memory_order_relaxed)) return;

  u32 base = cap.dmemCmdsOffset + cmdDmemOffset;
  if(base > 0xFFF) return;

  // Read first word to get command ID
  u32 firstWord = rsp.dmem.read<Word>(base);
  u8 overlayId = (firstWord >> 28) & 0xF;
  u8 commandId = (firstWord >> 24) & 0xF;

  // Read up to 16 argument words
  u32 words[16];
  u32 maxWords = (cap.dmemCmdsSize - cmdDmemOffset) / 4;
  if(maxWords > 16) maxWords = 16;
  u8 wordCount = (u8)maxWords;
  for(u32 i = 0; i < wordCount; i++) {
    words[i] = rsp.dmem.read<Word>(base + i * 4);
  }

  cap.push(cycle, cap.frameNumber, overlayId, commandId, wordCount, words);

  // Step mode: flush GPU so framebuffer is current, then spin until UI advances
  if(cap.stepMode.load(std::memory_order_relaxed)) {
    vulkan.flush();
    rdp.capture.committedCount.store(rdp.capture.writePos.load(std::memory_order_acquire), std::memory_order_release);
    while(!cap.stepPending.load(std::memory_order_acquire)
          && cap.stepMode.load(std::memory_order_relaxed)) {}
    cap.stepPending.store(false, std::memory_order_release);
  }
}

auto RSP::captureCommandHook() -> void {
  static u64 lastClocks = 0;
  u64 delta = pipeline.clocksTotal - lastClocks;
  lastClocks = pipeline.clocksTotal;
  rspCaptureCommand(delta, ipu.r[28].u32);
}
