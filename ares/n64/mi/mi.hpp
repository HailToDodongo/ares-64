//MIPS Interface

struct MI : Memory::RCP<MI> {
  Node::Object node;

  struct Debugger {
    //debugger.cpp
    auto load(Node::Object) -> void;
    auto interrupt(u8 source) -> void;
    auto io(bool mode, u32 address, u32 data) -> void;

    struct Tracer {
      Node::Debugger::Tracer::Notification interrupt;
      Node::Debugger::Tracer::Notification io;
    } tracer;
  } debugger;

  //mi.cpp
  auto load(Node::Object) -> void;
  auto unload() -> void;

  enum class IRQ : u32 { SP, SI, AI, VI, PI, DP };
  auto raise(IRQ) -> void;
  auto lower(IRQ) -> void;
  auto poll() -> void;

  //bitmask (by IRQ index) of RCP sub-sources currently asserting an interrupt;
  //used by the CPU profiler to label the active interrupt source.
  auto activeIRQs() const -> u32 {
    u32 m = 0;
    if(irq.sp.line & irq.sp.mask) m |= 1u << (u32)IRQ::SP;
    if(irq.si.line & irq.si.mask) m |= 1u << (u32)IRQ::SI;
    if(irq.ai.line & irq.ai.mask) m |= 1u << (u32)IRQ::AI;
    if(irq.vi.line & irq.vi.mask) m |= 1u << (u32)IRQ::VI;
    if(irq.pi.line & irq.pi.mask) m |= 1u << (u32)IRQ::PI;
    if(irq.dp.line & irq.dp.mask) m |= 1u << (u32)IRQ::DP;
    return m;
  }

  auto power(bool reset) -> void;

  //io.cpp
  auto readWord(u32 address, Thread& thread) -> u32;
  auto writeWord(u32 address, u32 data, Thread& thread) -> void;
  auto initializeMode() -> bool { bool m = io.initializeMode; io.initializeMode = 0; return m; }
  auto initializeLength() -> n7 { return io.initializeLength; }

  //serialization.cpp
  auto serialize(serializer&) -> void;

private:
  struct Interrupt {
    b1 line = 1;
    b1 mask;
  };

  struct IRQs {
    Interrupt sp;
    Interrupt si;
    Interrupt ai;
    Interrupt vi;
    Interrupt pi;
    Interrupt dp;
  } irq;

  struct IO {
    n7 initializeLength;
    n1 initializeMode;
    n1 ebusTestMode;
    n1 rdramRegisterSelect;
  } io;

  struct Revision {
    static constexpr u8 io  = 0x02;  //I/O interface
    static constexpr u8 rac = 0x01;  //RAMBUS ASIC cell
    static constexpr u8 rdp = 0x02;  //Reality Display Processor
    static constexpr u8 rsp = 0x02;  //Reality Signal Processor
  } revision;
};

extern MI mi;
