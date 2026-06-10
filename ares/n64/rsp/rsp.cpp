#include <n64/n64.hpp>
#include <nlohmann/json.hpp>
#include <nall/decode/elf.hpp>
#include <nall/decode/dwarf.hpp>

namespace ares::Nintendo64 {

RSP rsp;
#include "decoder.cpp"
#include "dma.cpp"
#include "io.cpp"
#include "interpreter.cpp"
#include "interpreter-ipu.cpp"
#include "interpreter-scc.cpp"
#include "interpreter-vpu.cpp"
#include "recompiler.cpp"
#include "debugger.cpp"
#include "serialization.cpp"
#include "disassembler.cpp"
#include "emux.cpp"
#include "rsp-capture.cpp"
#include "rsp-commands.cpp"

auto RSP::load(Node::Object parent) -> void {
  node = parent->append<Node::Object>("RSP");
  dmem.allocate(4_KiB);
  imem.allocate(4_KiB);
  debugger.load(node);
}

auto RSP::unload() -> void {
  debugger.unload();
  dmem.reset();
  imem.reset();
  node.reset();
}

auto RSP::main() -> void {
  while(Thread::clock < 0) {
    auto clock = Thread::clock;

    captureHaltState();
    if(status.halted) {
      step(128);
      profile.cycles += 128;
      profile.haltedCycles += 128;
    } else {
      instruction();
    }

    dmaStep(Thread::clock - clock);
  }
}

// Flame chart: detect SP_STATUS.halted edges and emit a halt interval on the same
// rspWall axis as the command lane (now() + the RSP scheduler clock; see
// RSPCapture::TlSpan). While halted the clock still advances via step(), so rspWall
// tracks real time during the stop and the bar gets its true width. The still-open
// halt is drawn live by the UI from haltOpen/haltStartWall.
auto RSP::captureHaltState() -> void {
  auto& cap = capture;
  bool h = status.halted;
  if(!cap.enabled.load(std::memory_order_relaxed)) { cap.lastHalted = h; return; }
  if(h == cap.lastHalted) return;
  u64 wall = cpu.profiler.now() + (u64)(s64)Thread::clock;
  if(wall < cap.lastWall) wall = cap.lastWall;  //monotonic, shared with the command lane
  cap.lastWall = wall;
  if(h) {
    // BREAK / task end.
    if(cap.mode == RSPCapture::ModeF3DEX2) {
      // Close the last command's segment at the halt so it shows its true width
      // instead of stretching across the idle gap.
      captureF3DFlush(wall);
      // If this run produced no F3D command it was some other ucode we don't decode (e.g. audio) show the whole run as one "Unknown"
      if(cap.inTask && !cap.f3dSeenThisTask && wall > cap.taskStartWall) {
        u32 delta = (u32)((u32)pipeline.clocksTotal - (u32)cap.taskStartClocks);
        cap.pushTimeline(cap.taskStartWall, wall, 0, 0, true, RSPCapture::OverheadUnknown);
        cap.pushOverhead(delta, captureSequence++, RSPCapture::OverheadUnknown, 0, 0);
      }
    }
    cap.inTask = false;
    cap.haltStartWall.store(wall, std::memory_order_relaxed);
    cap.haltOpen.store(true, std::memory_order_release);
  } else {
    // A new RSP task begins, start classifying it (Unknown until an F3D command fires).
    cap.inTask = true;
    cap.f3dSeenThisTask = false;
    cap.taskStartWall = wall;
    cap.taskStartClocks = pipeline.clocksTotal;
    if(cap.haltOpen.load(std::memory_order_relaxed)) {
      cap.pushHalt(cap.haltStartWall.load(std::memory_order_relaxed), wall);
      cap.haltOpen.store(false, std::memory_order_release);
    }
  }
  cap.lastHalted = h;
}

auto RSP::instruction() -> void {
  if(Accuracy::RSP::Recompiler && recompiler.enabled) {
    auto block = recompiler.block(ipu.pc);
    block->execute(*this);
  } else {
    pipeline.dblIssueCount = 0;
    u32 instruction = imem.read<Word>(ipu.pc);
    instructionPrologue(instruction);
    branch.begin();
    pipeline.begin();
    OpInfo op0 = decoderEXECUTE(instruction);
    pipeline.issue(op0);
    interpreterEXECUTE();

    if(!pipeline.singleIssue && !op0.branch()) {
      u32 instruction = imem.read<Word>(ipu.pc + 4);
      OpInfo op1 = decoderEXECUTE(instruction);

      if(canDualIssue(op0, op1)) {
        pipeline.dblIssueCount = 1;
        instructionEpilogue<0>(0);
        instructionPrologue(instruction);
        branch.begin();
        pipeline.issue(op1);
        interpreterEXECUTE();
      }
    }

    pipeline.end();
    instructionEpilogue<0>(0);
  }

  //this handles all stepping for the interpreter
  //with the recompiler, it only steps for taken branch stalls
  step(pipeline.clocks);
  profile.cycles += pipeline.clocks;
  pipeline.clocksTotal += pipeline.clocks;
}

auto RSP::instructionPrologue(u32 instruction) -> void {
  pipeline.address = ipu.pc;
  pipeline.instruction = instruction;
  debugger.instruction();

  // RSP command viewer: check if this PC is a hooked RSPQ dispatch address
  if(capture.configLoaded && capture.hasHook(ipu.pc)) {
    captureCommandHook(ipu.pc);
  }
}

auto RSP::instructionBranchEpilogue() -> s32 {
  bool endBlock = branch.state & Branch::EndBlock;
  if(branch.inDelaySlot()) {
    pipeline.stall();
    if(branch.pc & 4) pipeline.singleIssue = 1;
  }

  branch.end();
  ipu.pc = branch.pc;
  return status.halted || endBlock;
}

template<bool Recompiled>
auto RSP::instructionEpilogue(u32 clocks) -> s32 {
  if constexpr(Recompiled) {
    step(clocks);
    profile.cycles += clocks;
    pipeline.clocksTotal += clocks;

    assert(ipu.r[0].u32 == 0);
  } else {
    ipu.r[0].u32 = 0;
  }

  return instructionBranchEpilogue();
}

auto RSP::power(bool reset) -> void {
  Thread::reset();
  dmem.fill();
  imem.fill();

  pipeline = {};
  profile = {};
  dma = {};
  status.semaphore = 0;
  status.halted = 1;
  status.broken = 0;
  status.full = 0;
  status.singleStep = 0;
  status.interruptOnBreak = 0;
  for(auto& signal : status.signal) signal = 0;
  for(auto& r : ipu.r) r.u32 = 0;
  ipu.pc = 0;
  branch.setPc(ipu.pc);
  for(auto& r : vpu.r) r = zero;
  vpu.acch = zero;
  vpu.accm = zero;
  vpu.accl = zero;
  vpu.vcoh = zero;
  vpu.vcol = zero;
  vpu.vcch = zero;
  vpu.vccl = zero;
  vpu.vce = zero;
  vpu.divin = 0;
  vpu.divout = 0;
  vpu.divdp = 0;

  reciprocals[0] = u16(~0);
  for(u16 index : range(1, 512)) {
    u64 a = index + 512;
    u64 b = (u64(1) << 34) / a;
    reciprocals[index] = u16(b + 1 >> 8);
  }

  for(u16 index : range(0, 512)) {
    u64 a = index + 512 >> (index % 2 == 1);
    u64 b = 1 << 17;
    //find the largest b where b < 1.0 / sqrt(a)
    while(a * (b + 1) * (b + 1) < (u64(1) << 44)) b++;
    inverseSquareRoots[index] = u16(b >> 1);
  }

  if constexpr(Accuracy::RSP::Recompiler) {
    auto buffer = ares::Memory::FixedAllocator::get().tryAcquire(1_MiB);
    recompiler.allocator.resize(1_MiB, bump_allocator::executable, buffer);
    recompiler.reset();
  }

  if constexpr(Accuracy::RSP::SISD) {
    platform->status("RSP vectorization disabled (no SSE 4.1 support)");
  }
}

}
