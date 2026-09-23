//NEC VR4300

struct CPU : Thread {
  Node::Object node;

  struct Debugger {
    //debugger.cpp
    auto load(Node::Object) -> void;
    auto unload() -> void;
    auto instruction(u64 address, u32 instruction) -> void;
    auto exception(u8 code) -> void;
    auto interrupt(u8 mask) -> void;
    auto nmi() -> void;
    auto tlbWrite(u32 index) -> void;
    auto tlbModification(u64 address) -> void;
    auto tlbLoad(u64 address, u64 physical) -> void;
    auto tlbLoadInvalid(u64 address) -> void;
    auto tlbLoadMiss(u64 address) -> void;
    auto tlbStore(u64 address, u64 physical) -> void;
    auto tlbStoreInvalid(u64 address) -> void;
    auto tlbStoreMiss(u64 address) -> void;

    struct Tracer {
      Node::Debugger::Tracer::Instruction instruction;
      Node::Debugger::Tracer::Notification exception;
      Node::Debugger::Tracer::Notification interrupt;
      Node::Debugger::Tracer::Notification tlb;
      Node::Debugger::Tracer::Notification emux;
    } tracer;
  } debugger;

  //cpu.cpp
  auto load(Node::Object) -> void;
  auto unload() -> void;

  static constexpr u64 CountMask = (1ull << 33) - 1;

  auto main() -> void;
  auto synchronize() -> void;
  auto stepCount(u64 clocks) -> void;
  auto flushCount() -> void;
  auto pendingCount() const -> u64 { return (Thread::clock - countClock) >> 1; }
  auto effectiveCount() const -> u64 { return (scc.count + pendingCount()) & CountMask; }
  auto forceSynchronize() -> void;
  auto setInterruptPending(u32 bit, bool value) -> void;
  auto interruptPoll() -> void;

  auto gdbPoll() -> void;
  auto scriptPoll() -> void;
  auto queueInsert(u32 event, u32 clocks) -> void;

  auto instruction() -> bool;
  auto instructionPrologue(u64 address, u32 instruction) -> void;
  template<bool Recompiled> auto instructionEpilogue() -> void;
  auto raiseCoprocessor1Exception() -> void;
  auto icacheFillLine(u64 vaddr, u32 paddr) -> void;

  //profiler.cpp: recompute whether the per-instruction prologue hook must be
  //emitted by the JIT (needed by the instruction tracer and/or the profiler).
  auto updatePrologueHook() -> void;

  auto power(bool reset) -> void;

  struct Pipeline {
    CPU& self;
    u64 pc     = 0;  //pc after current instruction
    u64 nextpc = 0;  //pc after next instruction
    u32 state  = 0;  //current branch state
    u32 nstate = 0;  //next branch state

    enum : u32 {
      EndBlock  = 1 << 0,
      DelaySlot = 1 << 1,
    };

    auto inDelaySlot() const -> bool { return state & DelaySlot; }
    auto setPc(u64 address) -> void { self.ipu.pc = pc = address; nextpc = address + 4; state = nstate = 0; }
    auto branch(u64 address) -> void { nextpc = address; nstate |= DelaySlot | EndBlock; }
    auto noBranch() -> void { nstate |= DelaySlot; }
    auto exception() -> void { state |= EndBlock; }
    auto skip() -> void { pc += 4; nextpc = pc + 4; state |= EndBlock; }
    auto begin() -> void {
      nstate = 0;
      pc = nextpc;
      nextpc += 4;
    }
    auto end() -> void {
      state = nstate;
      self.ipu.pc = pc;
    }
  } pipeline{*this};

  struct OpInfo {
    enum : u32 {
      Branch        = 1 << 0,
      LikelyBranch  = 1 << 1,
      JitStateKeyMayChange = 1 << 2,
      CountCompareWrite = 1 << 3,
      UnconditionalJump = 1 << 4,
      UnconditionalJumpAndLink = 1 << 5,
    };

    u32 flags = 0;

    auto branch() const -> bool { return flags & Branch; }
    auto likelyBranch() const -> bool { return flags & LikelyBranch; }
    auto jitStateKeyMayChange() const -> bool { return flags & JitStateKeyMayChange; }
    auto countCompareWrite() const -> bool { return flags & CountCompareWrite; }
    auto unconditionalJump() const -> bool { return flags & UnconditionalJump; }
    auto unconditionalJumpAndLink() const -> bool { return flags & UnconditionalJumpAndLink; }
  };

  struct PhysAccess {
    enum Direction : u32 { Read, Write };

    explicit operator bool() const { return found; }

    bool found;   //this is a valid physical access
    bool cache;   //access must go through cache
    u32  paddr;   //physical address on 32-bit MIPS bus
    u64  vaddr;   //virtual address used on the CPU (64-bit)
  };

  //context.cpp
  struct Context {
    CPU& self;
    Context(CPU& self) : self(self) {}

    enum Endian : bool { Little, Big };
    enum Mode : u32 { Kernel, Supervisor, User };
    enum Segment : u32 { Unused, Mapped, Cached, Direct, Cached32, Direct32, Kernel64, Supervisor64, User64 };

    auto littleEndian() const -> bool { return endian == Endian::Little; }
    auto bigEndian() const -> bool { return endian == Endian::Big; }

    auto kernelMode() const -> bool { return mode == Mode::Kernel; }
    auto supervisorMode() const -> bool { return mode == Mode::Supervisor; }
    auto userMode() const -> bool { return mode == Mode::User; }

    auto setMode() -> void;

    bool endian;
    u64  physMask;
    u32  mode;
    u32  bits;
    u32  segment[8];  //512_MiB chunks
  } context{*this};

  //icache.cpp
  struct InstructionCache {
    CPU& self;
    struct Line;
    auto line(u64 vaddr) -> Line& { return lines[vaddr >> 5 & 0x1ff]; }

    //call by recompiled blocks to prefetch instructions into the cache
    auto jitFetch(u64 vaddr, u32 paddr, CPU& cpu) -> void {
      auto& line = this->line(vaddr);
      if(!line.hit(paddr)) {
        self.profile.icacheMisses++;
        line.fill(paddr, cpu);
      } else {
        self.profile.icacheHits++;
      }
    }

    //used by the interpreter to fully emulate the instruction cache
    auto fetch(u64 vaddr, u32 paddr, CPU& cpu) -> u32 {
      auto& line = this->line(vaddr);
      if(!line.hit(paddr)) {
        self.profile.icacheMisses++;
        line.fill(paddr, cpu);
      } else {
        self.profile.icacheHits++;
      }
      return line.read(paddr);
    }

    auto coherent(u64 vaddr, u32 paddr) -> bool {
      auto& line = this->line(vaddr);
      if(!line.hit(paddr))
        return true;
      u32 ram[8];
      self.busReadBurst<ICache>(paddr & ~0x0000'0fff | line.index, ram, false);  //probe: not real bus traffic
      for (int i=0; i<8; i++)
        if (ram[i] != line.words[i])
          return false;
      return true;
    }

    auto power(bool reset) -> void {
      u32 index = 0;
      for(auto& line : lines) {
        line.tagKey = 0;
        line.index = index++ << 5 & 0xfe0;
        for(auto& word : line.words) word = 0;
       }
    }

    //16KB
    struct Line {
      auto valid() const -> bool { return tagKey & 1u; }

      auto setValid(bool on) -> void {
        if(on) tagKey |= 1u;
        else tagKey &= ~1u;
      }

      auto hit(u32 paddr) const -> bool {
        const u32 t = paddr & ~0x0000'0fffu;
        return valid() && (tagKey & ~1u) == t;
      }

      auto fill(u32 paddr, CPU& cpu) -> void {
        const u32 tag = paddr & ~0x0000'0fffu;
        profileEvict(cpu);  //the content being replaced
        eventId = cpu.profileCacheFill(Profiler::CacheIFill, tag | index);
#if ARES_DEBUG_TOOLS
        //the instruction whose fetch caused this fill was already announced
        //to the profiler (the JIT runs the prologue before its line check)
        executed = cpu.profiler.takePendingExec(tag | index);
#endif
        cpu.step(48 * 2);
        tagKey = tag;
        setValid(true);
        cpu.busReadBurst<ICache>(tag | index, words);
        cpu.profileCacheContent(eventId, words, 8);  //what was fetched, for the flame chart
      }

      auto writeBack(CPU& cpu) -> void {
        cpu.step(48 * 2);
        const u32 tag = tagKey & ~0x0000'0fffu;
        cpu.busWriteBurst<ICache>(tag | index, words);
      }

      auto read(u32 paddr) const -> u32 { return words[paddr >> 2 & 7]; }

      //profiler: close the line's fill event with what was executed out of it
      auto profileEvict(CPU& cpu) -> void {
        cpu.profileCacheEvict(eventId, executed, 0);
        eventId = 0;
        executed = 0;
      }

      u32  tagKey;    // valid bit (bit 0) + tag
      u16  index;
      u32  words[8];
      //profiling only (not serialized): words executed since the fill, and the
      //fill's slot in the profiler's cache-event ring (0 = none)
      u8   executed = 0;
      u64  eventId = 0;
    } lines[512];
  } icache{*this};

  //dcache.cpp
  struct DataCache {
    CPU& self;
    struct Line;
    auto line(u64 vaddr) -> Line&;
    template<u32 Size> auto read(u64 vaddr, u32 paddr) -> u64;
    template<u32 Size> auto write(u64 vaddr, u32 paddr, u64 data) -> void;
    auto power(bool reset) -> void;

    template<u32 Size> auto readDebug(u64 vaddr, u32 paddr) -> u64;
    template<u32 Size> auto writeDebug(u64 vaddr, u32 paddr, u64 value) -> void;

    //8KB
    struct Line {
      auto valid() const -> bool { return tagKey & 1u; }
      auto setValid(bool on) -> void {
        if(on) tagKey |= 1u;
        else tagKey &= ~1u;
      }

      auto hit(u32 paddr) const -> bool;
      auto fill(u32 paddr) -> void;
      auto writeBack() -> void;
      auto profileEvict() -> void;  //close the fill event with the bytes read/written
      template<u32 Size> auto read(u32 paddr) const -> u64;
      template<u32 Size> auto write(u32 paddr, u64 data) -> void;

      u32  tagKey;
      u16  dirty;
      u16  index;
      u64  fillPc;
      u64  dirtyPc;
      //profiling only (not serialized): bytes read since the fill (writes are in
      //`dirty`), and the fill's slot in the profiler's cache-event ring (0 = none)
      u16  touched = 0;
      u16  writtenBack = 0;  //dirty bytes already flushed by a `cache hit write back`
      u64  eventId = 0;
      union {
        u8  bytes[16];
        u16 halfs[8];
        u32 words[4];
      };
    } lines[512];
  } dcache{*this};

  //tlb.cpp: Translation Lookaside Buffer
  struct TLB {
    CPU& self;
    TLB(CPU& self) : self(self) {}
    static constexpr u32 Entries = 32;

    struct Entry {
      //scc-tlb.cpp
      auto synchronize() -> void;

      n1  global[2];
      n1  valid[2];
      n1  dirty[2];
      n3  cacheAlgorithm[2];
      n36 physicalAddress[2];
      n32 pageMask;
      n40 virtualAddress;
      n8  addressSpaceID;
      n2  region;
      //internal:
      n1  globals;
      n40 addressMaskHi;
      n40 addressMaskLo;
      n40 addressSelect;
    } entry[TLB::Entries];

    //tlb.cpp
    auto load(u64 vaddr, bool noExceptions = false) -> PhysAccess;
    auto load(u64 vaddr, const Entry& entry, bool noExceptions = false) -> maybe<PhysAccess>;
    
    auto store(u64 vaddr, bool noExceptions = false) -> PhysAccess;
    auto store(u64 vaddr, const Entry& entry, bool noExceptions = false) -> maybe<PhysAccess>;

    struct TlbCache { ;
      static constexpr int entries = 4;

      struct CachedTlbEntry {
        const Entry *entry;
        int frequency;
      } entry[entries];

      void insert(const Entry& entry) {
        this->entry[refresh()].entry = &entry;
      }

      int refresh() {
        CachedTlbEntry* leastUsed = &entry[0];
        int index = 0;

        for(auto n = 0; n < entries; n++) {
          if(entry[n].frequency < leastUsed->frequency) {
            index = n;
            leastUsed = &entry[n];
          }
        }

        leastUsed->entry = nullptr;
        leastUsed->frequency = 0;
        return index;
      }
    } tlbCache;

    u32 physicalAddress;
  } tlb{*this};

  //memory.cpp
  auto kernelSegment32(u32 vaddr) const -> Context::Segment;
  auto supervisorSegment32(u32 vaddr) const -> Context::Segment;
  auto userSegment32(u32 vaddr) const -> Context::Segment;

  auto kernelSegment64(u64 vaddr) const -> Context::Segment;
  auto supervisorSegment64(u64 vaddr) const -> Context::Segment;
  auto userSegment64(u64 vaddr) const -> Context::Segment;

  auto segment(u64 vaddr) -> Context::Segment;
  template<u32 Dir, u32 Size> auto devirtualize(u64 vaddr, bool raiseAlignedError = true, bool raiseExceptions = true) -> PhysAccess;
  auto devirtualizeDebug(u64 vaddr) -> u64;

  auto fetch(PhysAccess access) -> maybe<u32>;
  auto jitFetch(u64 vaddr, u32 addr) -> void {
    icache.jitFetch(vaddr, addr, *this);
  }

  auto jitIcacheFillMiss(u64 vaddr, u32 paddr) -> void {
    auto& line = icache.line(vaddr);
    profile.icacheMisses++;
    line.fill(paddr, *this);
  }
  auto profileBusAccess(bool toRDRAM, u32 address, u64 bytes) -> void;  //memory.cpp
  //profiler.cpp: cache line transfers (Profiler::CacheKind). A fill returns the
  //event's id so the line can close it on eviction with its utilisation masks.
  auto profileCacheFill(u8 kind, u32 address) -> u64;
  auto profileCacheContent(u64 eventId, const u32* words, u32 count) -> void;
  auto profileCacheEvict(u64 eventId, u16 readMask, u16 writeMask) -> void;
  template<u32 Size> auto busWrite(u32 address, u64 data) -> void;
  template<u32 Size> auto busRead(u32 address) -> u64;
  template<u32 Size> auto busWriteBurst(u32 address, u32 *data) -> bool;
  //count=false skips memory-bandwidth profiling for emulator-only probe reads
  //(e.g. the icache coherency check) that real hardware never drives on the bus.
  template<u32 Size> auto busReadBurst(u32 address, u32 *data, bool count = true) -> bool;
  template<u32 Size> auto read(PhysAccess access) -> maybe<u64>;
  template<u32 Size> auto write(PhysAccess access, u64 data) -> bool;
  template<u32 Size> auto read(u64 vaddr) -> maybe<u64> {
    return read<Size>(devirtualize<Read, Size>(vaddr));
  }
  template<u32 Size> auto write(u64 vaddr, u64 data, bool alignedError = true) -> bool {
    return write<Size>(devirtualize<Write, Size>(vaddr, alignedError), data);
  }
  template<u32 Size> auto vaddrAlignedError(u64 vaddr, bool write) -> bool;
  auto addressException(u64 vaddr) -> void;
  auto emuxException(u8 kind) -> void;

  template <u32 Size> auto readDebug(u64 vaddr) -> u64;
  template <u32 Size> auto writeDebug(u64 vaddr, u64 data) -> bool;

  //serialization.cpp
  auto serialize(serializer&) -> void;

  //exception.cpp
  struct Exception {
    CPU& self;
    Exception(CPU& self) : self(self) {}

    auto trigger(u32 code, u32 coprocessor = 0, bool tlbMiss = 0) -> void;
    auto reportGDBException(int code, u64 pc) -> void;

    auto interrupt() -> void;
    auto tlbModification() -> void;
    auto tlbLoadInvalid() -> void;
    auto tlbLoadMiss() -> void;
    auto tlbStoreInvalid() -> void;
    auto tlbStoreMiss() -> void;
    auto addressLoad() -> void;
    auto addressStore() -> void;
    auto busInstruction() -> void;
    auto busData() -> void;
    auto systemCall() -> void;
    auto breakpoint() -> void;
    auto reservedInstruction() -> void;
    auto reservedInstructionCop2() -> void;
    auto coprocessor0() -> void;
    auto coprocessor1() -> void;
    auto coprocessor2() -> void;
    auto coprocessor3() -> void;
    auto arithmeticOverflow() -> void;
    auto trap() -> void;
    auto emux() -> void;
    auto floatingPoint() -> void;
    auto watchAddress() -> void;
    auto nmi() -> void;
  } exception{*this};

  enum Interrupt : u32 {
    Software0 = 0,
    Software1 = 1,
    RCP       = 2,
    Cartridge = 3,
    Reset     = 4,
    ReadRDB   = 5,
    WriteRDB  = 6,
    Timer     = 7,
  };

  //ipu.cpp
  union r64 {
    struct {   int32_t order_msb2(s32h, s32); };
    struct {  uint32_t order_msb2(u32h, u32); };
    struct { float32_t order_msb2(f32h, f32); };
    struct {   int64_t s64; };
    struct {  uint64_t u64; };
    struct { float64_t f64; };
  };
  using cr64 = const r64;

  struct IPU {
    enum Register : u32 {
      R0,                              //zero (read-only)
      AT,                              //assembler temporary
      V0, V1,                          //arithmetic values
      A0, A1, A2, A3,                  //subroutine parameters
      T0, T1, T2, T3, T4, T5, T6, T7,  //temporary registers
      S0, S1, S2, S3, S4, S5, S6, S7,  //saved registers
      T8, T9,                          //temporary registers
      K0, K1,                          //kernel registers
      GP,                              //global pointer
      SP,                              //stack pointer
      S8,                              //saved register
      RA,                              //return address
    };

    r64 r[32];
    r64 lo;
    r64 hi;
    u64 pc;  //program counter
  } ipu;

  //algorithms.cpp
  template<typename T> auto roundNearest(f32 f) -> T;
  template<typename T> auto roundNearest(f64 f) -> T;
  template<typename T> auto roundCeil(f32 f) -> T;
  template<typename T> auto roundCeil(f64 f) -> T;
  template<typename T> auto roundCurrent(f32 f) -> T;
  template<typename T> auto roundCurrent(f64 f) -> T;
  template<typename T> auto roundFloor(f32 f) -> T;
  template<typename T> auto roundFloor(f64 f) -> T;
  template<typename T> auto roundTrunc(f32 f) -> T;
  template<typename T> auto roundTrunc(f64 f) -> T;
  auto squareRoot(f32 f) -> f32;
  auto squareRoot(f64 f) -> f64;

  //interpreter-ipu.cpp
  auto ADD(r64& rd, cr64& rs, cr64& rt) -> void;
  auto ADDI(r64& rt, cr64& rs, s16 imm) -> void;
  auto ADDIU(r64& rt, cr64& rs, s16 imm) -> void;
  auto ADDU(r64& rd, cr64& rs, cr64& rt) -> void;
  auto AND(r64& rd, cr64& rs, cr64& rt) -> void;
  auto ANDI(r64& rt, cr64& rs, u16 imm) -> void;
  auto BEQ(cr64& rs, cr64& rt, s16 imm) -> void;
  auto BEQL(cr64& rs, cr64& rt, s16 imm) -> void;
  auto BGEZ(cr64& rs, s16 imm) -> void;
  auto BGEZAL(cr64& rs, s16 imm) -> void;
  auto BGEZALL(cr64& rs, s16 imm) -> void;
  auto BGEZL(cr64& rs, s16 imm) -> void;
  auto BGTZ(cr64& rs, s16 imm) -> void;
  auto BGTZL(cr64& rs, s16 imm) -> void;
  auto BLEZ(cr64& rs, s16 imm) -> void;
  auto BLEZL(cr64& rs, s16 imm) -> void;
  auto BLTZ(cr64& rs, s16 imm) -> void;
  auto BLTZAL(cr64& rs, s16 imm) -> void;
  auto BLTZALL(cr64& rs, s16 imm) -> void;
  auto BLTZL(cr64& rs, s16 imm) -> void;
  auto BNE(cr64& rs, cr64& rt, s16 imm) -> void;
  auto BNEL(cr64& rs, cr64& rt, s16 imm) -> void;
  auto BREAK() -> void;
  auto CACHE(u8 operation, cr64& rs, s16 imm) -> void;
  auto DADD(r64& rd, cr64& rs, cr64& rt) -> void;
  auto DADDI(r64& rt, cr64& rs, s16 imm) -> void;
  auto DADDIU(r64& rt, cr64& rs, s16 imm) -> void;
  auto DADDU(r64& rd, cr64& rs, cr64& rt) -> void;
  auto DDIV(cr64& rs, cr64& rt) -> void;
  auto DDIVU(cr64& rs, cr64& rt) -> void;
  auto DIV(cr64& rs, cr64& rt) -> void;
  auto DIVU(cr64& rs, cr64& rt) -> void;
  auto DMULT(cr64& rs, cr64& rt) -> void;
  auto DMULTU(cr64& rs, cr64& rt) -> void;
  auto DSLL(r64& rd, cr64& rt, u8 sa) -> void;
  auto DSLLV(r64& rd, cr64& rt, cr64& rs) -> void;
  auto DSRA(r64& rd, cr64& rt, u8 sa) -> void;
  auto DSRAV(r64& rd, cr64& rt, cr64& rs) -> void;
  auto DSRL(r64& rd, cr64& rt, u8 sa) -> void;
  auto DSRLV(r64& rd, cr64& rt, cr64& rs) -> void;
  auto DSUB(r64& rd, cr64& rs, cr64& rt) -> void;
  auto DSUBU(r64& rd, cr64& rs, cr64& rt) -> void;
  auto J(u32 imm) -> void;
  auto JAL(u32 imm) -> void;
  auto JALR(r64& rd, cr64& rs) -> void;
  auto JR(cr64& rs) -> void;
  auto LB(r64& rt, cr64& rs, s16 imm) -> void;
  auto LBU(r64& rt, cr64& rs, s16 imm) -> void;
  auto LD(r64& rt, cr64& rs, s16 imm) -> void;
  auto LDL(r64& rt, cr64& rs, s16 imm) -> void;
  auto LDR(r64& rt, cr64& rs, s16 imm) -> void;
  auto LH(r64& rt, cr64& rs, s16 imm) -> void;
  auto LHU(r64& rt, cr64& rs, s16 imm) -> void;
  auto LUI(r64& rt, u16 imm) -> void;
  auto LL(r64& rt, cr64& rs, s16 imm) -> void;
  auto LLD(r64& rt, cr64& rs, s16 imm) -> void;
  auto LW(r64& rt, cr64& rs, s16 imm) -> void;
  auto LWL(r64& rt, cr64& rs, s16 imm) -> void;
  auto LWR(r64& rt, cr64& rs, s16 imm) -> void;
  auto LWU(r64& rt, cr64& rs, s16 imm) -> void;
  auto MFHI(r64& rd) -> void;
  auto MFLO(r64& rd) -> void;
  auto MTHI(cr64& rs) -> void;
  auto MTLO(cr64& rs) -> void;
  auto MULT(cr64& rs, cr64& rt) -> void;
  auto MULTU(cr64& rs, cr64& rt) -> void;
  auto NOR(r64& rd, cr64& rs, cr64& rt) -> void;
  auto OR(r64& rd, cr64& rs, cr64& rt) -> void;
  auto ORI(r64& rt, cr64& rs, u16 imm) -> void;
  auto SB(cr64& rt, cr64& rs, s16 imm) -> void;
  auto SC(r64& rt, cr64& rs, s16 imm) -> void;
  auto SD(cr64& rt, cr64& rs, s16 imm) -> void;
  auto SCD(r64& rt, cr64& rs, s16 imm) -> void;
  auto SDL(cr64& rt, cr64& rs, s16 imm) -> void;
  auto SDR(cr64& rt, cr64& rs, s16 imm) -> void;
  auto SH(cr64& rt, cr64& rs, s16 imm) -> void;
  auto SLL(r64& rd, cr64& rt, u8 sa) -> void;
  auto SLLV(r64& rd, cr64& rt, cr64& rs) -> void;
  auto SLT(r64& rd, cr64& rs, cr64& rt) -> void;
  auto SLTI(r64& rt, cr64& rs, s16 imm) -> void;
  auto SLTIU(r64& rt, cr64& rs, s16 imm) -> void;
  auto SLTU(r64& rd, cr64& rs, cr64& rt) -> void;
  auto SRA(r64& rd, cr64& rt, u8 sa) -> void;
  auto SRAV(r64& rd, cr64& rt, cr64& rs) -> void;
  auto SRL(r64& rd, cr64& rt, u8 sa) -> void;
  auto SRLV(r64& rd, cr64& rt, cr64& rs) -> void;
  auto SUB(r64& rd, cr64& rs, cr64& rt) -> void;
  auto SUBU(r64& rd, cr64& rs, cr64& rt) -> void;
  auto SW(cr64& rt, cr64& rs, s16 imm) -> void;
  auto SWL(cr64& rt, cr64& rs, s16 imm) -> void;
  auto SWR(cr64& rt, cr64& rs, s16 imm) -> void;
  auto SYNC() -> void;
  auto SYSCALL() -> void;
  auto TEQ(cr64& rs, cr64& rt) -> void;
  auto TEQI(cr64& rs, s16 imm) -> void;
  auto TGE(cr64& rs, cr64& rt) -> void;
  auto TGEI(cr64& rs, s16 imm) -> void;
  auto TGEIU(cr64& rs, s16 imm) -> void;
  auto TGEU(cr64& rs, cr64& rt) -> void;
  auto TLT(cr64& rs, cr64& rt) -> void;
  auto TLTI(cr64& rs, s16 imm) -> void;
  auto TLTIU(cr64& rs, s16 imm) -> void;
  auto TLTU(cr64& rs, cr64& rt) -> void;
  auto TNE(cr64& rs, cr64& rt) -> void;
  auto TNEI(cr64& rs, s16 imm) -> void;
  auto XOR(r64& rd, cr64& rs, cr64& rt) -> void;
  auto XORI(r64& rt, cr64& rs, u16 imm) -> void;

  struct SCC {
    //0
    struct Index {
      n6 tlbEntry;
      n1 probeFailure;
    } index;

    //1: Random
    //2: EntryLo0
    //3: EntryLo1
    //5: PageMask
    //10: EntryHi
    TLB::Entry tlb;

    //4
    struct Context {
      n19 badVirtualAddress;
      n41 pageTableEntryBase;
    } context;

    //6
    struct Wired {
      n6 index;
    } wired;

    //8
    n64 badVirtualAddress;

    //9
    n33 count;  //32-bit; +1 to count half-cycles

    //11
    n33 compare;

    //12
    struct Status {
      n1 interruptEnable;
      n1 exceptionLevel;
      n1 errorLevel = 1;
      n2 privilegeMode;
      n1 userExtendedAddressing;
      n1 supervisorExtendedAddressing;
      n1 kernelExtendedAddressing;
      n8 interruptMask = 0xff;
      n1 de;  //unused
      n1 ce;  //unused
      n1 condition;
      n1 softReset = 1;
      n1 tlbShutdown;
      n1 vectorLocation = 1;
      n1 instructionTracing;
      n1 reverseEndian;
      n1 floatingPointMode = 1;
      n1 lowPowerMode;
      struct Enable {
        n1 coprocessor0 = 1;
        n1 coprocessor1 = 1;
        n1 coprocessor2;
        n1 coprocessor3;
      } enable;
    } status;

    //13
    struct Cause {
      n5 exceptionCode;
      n8 interruptPending;
      n2 coprocessorError;
      n1 branchDelay;
    } cause;

    //14: Exception Program Counter
    n64 epc;

    //15: Coprocessor Revision Identifier
    struct Coprocessor {
      static constexpr u8 revision = 0x22;
      static constexpr u8 implementation = 0x0b;
    } coprocessor;

    //16
    struct Configuration {
      n2 coherencyAlgorithmKSEG0;
      n2 cu;  //reserved
      n1 bigEndian = 1;
      n2 sysadWritebackPattern;
      n3 systemClockRatio = 7;
    } configuration;

    //17: Load Linked Address
    n32 ll;
    n1  llbit;

    //18
    struct WatchLo {
      n1  trapOnWrite;
      n1  trapOnRead;
      n32 physicalAddress;
    } watchLo;

    //19
    struct WatchHi {
      n4 physicalAddressExtended;  //unused; for R4000 compatibility only
    } watchHi;

    //20
    struct XContext {
      n27 badVirtualAddress;
      n2  region;
      n31 pageTableEntryBase;
    } xcontext;

    //26
    struct ParityError {
      n8 diagnostic;  //unused; for R4000 compatibility only
    } parityError;

    //27
    struct CacheError {
      n32 unused;     //unused; for R4000 compatibility only
    } cacheError;

    //28
    struct TagLo {
      auto primaryCacheState() const -> n2 { return value.bit(6,7); }
      auto physicalAddress() const -> n32 { return value.bit(8,27) << 12; }

      auto setPrimaryCacheState(n2 state) -> void { value.bit(6,7) = state; }
      auto setPhysicalAddress(n32 address) -> void { value.bit(8,27) = address >> 12; }

      n32 value;
    } tagLo;

    //30: Error Exception Program Counter
    n64 epcError;

    //other
    n64 latch;
    n1 nmiPending;
    n1 sysadFrozen;
  } scc;

  //interpreter-scc.cpp
  auto getControlRegister(n5) -> u64;
  auto setControlRegister(n5, n64) -> void;
  auto getControlRandom() -> u8;

  auto DMFC0(r64& rt, u8 rd) -> void;
  auto DMTC0(cr64& rt, u8 rd) -> void;
  auto ERET() -> void;
  auto MFC0(r64& rt, u8 rd) -> void;
  auto MTC0(cr64& rt, u8 rd) -> void;
  auto TLBP() -> void;
  auto TLBR() -> void;
  auto TLBWI() -> void;
  auto TLBWR() -> void;

  struct FPU {
    auto setFloatingPointMode(bool) -> void;

    r64 r[32];

    struct Coprocessor {
      static constexpr u8 revision = 0x00;
      static constexpr u8 implementation = 0x0a;
    } coprocessor;

    struct ControlStatus {
      n2 roundMode = 0;
#if defined(ARCHITECTURE_ARM64)
      enum : u32 {
        InvalidOperationBit       = 0,
        DivisionByZeroBit         = 1,
        OverflowBit               = 2,
        UnderflowBit              = 3,
        InexactBit                = 4,
        DenormalBit               = 7,
        UnimplementedOperationBit = 6,
      };
#else
      enum : u32 {
        InvalidOperationBit       = 0,
        DenormalBit               = 1,
        DivisionByZeroBit         = 2,
        OverflowBit               = 3,
        UnderflowBit              = 4,
        InexactBit                = 5,
        UnimplementedOperationBit = 6,
      };
#endif
      template<bool HasUnimplemented>
      struct ExceptionBits {
        n8 data = 0;

        auto inexact() const -> bool { return data.bit(InexactBit); }
        auto setInexact(bool value) -> void { data.bit(InexactBit) = value; }

        auto underflow() const -> bool { return data.bit(UnderflowBit); }
        auto setUnderflow(bool value) -> void { data.bit(UnderflowBit) = value; }

        auto overflow() const -> bool { return data.bit(OverflowBit); }
        auto setOverflow(bool value) -> void { data.bit(OverflowBit) = value; }

        auto divisionByZero() const -> bool { return data.bit(DivisionByZeroBit); }
        auto setDivisionByZero(bool value) -> void { data.bit(DivisionByZeroBit) = value; }

        auto invalidOperation() const -> bool { return data.bit(InvalidOperationBit); }
        auto setInvalidOperation(bool value) -> void { data.bit(InvalidOperationBit) = value; }

        auto unimplementedOperation() const -> bool {
          if constexpr(HasUnimplemented) return data.bit(UnimplementedOperationBit);
          return 0;
        }
        auto setUnimplementedOperation(bool value) -> void {
          if constexpr(HasUnimplemented) data.bit(UnimplementedOperationBit) = value;
        }

        auto reset() -> void { data = 0; }
      };
      using Flag = ExceptionBits<false>;
      using Enable = ExceptionBits<false>;
      using Cause = ExceptionBits<true>;
      Flag flag;
      Enable enable;
      Cause cause;
      n1 compare = 0;
      n1 flushSubnormals = 0;
    } csr;
  } fpu;

  //interpreter-fpu.cpp
  float_env fenv;

  template<typename T> auto fgr_t(u32) -> T&;
  template<typename T> auto fgr_s(u32) -> T&;
  template<typename T> auto fgr_d(u32) -> T&;
  auto getControlRegisterFPU(n5) -> u32;
  auto setControlRegisterFPU(n5, n32) -> void;
  template<bool CVT> auto checkFPUExceptions() -> bool;
  auto fpeDivisionByZero() -> bool;
  auto fpeInexact() -> bool;
  auto fpeUnderflow() -> bool;
  auto fpeOverflow() -> bool;
  auto fpeInvalidOperation() -> bool;
  auto fpeUnimplemented() -> bool;
  auto fpuCheckStart() -> bool;
  template <typename T>
  auto fpuCheckInput(T& f) -> bool;
  template <typename T>
  auto fpuCheckInputs(T& f1, T& f2) -> bool;
  auto fpuCheckOutput(f32& f) -> bool;
  auto fpuCheckOutput(f64& f) -> bool;
  template<typename DST, typename SF>
  auto fpuCheckInputConv(SF& f) -> bool;

  auto BC1(bool value, bool likely, s16 imm) -> void;
  auto CFC1(r64& rt, u8 rd) -> void;
  auto CTC1(cr64& rt, u8 rd) -> void;
  auto DCFC1(r64& rt, u8 rd) -> void;
  auto DCTC1(cr64& rt, u8 rd) -> void;
  auto DMFC1(r64& rt, u8 fs) -> void;
  auto DMTC1(cr64& rt, u8 fs) -> void;
  auto FABS_S(u8 fd, u8 fs) -> void;
  auto FABS_D(u8 fd, u8 fs) -> void;
  auto FADD_S(u8 fd, u8 fs, u8 ft) -> void;
  auto FADD_D(u8 fd, u8 fs, u8 ft) -> void;
  auto FCEIL_L_S(u8 fd, u8 fs) -> void;
  auto FCEIL_L_D(u8 fd, u8 fs) -> void;
  auto FCEIL_L_W(u8 fd, u8 fs) -> void;
  auto FCEIL_L_L(u8 fd, u8 fs) -> void;
  auto FCEIL_W_S(u8 fd, u8 fs) -> void;
  auto FCEIL_W_D(u8 fd, u8 fs) -> void;
  auto FCEIL_W_W(u8 fd, u8 fs) -> void;
  auto FCEIL_W_L(u8 fd, u8 fs) -> void;
  auto FC_EQ_S(u8 fs, u8 ft) -> void;
  auto FC_EQ_D(u8 fs, u8 ft) -> void;
  auto FC_F_S(u8 fs, u8 ft) -> void;
  auto FC_F_D(u8 fs, u8 ft) -> void;
  auto FC_LE_S(u8 fs, u8 ft) -> void;
  auto FC_LE_D(u8 fs, u8 ft) -> void;
  auto FC_LT_S(u8 fs, u8 ft) -> void;
  auto FC_LT_D(u8 fs, u8 ft) -> void;
  auto FC_NGE_S(u8 fs, u8 ft) -> void;
  auto FC_NGE_D(u8 fs, u8 ft) -> void;
  auto FC_NGL_S(u8 fs, u8 ft) -> void;
  auto FC_NGL_D(u8 fs, u8 ft) -> void;
  auto FC_NGLE_S(u8 fs, u8 ft) -> void;
  auto FC_NGLE_D(u8 fs, u8 ft) -> void;
  auto FC_NGT_S(u8 fs, u8 ft) -> void;
  auto FC_NGT_D(u8 fs, u8 ft) -> void;
  auto FC_OLE_S(u8 fs, u8 ft) -> void;
  auto FC_OLE_D(u8 fs, u8 ft) -> void;
  auto FC_OLT_S(u8 fs, u8 ft) -> void;
  auto FC_OLT_D(u8 fs, u8 ft) -> void;
  auto FC_SEQ_S(u8 fs, u8 ft) -> void;
  auto FC_SEQ_D(u8 fs, u8 ft) -> void;
  auto FC_SF_S(u8 fs, u8 ft) -> void;
  auto FC_SF_D(u8 fs, u8 ft) -> void;
  auto FC_UEQ_S(u8 fs, u8 ft) -> void;
  auto FC_UEQ_D(u8 fs, u8 ft) -> void;
  auto FC_ULE_S(u8 fs, u8 ft) -> void;
  auto FC_ULE_D(u8 fs, u8 ft) -> void;
  auto FC_ULT_S(u8 fs, u8 ft) -> void;
  auto FC_ULT_D(u8 fs, u8 ft) -> void;
  auto FC_UN_S(u8 fs, u8 ft) -> void;
  auto FC_UN_D(u8 fs, u8 ft) -> void;
  auto FCVT_S_S(u8 fd, u8 fs) -> void;
  auto FCVT_S_D(u8 fd, u8 fs) -> void;
  auto FCVT_S_W(u8 fd, u8 fs) -> void;
  auto FCVT_S_L(u8 fd, u8 fs) -> void;
  auto FCVT_D_S(u8 fd, u8 fs) -> void;
  auto FCVT_D_D(u8 fd, u8 fs) -> void;
  auto FCVT_D_W(u8 fd, u8 fs) -> void;
  auto FCVT_D_L(u8 fd, u8 fs) -> void;
  auto FCVT_L_S(u8 fd, u8 fs) -> void;
  auto FCVT_L_D(u8 fd, u8 fs) -> void;
  auto FCVT_L_W(u8 fd, u8 fs) -> void;
  auto FCVT_L_L(u8 fd, u8 fs) -> void;
  auto FCVT_W_S(u8 fd, u8 fs) -> void;
  auto FCVT_W_D(u8 fd, u8 fs) -> void;
  auto FCVT_W_W(u8 fd, u8 fs) -> void;
  auto FCVT_W_L(u8 fd, u8 fs) -> void;
  auto FDIV_S(u8 fd, u8 fs, u8 ft) -> void;
  auto FDIV_D(u8 fd, u8 fs, u8 ft) -> void;
  auto FFLOOR_L_S(u8 fd, u8 fs) -> void;
  auto FFLOOR_L_D(u8 fd, u8 fs) -> void;
  auto FFLOOR_L_W(u8 fd, u8 fs) -> void;
  auto FFLOOR_L_L(u8 fd, u8 fs) -> void;
  auto FFLOOR_W_S(u8 fd, u8 fs) -> void;
  auto FFLOOR_W_D(u8 fd, u8 fs) -> void;
  auto FFLOOR_W_W(u8 fd, u8 fs) -> void;
  auto FFLOOR_W_L(u8 fd, u8 fs) -> void;
  auto FMOV_S(u8 fd, u8 fs) -> void;
  auto FMOV_D(u8 fd, u8 fs) -> void;
  auto FMUL_S(u8 fd, u8 fs, u8 ft) -> void;
  auto FMUL_D(u8 fd, u8 fs, u8 ft) -> void;
  auto FNEG_S(u8 fd, u8 fs) -> void;
  auto FNEG_D(u8 fd, u8 fs) -> void;
  auto FROUND_L_S(u8 fd, u8 fs) -> void;
  auto FROUND_L_D(u8 fd, u8 fs) -> void;
  auto FROUND_L_W(u8 fd, u8 fs) -> void;
  auto FROUND_L_L(u8 fd, u8 fs) -> void;
  auto FROUND_W_S(u8 fd, u8 fs) -> void;
  auto FROUND_W_D(u8 fd, u8 fs) -> void;
  auto FROUND_W_W(u8 fd, u8 fs) -> void;
  auto FROUND_W_L(u8 fd, u8 fs) -> void;
  auto FSQRT_S(u8 fd, u8 fs) -> void;
  auto FSQRT_D(u8 fd, u8 fs) -> void;
  auto FSUB_S(u8 fd, u8 fs, u8 ft) -> void;
  auto FSUB_D(u8 fd, u8 fs, u8 ft) -> void;
  auto FTRUNC_L_S(u8 fd, u8 fs) -> void;
  auto FTRUNC_L_D(u8 fd, u8 fs) -> void;
  auto FTRUNC_L_W(u8 fd, u8 fs) -> void;
  auto FTRUNC_L_L(u8 fd, u8 fs) -> void;
  auto FTRUNC_W_S(u8 fd, u8 fs) -> void;
  auto FTRUNC_W_D(u8 fd, u8 fs) -> void;
  auto FTRUNC_W_W(u8 fd, u8 fs) -> void;
  auto FTRUNC_W_L(u8 fd, u8 fs) -> void;
  auto LDC1(u8 ft, cr64& rs, s16 imm) -> void;
  auto LWC1(u8 ft, cr64& rs, s16 imm) -> void;
  auto MFC1(r64& rt, u8 fs) -> void;
  auto MTC1(cr64& rt, u8 fs) -> void;
  auto SDC1(u8 ft, cr64& rs, s16 imm) -> void;
  auto SWC1(u8 ft, cr64& rs, s16 imm) -> void;
  auto COP1UNIMPLEMENTED() -> void;

  //interpreter-cop2.cpp
  struct COP2 {
    u64 latch;
  } cop2;

  auto MFC2(r64& rt, u8 rd) -> void;
  auto DMFC2(r64& rt, u8 rd) -> void;
  auto CFC2(r64& rt, u8 rd) -> void;
  auto MTC2(cr64& rt, u8 rd) -> void;
  auto DMTC2(cr64& rt, u8 rd) -> void;
  auto CTC2(cr64& rt, u8 rd) -> void;
  auto COP2INVALID() -> void;

  //decoder.cpp
  auto decoderEXECUTE(u32 instruction) -> void;
  auto decoderSPECIAL(u32 instruction) -> void;
  auto decoderREGIMM(u32 instruction) -> void;
  auto decoderSCC(u32 instruction) -> void;
  auto decoderFPU(u32 instruction) -> void;
  auto decoderCOP2(u32 instruction) -> void;
  auto decoderEXECUTEInfo(u32 instruction) const -> OpInfo;
  auto decoderSPECIALInfo(u32 instruction) const -> OpInfo;
  auto decoderREGIMMInfo(u32 instruction) const -> OpInfo;
  auto decoderSCCInfo(u32 instruction) const -> OpInfo;
  auto decoderFPUInfo(u32 instruction) const -> OpInfo;
  auto decoderCOP2Info(u32 instruction) const -> OpInfo;

  auto COP3() -> void;
  auto INVALID() -> void;

  //recompiler.cpp, recompiler-fpu.cpp, recompiler-ipu.cpp
  struct Recompiler : recompiler::generic {
    CPU& self;
    Recompiler(CPU& self) : self(self), generic(allocator) {
      slowPaths.reserve(128);
    }

    enum : u32 {
      SectionSize  = 4_KiB,
      SectionShift = 12,
      SectionMask  = SectionSize - 1,
      SectionLineSize = 32,
      SectionLineShift = 5,
      SectionLineCount = SectionSize / SectionLineSize,
      SectionWords = SectionSize / sizeof(u32),
      RdramSize    = 8_MiB,
      RdramMask    = RdramSize - 1,
      SectionCount = RdramSize / SectionSize,
    };

    struct StateKey {
      StateKey() = default;
      StateKey(u64 data) : data(data) {}

      operator u64() const { return data; }

      auto coprocessor1Enabled() const -> bool { return data.bit(0); }
      auto setCoprocessor1Enabled(bool value) -> void { data.bit(0) = value; }

      auto floatingPointMode() const -> bool { return data.bit(1); }
      auto setFloatingPointMode(bool value) -> void { data.bit(1) = value; }

      auto exceptionLevel() const -> bool { return data.bit(2); }
      auto setExceptionLevel(bool value) -> void { data.bit(2) = value; }

      auto errorLevel() const -> bool { return data.bit(3); }
      auto setErrorLevel(bool value) -> void { data.bit(3) = value; }

      auto privilegeMode() const -> u32 { return data.bit(4, 5); }
      auto setPrivilegeMode(u32 value) -> void { data.bit(4, 5) = value; }

      auto userExtendedAddressing() const -> bool { return data.bit(6); }
      auto setUserExtendedAddressing(bool value) -> void { data.bit(6) = value; }

      auto supervisorExtendedAddressing() const -> bool { return data.bit(7); }
      auto setSupervisorExtendedAddressing(bool value) -> void { data.bit(7) = value; }

      auto kernelExtendedAddressing() const -> bool { return data.bit(8); }
      auto setKernelExtendedAddressing(bool value) -> void { data.bit(8) = value; }

      auto reverseEndian() const -> bool { return data.bit(9); }
      auto setReverseEndian(bool value) -> void { data.bit(9) = value; }

      auto coprocessor0Enabled() const -> bool { return data.bit(10); }
      auto setCoprocessor0Enabled(bool value) -> void { data.bit(10) = value; }

      auto fpuRoundMode() const -> u32 { return data.bit(11, 12); }
      auto setFpuRoundMode(u32 value) -> void { data.bit(11, 12) = value; }

      auto fpuFlushSubnormals() const -> bool { return data.bit(13); }
      auto setFpuFlushSubnormals(bool value) -> void { data.bit(13) = value; }

      auto fpuInexactEnabled() const -> bool { return data.bit(14); }
      auto setFpuInexactEnabled(bool value) -> void { data.bit(14) = value; }

      auto fpuUnderflowEnabled() const -> bool { return data.bit(15); }
      auto setFpuUnderflowEnabled(bool value) -> void { data.bit(15) = value; }

      auto fpuOverflowEnabled() const -> bool { return data.bit(16); }
      auto setFpuOverflowEnabled(bool value) -> void { data.bit(16) = value; }

      auto fpuDivisionByZeroEnabled() const -> bool { return data.bit(17); }
      auto setFpuDivisionByZeroEnabled(bool value) -> void { data.bit(17) = value; }

      auto fpuInvalidOperationEnabled() const -> bool { return data.bit(18); }
      auto setFpuInvalidOperationEnabled(bool value) -> void { data.bit(18) = value; }

      auto gpCachedRdram() const -> bool { return data.bit(19); }
      auto setGpCachedRdram(bool value) -> void { data.bit(19) = value; }

      auto gpCachedRdramOff16() const -> bool { return data.bit(20); }
      auto setGpCachedRdramOff16(bool value) -> void { data.bit(20) = value; }

      auto gpAligned4() const -> bool { return data.bit(21); }
      auto setGpAligned4(bool value) -> void { data.bit(21) = value; }

      auto gpAligned8() const -> bool { return data.bit(22); }
      auto setGpAligned8(bool value) -> void { data.bit(22) = value; }

      auto spAligned4() const -> bool { return data.bit(23); }
      auto setSpAligned4(bool value) -> void { data.bit(23) = value; }

      auto spAligned8() const -> bool { return data.bit(24); }
      auto setSpAligned8(bool value) -> void { data.bit(24) = value; }

      auto watchpointsActive() const -> bool { return data.bit(25); }
      auto setWatchpointsActive(bool value) -> void { data.bit(25) = value; }

      auto rdramMapIdentity() const -> bool { return data.bit(26); }
      auto setRdramMapIdentity(bool value) -> void { data.bit(26) = value; }

      n64 data = 0;
    };

    struct Block {
      auto execute(CPU& self) -> void {
        self.recompiler.activeBlock = this;
        ((void (*)(CPU*, r64*, r64*))code)(&self, &self.ipu.r[16], &self.fpu.r[16]);
      }

      u8* code = nullptr;
      Block* next = nullptr;
      u64 stateKey = 0;
      u64 vaddrPage = 0;
      u32 startAddress = 0;
      u32 endAddress = 0;
      u8* sectionDirty = nullptr;
    };

    struct Section {
      Block* blocks[SectionWords];
      u8 lineBlocks[SectionLineCount];
    };

    struct SlowPath {
      std::vector<sljit_jump*> enters;
      sljit_label* resume = nullptr;
      u32 instruction = 0;
      u64 vaddr = 0;
      u32 deferredCycles = 0;
      u32 instructionCycles = 0;
      bool jumpEpilog = false;
      bool icacheMiss = false;
      bool runtimePc = false;
      u32 icachePaddr = 0;
    };

    enum class EmitPcMode : bool { JitTime, Runtime };
    enum class EmitExecuteResult : u8 { Linear, MayBranch, MayFault };

    auto reset() -> void {
      sections.resize(SectionCount);
      sectionDirty.resize(SectionCount);
      std::ranges::fill(sections, nullptr);
      std::ranges::fill(sectionDirty, 0);
      activeBlock = nullptr;
    }

    auto isRdramAddress(u32 address) const -> bool {
      return address < rdram.ram.size;
    }

    auto rdramAddress(u32 address) const -> u32 {
      return address & RdramMask;
    }

    auto sectionIndex(u32 address) const -> u32 {
      return rdramAddress(address) >> SectionShift;
    }

    auto sectionOffset(u32 address) const -> u32 {
      return rdramAddress(address) & SectionMask;
    }

    auto blockIndex(u32 address) const -> u32 {
      return sectionOffset(address) >> 2;
    }

    auto sectionLineIndex(u32 address) const -> u32 {
      return sectionOffset(address) >> SectionLineShift;
    }

    auto invalidate(u32 address) -> void {
      invalidateSection(address);
    }

    auto invalidateSection(u32 address) -> void {
      if(!isRdramAddress(address)) return;
      auto index = sectionIndex(address);
      auto section = sections[index];
      if(!section) return;
      if(!section->lineBlocks[sectionLineIndex(address)]) return;
      sectionDirty[index] = 1;
      // If the code is modifying the current block, we need to end it, as we
      // have recompiled the previous version of the code.
      if(activeBlock && activeBlock->sectionDirty == &sectionDirty[index]) {
        self.pipeline.state |= Pipeline::EndBlock;
      }
    }

    auto invalidateRange(u32 address, u32 length) -> void {
      if(!length) return;
      u64 start = address;
      u64 end = start + length - 1;
      if(start >= RdramSize) return;
      if(end >= RdramSize) end = RdramSize - 1;
      u32 firstSection = u32(start >> SectionShift);
      u32 lastSection  = u32(end >> SectionShift);
      for(u32 sidx = firstSection; sidx <= lastSection; sidx++) {
        if(sectionDirty[sidx]) {
          if(activeBlock && activeBlock->sectionDirty == &sectionDirty[sidx]) {
            self.pipeline.state |= Pipeline::EndBlock;
          }
          continue;
        }
        auto section = sections[sidx];
        if(!section) continue;
        u32 firstLine = 0;
        u32 lastLine  = SectionLineCount - 1;
        if(sidx == firstSection) firstLine = u32((start & SectionMask) >> SectionLineShift);
        if(sidx == lastSection)  lastLine  = u32((end   & SectionMask) >> SectionLineShift);
        for(u32 line = firstLine; line <= lastLine; line++) {
          if(section->lineBlocks[line]) {
            sectionDirty[sidx] = 1;
            if(activeBlock && activeBlock->sectionDirty == &sectionDirty[sidx]) {
              self.pipeline.state |= Pipeline::EndBlock;
            }
            break;
          }
        }
      }
    }

    auto computeStateKey() const -> u64;
    auto reservedInstruction64() const -> bool;
    auto updateStackPointerStateKey(s16 offset) -> void;
    auto section(u32 address) -> Section*;
    auto block(u64 vaddr, u32 address) -> Block*;

    auto flushDeferredCycles() -> void;
    auto setupPipeline() -> void;
    auto setupCallf() -> void;
    auto emitCpuStep(u32 clocks) -> void;
    auto deferSlowPath(sljit_jump* enter, u32 instruction) -> void;
    auto deferSlowPath(std::initializer_list<sljit_jump*> enters, u32 instruction) -> void;
    auto deferSlowPathCacheMiss(sljit_jump* enter, u32 paddr) -> void;
    auto emit(u64 vaddr, u32 address, u64 stateKey) -> Block*;
    auto emitZeroClear(u32 n) -> void;
    enum JitMemoryOpcodeMode : u32 {
      SignExtend = 1 << 0,
      Require64  = 1 << 1,
      Store      = 1 << 2,
      PartialLeft = 1 << 3,
      PartialRight = 1 << 4,
      Floating   = 1 << 5,
      LinkedConditional = 1 << 6,
    };

    auto jitMemoryOpcode(u32 instruction, u32 size, u32 mode,
      const std::function<EmitExecuteResult()>& fallback, bool emitSlowPath) -> EmitExecuteResult;
    auto emitEXECUTE(u32 instruction, bool emitSlowPath, EmitPcMode pcMode) -> EmitExecuteResult;
    auto emitSPECIAL(u32 instruction) -> EmitExecuteResult;
    auto emitREGIMM(u32 instruction, EmitPcMode pcMode) -> EmitExecuteResult;
    auto emitSCC(u32 instruction, EmitPcMode pcMode) -> EmitExecuteResult;
    auto emitFPU(u32 instruction, EmitPcMode pcMode) -> EmitExecuteResult;
    auto emitCOP2(u32 instruction) -> EmitExecuteResult;

    bool enabled = false;
    bool callInstructionPrologue = false;
    bool emitSlowPathSection = false;
    bool emitPipelineSetupDone = false;
    bool emitCallfSetupDone = false;
    bool emitCallfEmitted = false;
    bool emitStateKeyChanged = false;
    bool emitAllocatorFlushed = false;
    EmitPcMode emitPcMode = EmitPcMode::JitTime;
    StateKey emitStateKey = 0;
    u64 emitVaddr = 0;
    u32 emitDeferredCycles = 0;
    u32 emitFpuFastMxcsr = 0;
    u32 emitFpuSaveMxcsr = 0;
    Block* activeBlock = nullptr;
    bump_allocator allocator;
    std::vector<u32> emitAliasAddresses;
    std::vector<SlowPath> slowPaths;
    std::vector<Section*> sections;
    std::vector<u8> sectionDirty;
  } recompiler{*this};
  s64 jitClockTarget = 0;
  s64 countClock = 0;

  struct Disassembler {
    CPU& self;
    Disassembler(CPU& self) : self(self) {}

    //disassembler.cpp
    auto disassemble(u32 address, u32 instruction) -> string;
    template<typename... P> auto hint(P&&... p) const -> string;

    bool showColors = true;
    bool showValues = true;

  private:
    auto EXECUTE() -> std::vector<string>;
    auto SPECIAL() -> std::vector<string>;
    auto REGIMM() -> std::vector<string>;
    auto SCC() -> std::vector<string>;
    auto FPU() -> std::vector<string>;
    auto immediate(s64 value, u32 bits = 0) const -> string;
    auto ipuRegisterName(u32 index) const -> string;
    auto ipuRegisterValue(u32 index) const -> string;
    auto ipuRegisterIndex(u32 index, s16 offset) const -> string;
    auto sccRegisterName(u32 index) const -> string;
    auto sccRegisterValue(u32 index) const -> string;
    auto fpuRegisterName(u32 index) const -> string;
    auto fpuRegisterValue(u32 index) const -> string;
    auto ccrRegisterName(u32 index) const -> string;
    auto ccrRegisterValue(u32 index) const -> string;

    u32 address;
    u32 instruction;
  } disassembler{*this};

  struct DevirtualizeCache {
    uint64_t vbase;
    uint64_t pbase;
  } devirtualizeCache;

  //emux.cpp
  union Profile {
    struct {
      s64 cpuCycles;
      s64 cpuCyclesExc;
      s64 icacheHits, icacheMisses, icacheWritebacks;
      s64 dcacheHits, dcacheMisses, dcacheWritebacks;
    };
    s64 data[8];
    Profile() : data{0} {}
  } profile;

  struct ProfileSlot {
    Profile cpu;
    struct {
      s64 cycles;
      s64 haltedCycles;
    } rsp;
    RDRAM::Profile rdram;
    n1 started = 0;

    static auto global() -> ProfileSlot;
  };

  std::vector<ProfileSlot> profileSlots;

  //profiler.cpp — in-game CPU cost profiler. Instruments calls (JAL/JALR) and
  //returns (JR $ra) while "enabled" to build per-function call counts + cycle
  //costs. Written on the emulation thread, read live by the desktop-ui viewer.
  struct Profiler {
    struct FuncStat {
      u32 addr = 0;
      u64 callCount = 0;
      u64 inclCycles = 0;  //cycles between entry and matching return (incl children)
      u64 exclCycles = 0;  //inclCycles minus time spent in callees
      u64 waitCycles = 0;  //exclusive time spent in spin/wait functions anywhere
                           //in this function's subtree (direct + indirect)
      u64 inclBytesIn = 0;   //RDRAM -> CPU bytes incl. callees
      u64 inclBytesOut = 0;  //CPU -> RDRAM bytes incl. callees
      u64 exclBytesIn = 0;   //RDRAM -> CPU bytes from this function's own code
      u64 exclBytesOut = 0;  //CPU -> RDRAM bytes from this function's own code
      u64 inclCacheBytes[3] = {};  //cache line bytes transferred incl. callees, per CacheKind
      u64 exclCacheBytes[3] = {};  //cache line bytes transferred while this function was on top
      bool isSpin = false;
    };
    struct Sym {
      u32 addr = 0;
      u32 size = 0;
      string name;
      bool isSpin = false;
    };
    struct Frame {
      u32 funcAddr = 0;
      u32 retAddr = 0;      //expected $ra on return (call address + 8); 0 for exception frames
      u32 sp = 0;           //caller's $sp at the call site; used to unwind across OS thread switches
      u64 entryCycle = 0;
      u64 segmentCycle = 0;  //start of the unaccounted portion of this call
      bool newCall = true;
      bool markerPending = false;
      bool markerRoot = false;
      u64 childCycles = 0;  //inclusive time of callees that have returned
      u64 childWait = 0;    //subtree wait time of callees that have returned
      u64 bytesInOwn = 0;    //RDRAM -> CPU bytes while this frame was on top (exclusive)
      u64 bytesOutOwn = 0;   //CPU -> RDRAM bytes while this frame was on top (exclusive)
      u64 childBytesIn = 0;  //inclusive RDRAM -> CPU bytes of callees that have returned
      u64 childBytesOut = 0; //inclusive CPU -> RDRAM bytes of callees that have returned
      u64 cacheOwn[3] = {};    //cache bytes per CacheKind while this frame was on top (exclusive)
      u64 childCache[3] = {};  //inclusive cache bytes per CacheKind of callees that have returned
      bool isException = false;  //synthetic frame for an exception/interrupt handler
    };

    // Flame-chart timeline: one span per completed call, recorded at popFrame time
    // when timeline recording is on. start/end are absolute master-clock ticks
    // (now()); depth is the call-stack nesting level (0 = outermost tracked call).
    struct Span {
      u64 start = 0;
      u64 end = 0;
      u32 funcAddr = 0;
      u16 depth = 0;
      bool isException = false;
    };

    // Synthetic "function" addresses for exception/interrupt handler entries, so
    // their time is attributed separately (and subtracted from the interrupted
    // function) rather than counted as that function's own work.
    static constexpr u32 kExcBase = 0xE000'0000;
    auto isExceptionAddr(u32 addr) const -> bool { return (addr & 0xFFFF'0000) == kExcBase; }
    auto exceptionEntryAddr(u32 code) -> u32;       //synthetic addr for current exception
    auto labelFor(u32 addr) -> string;              //display name (symbol / exception / hex)

    std::atomic<bool> enabled{false};

    std::unordered_map<u32, FuncStat> stats;       //continuous totals
    std::unordered_map<u32, FuncStat> swapTotals; //totals of completed captured swap windows
    std::unordered_map<u32, FuncStat> frameStats;  //last fully-completed frame
    std::unordered_map<u32, FuncStat> frameAccum;  //frame in progress
    std::unordered_map<u32, FuncStat> markerStats; //last completed function window
    std::unordered_map<u32, FuncStat> markerAccum;
    u32 markerAddr = 0;
    bool markerActive = false;
    bool markerReady = false;
    std::vector<Frame> callStack;        //the active thread's call stack (see Suspended)
    u64 frameCount = 0;  //presented frames accumulated into stats since last clear

    // Multi-stack tracking / thread handling. $sp says which stack we're on. callStack holds the running thread.
    // the others are parked here keyed by the $sp region they occupy, and swapped in when $sp jumps to their region.
    struct Suspended {
      u32 lo = 0, hi = 0;          //observed $sp extent of this parked stack
      u64 used = 0;                //LRU stamp (stackClock at the time it was parked)
      std::vector<Frame> frames;
    };
    std::vector<Suspended> suspended;
    u32  spLo = 0, spHi = 0;       //observed $sp extent of the active stack
    bool haveSp = false;           //active region initialized
    u32  excActive = 0;            //exception/interrupt handlers open on the active stack
    u64  stackClock = 0;           //monotonic LRU counter for parked stacks

    static constexpr u32 regionSlack = 0x8000;
    static constexpr u32 maxStacks = 16;  //parked stacks retained (LRU beyond this)

    // Flame-chart timeline: a sliding-window ring of completed-call spans in
    // absolute master-clock time (now()). The emu thread appends one span per
    // popFrame() (single producer)
    std::atomic<bool> recordTimeline{false};
    static constexpr u32 maxSpans = 1u << 18;  //ring capacity (262144 spans)
    std::vector<Span> timeline;          //ring buffer, sized to maxSpans when enabled
    std::atomic<u64> timelineWrite{0};   //monotonic append count

    // Still-open calls, published for the flame chart. A Span only reaches the ring
    // when its call *returns*, so a function that is still running has no entry and
    // would be drawn as a gap rather than a bar. This mirrors the live callStack into
    // a fixed POD array the UI thread can sample: callStack itself is a std::vector the
    // emu thread reallocates, so reading it cross-thread is not safe. Slots are filled
    // before openDepth is published with release, so a reader that acquires openDepth
    // sees initialized entries; a pop+push racing the read can still hand it one stale
    // frame, which is cosmetic for a live view (the UI clamps implausible starts).
    struct OpenFrame {
      u64 start = 0;       //absolute now() tick at which the call was entered
      u32 funcAddr = 0;
      bool isException = false;
    };
    static constexpr u32 maxOpenFrames = 128;  //deeper frames are off-chart anyway
    OpenFrame openFrames[maxOpenFrames] = {};
    std::atomic<u32> openDepth{0};
    auto syncOpenFrames() -> void;  //re-publish from callStack; call after any mutation
    auto power() -> void;           //machine reset / new game: drop all captured state

    // VI framebuffer-swap markers for the flame chart, in absolute now() ticks.
    // Appended in onFrame() (called at each presented swap); the UI draws the ones
    // that fall within its window as vertical "VI" lines.
    static constexpr u32 maxViMarks = 256;
    u64 viMarks[maxViMarks] = {};
    std::atomic<u64> viMarkWrite{0};

    // Cache line transfers
    enum CacheKind : u8 {
      CacheIFill  = 0,  //icache line fill: 8 words RDRAM -> CPU
      CacheDFill  = 1,  //dcache line fill: 4 words RDRAM -> CPU
      CacheDWrite = 2,  //dcache write-back: 4 words CPU -> RDRAM
      CacheKinds  = 3,
    };
    static constexpr u32 cacheBytes[CacheKinds] = {32, 16, 16};
    //now() ticks per transfer, matching the step() in the Line::fill/writeBack
    //implementations: icache 48 CPU cycles, dcache 40 CPU cycles
    static constexpr u32 cacheTicks[CacheKinds] = {96, 80, 80};
    //transfer time for a byte count of one kind (each line is one fixed-cost transfer)
    static constexpr auto cacheTime(u32 kind, u64 bytes) -> u64 { return bytes / cacheBytes[kind] * cacheTicks[kind]; }
    struct CacheEvent {
      u64 time = 0;      //absolute now() tick at which the transfer started
      u32 paddr = 0;     //physical address of the cache line
      u32 funcAddr = 0;  //function on top of the call stack (0 = none tracked)
      u32 causePc = 0;   //PC of the instruction that triggered the transfer
      u8  kind = CacheIFill;
      bool isException = false;
      //Line utilisation, filled in when the line is evicted (a write-back is
      //complete immediately). Until then evictTime is 0 ("still cached").
      //Bit n = byte n of a dcache line; for an icache line the low 8 bits are
      //its words. readMask holds bytes read / words executed, writeMask bytes
      //written (the line's dirty mask).
      u16 readMask = 0;
      u16 writeMask = 0;
      u64 evictTime = 0;
      u32 evictorPc = 0;       //instruction whose access replaced the line
      u32 evictorFuncAddr = 0; //function it ran in
      //The line's content: the 8 words an icache fill fetched (its instructions),
      //or the 4 words a dcache line held when it was evicted / written back.
      u32 words[8] = {};
      u8  wordCount = 0;
      //Reuse distance of a fill: how many fills into this cache happened since
      //the same line was last evicted (0 = never evicted since profiling
      //started, i.e. a first touch). Fewer than the cache's line count means a
      //better-mapped cache of the same size would still have held it — a
      //conflict miss, which layout can fix; more means capacity.
      u32 reuseFills = 0;
      u64 reuseTime = 0;         //ticks since that eviction
      u32 prevEvictorPc = 0;     //what pushed the line out back then
      u32 prevEvictorFuncAddr = 0;
    };
    static constexpr u32 cacheLines = 512;  //both caches: reuse distance below this = conflict
    //Per RDRAM line, when it was last evicted from its cache and by whom, for
    //the reuse distance of the next fill. Indexed by paddr / line size; sized
    //for the 8 MB RDRAM when the profiler is enabled (profiling only).
    struct LineHistory {
      u64 fillSeq = 0;   //fill counter value at eviction (0 = never)
      u64 time = 0;
      u32 evictorPc = 0;
      u32 evictorFuncAddr = 0;
    };
    std::vector<LineHistory> lineHistory[2];  //[0] icache (32 B lines), [1] dcache (16 B lines)
    u64 fillSeq[2] = {};                       //fills into each cache so far
    static constexpr u32 maxCacheEvents = 1u << 18;
    std::vector<CacheEvent> cacheEvents;  //ring buffer, sized to maxCacheEvents when enabled
    std::atomic<u64> cacheEventWrite{0};

    std::vector<Sym> syms;                 //sorted by addr, for enclosing lookup
    std::unordered_map<u32, u32> symByAddr;//exact entry addr -> index into syms
    bool symbolsLoaded = false;
    u32  symbolCount = 0;

    static constexpr u32 maxStackDepth = 1024;
    static constexpr u32 maxFrames = 1000;  //cap continuous accumulation window

    auto loadSymbols(const string& romPath) -> bool;
    auto resolve(u32 addr) -> Sym*;
    auto refreshWaitFunctions() -> void;
    auto setFunctionMarker(u32 addr) -> void;
    auto commitFrame(Frame& frame, Frame* parent, u64 time) -> void;
    auto flushFrames() -> void;
    auto onInstruction(u64 address, u32 instruction) -> void;
    //memory-bus access committed to RDRAM, attributed to the current frame.
    //toRDRAM=true is outgoing (CPU -> RDRAM), false is incoming (RDRAM -> CPU).
    auto onBusAccess(bool toRDRAM, u64 bytes) -> void;
    auto onCacheTouch(u64 address, u32 instruction) -> void;  //per instruction: mark cache line usage
    auto onCacheFill(u8 kind, u32 paddr) -> u64;  //cache line transfer: attribute + record; returns event id
    auto onCacheContent(u64 eventId, const u32* words, u32 count) -> void;  //attach the line's words
    auto onCacheEvict(u64 eventId, u16 readMask, u16 writeMask) -> void;  //close the event
    //Load whose line was not resident at the prologue: its bytes are applied to
    //the line once the fill has brought it in (see onInstruction / Line::fill).
    //Keyed by the line's physical address (a Line's `index` alone does not
    //identify it: it only holds the offset within the tag's 4 KB region).
    u32 pendingTouchAddr = ~0u;
    u16 pendingTouchMask = 0;
    auto takePendingTouch(u32 lineAddr) -> u16 {
      if(lineAddr != pendingTouchAddr) return 0;
      pendingTouchAddr = ~0u;
      return pendingTouchMask;
    }
    //same for the icache: the word of an instruction whose line was not resident
    //at the prologue (the fill follows right after)
    u32 pendingExecAddr = ~0u;
    u8  pendingExecMask = 0;
    auto takePendingExec(u32 lineAddr) -> u8 {
      if(lineAddr != pendingExecAddr) return 0;
      pendingExecAddr = ~0u;
      return pendingExecMask;
    }
    auto onException(u32 code) -> void;  //exception/interrupt entry: push handler frame
    auto onEret() -> void;               //exception return: pop handler frame
    auto popFrame() -> bool;             //record top frame; returns isException
    auto switchStack(u32 sp) -> void;    //park the active call stack and resume/create the one $sp belongs to
    auto onFrame() -> void;       //per framebuffer swap: publish frame snapshot
    auto setEnabled(bool value) -> void;
    auto clearStats() -> void;
    auto now() -> u64;   //master-clock wall timebase (the CPU is primary, runs continuously)
  } profiler;

  //exectrace.cpp — CPU execution trace for offline instruction-cache analysis.
  //While active, writes executed KSEG0 PC ranges, icache line fills, frame marks
  //and resets to a binary file. The stream does not depend on the code layout:
  //an external cache simulator can replay it against a relinked executable.
  //Emulation thread only.
  struct ExecTrace {
    enum : u32 {
      RecordRange    = 0,   //a = first pc, b = end pc (exclusive), low 28 bits = repeat count
      RecordFrame    = 1,   //a = frame number, b = low 32 bits of CPU cycles
      RecordFill     = 2,   //a = physical address of the filled icache line
      RecordUncached = 3,   //a = instructions executed outside KSEG0
      RecordReset    = 4,   //machine power/reset: every icache line became invalid
      RecordTotals   = 15,  //written by stop(), subtype in the low bits (Total*)
    };
    enum : u32 {
      TotalInstructions = 0,  //a/b = KSEG0 instructions (low/high)
      TotalFills        = 1,  //a/b = icache fills (low/high)
      TotalFlags        = 2,  //a bit 0 = truncated by maxBytes, b = frames
    };
    static constexpr u32 version = 1;
    static constexpr u32 headerBytes = 8 + 4 * 4 + 8 + 512 * 4;
    static constexpr u32 countMask = 0x0fff'ffff;
    static constexpr u32 bufferBytes = 1 << 20;

    struct Stats {
      u64 records = 0;
      u64 instructions = 0;  //KSEG0 (cached) instructions
      u64 uncached = 0;      //instructions outside KSEG0
      u64 fills = 0;         //icache line fills, exact for interpreter and recompiler
      u64 frames = 0;
      u64 bytes = 0;
      bool truncated = false;
      s64 icacheHits = 0;    //core counter deltas (the recompiler only maintains
      s64 icacheMisses = 0;  //them in homebrew mode)
    };

    auto active() const -> bool { return file != nullptr; }
    auto start(const string& path, u64 maxBytes) -> string;  //"" or an error message
    auto stop(Stats& out) -> string;
    auto onInstruction(u64 address) -> void;
    auto onIcacheFill(u32 lineAddress) -> void;
    auto onFrame(u64 frame) -> void;
    auto onPower() -> void;

    auto closeRun() -> void;
    auto flushPendingRange() -> void;
    auto flushUncached() -> void;
    auto flushAll() -> void;
    auto put(u32 a, u32 b, u32 typeAndCount) -> void;
    auto writeBuffer() -> void;

    std::FILE* file = nullptr;
    std::vector<u8> buffer;
    u64 maxBytes = 0;
    bool failed = false;
    Stats stats;
    s64 startHits = 0;
    s64 startMisses = 0;
    bool runOpen = false;     //sequential run being extended: [runStart, runEnd)
    u32 runStart = 0;
    u32 runEnd = 0;
    u32 pendingStart = 0;     //last completed run, held back to fold repeats
    u32 pendingEnd = 0;
    u32 pendingCount = 0;
    u32 uncachedRun = 0;      //uncached instructions not yet written
  } execTrace;

  struct EmuxState {
    n64 excMask;
  } emuxState;

  auto XDETECT(r64& rd, u64 code) -> void;
  auto XLOG(cr64& rd, cr64& rt, u64 code) -> void;
  auto XHEXDUMP(cr64& rd, cr64& rt) -> void;
  auto XPROF(cr64& rd, u64 code) -> void;
  auto XPROFREAD(cr64& rd, r64& rt) -> void;
  auto XEXCEPTION(r64& rt) -> void;
  auto XIOCTL(u64 code) -> void;
};

extern CPU cpu;
