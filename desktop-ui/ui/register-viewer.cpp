#include "ui.hpp"

#include "../desktop-ui.hpp"
#include <n64/n64.hpp>

namespace ares::ui {

bool showRegisterViewer = false;

// Components whose register file can be displayed. The visible set is a bitmask
// over this enum so several chips can be shown at once (multi-select combo).
enum RegComponent : u32 {
  COMP_CPU_GPR,   // CPU integer registers (COP0/IPU)
  COMP_CPU_COP0,  // CPU system control coprocessor (Status, Cause, ...)
  COMP_CPU_FPU,   // CPU floating-point unit (COP1)
  COMP_RSP_GPR,   // RSP scalar unit
  COMP_RSP_VU,    // RSP vector unit
  COMP_RDP,       // Reality Display Processor command regs
  COMP_VI,        // Video Interface
  COMP_AI,        // Audio Interface
  COMP_PI,        // Peripheral Interface
  COMP_SI,        // Serial Interface
  COMP_RI,        // RDRAM Interface
  COMP_COUNT,
};

static const char* compNames[COMP_COUNT] = {
  "CPU GPR", "CPU COP0", "CPU FPU", "RSP GPR", "RSP VU",
  "RDP", "VI", "AI", "PI", "SI", "RI",
};

// MIPS ABI register names, shared by the CPU and RSP scalar files.
static const char* gprNames[32] = {
  "r0", "at", "v0", "v1", "a0", "a1", "a2", "a3",
  "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
  "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7",
  "t8", "t9", "k0", "k1", "gp", "sp", "s8", "ra",
};

// One row in a detail table: name, raw hex value, and an optional human-readable
// decode (sample rate, fixed-point scale, flag name, ...)
static auto regRow(const char* name, u64 value, int hexDigits, const char* decoded = "") -> void {
  ImGui::TableNextRow();
  ImGui::TableNextColumn();
  ImGui::PushFont(monoFont);
  ImGui::TextUnformatted(name);
  ImGui::TableNextColumn();
  char buf[24];
  snprintf(buf, sizeof(buf), "%0*llX", hexDigits, (unsigned long long)value);
  ImGui::TextUnformatted(buf);
  ImGui::PopFont();
  ImGui::TableNextColumn();
  if(decoded && decoded[0]) ImGui::TextUnformatted(decoded);
}

// Three-column table (Reg | Value | Decoded) for the per-register detail views.
// The name column is generously sized so long labels like "VI_CTRL.colorDepth"
// fit, and the decode column stretches to fill the rest of the window.
static auto beginDetailTable(const char* id) -> bool {
  if(!ImGui::BeginTable(id, 3,
       ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
    return false;
  }
  ImGui::TableSetupColumn("Reg", ImGuiTableColumnFlags_WidthFixed, 150_px);
  ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed, 125_px);
  ImGui::TableSetupColumn("Decoded", ImGuiTableColumnFlags_WidthStretch);
  return true;
}

// Begin a register table inside a collapsing section. Returns false (and emits no table) 
// when the header is collapsed. `pairs` lays out N name/value column pairs per row so wide register files (GPRs) stay compact.
static auto beginRegTable(const char* id, int pairs) -> bool {
  if(!ImGui::BeginTable(id, pairs * 2,
       ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
    return false;
  }
  for(int p = 0; p < pairs; p++) {
    ImGui::TableSetupColumn("Reg", ImGuiTableColumnFlags_WidthFixed, 50_px);
    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed, 150_px);
  }
  return true;
}

// --- Per-component dumps -----------------------------------------------------

static auto drawCpuGpr() -> void {
  auto& ipu = ares::Nintendo64::cpu.ipu;
  if(ImGui::CollapsingHeader("CPU GPR", ImGuiTreeNodeFlags_DefaultOpen)) {
    // 32 GPRs laid out two pairs per row, then lo/hi/pc.
    if(beginRegTable("##cpugpr", 2)) {
      ImGui::PushFont(monoFont);
      for(int i = 0; i < 32; i += 2) {
        ImGui::TableNextRow();
        for(int c = 0; c < 2; c++) {
          int r = i + c;
          ImGui::TableNextColumn();
          ImGui::TextUnformatted(gprNames[r]);
          ImGui::TableNextColumn();
          ImGui::Text("%016llX", (unsigned long long)ipu.r[r].u64);
        }
      }
      ImGui::PopFont();
      ImGui::EndTable();
    }
    if(beginDetailTable("##cpugpr2")) {
      char dec[32];
      snprintf(dec, sizeof(dec), "%lld", (long long)ipu.lo.s64); regRow("lo", ipu.lo.u64, 16, dec);
      snprintf(dec, sizeof(dec), "%lld", (long long)ipu.hi.s64); regRow("hi", ipu.hi.u64, 16, dec);
      regRow("pc", ipu.pc, 16);
      ImGui::EndTable();
    }
  }
}

static auto drawCpuCop0() -> void {
  auto& scc = ares::Nintendo64::cpu.scc;
  if(ImGui::CollapsingHeader("CPU COP0 (System Control)")) {
    char dec[48];
    if(beginDetailTable("##cop0")) {
      regRow("Index", (u32)scc.index.tlbEntry | ((u32)scc.index.probeFailure << 31), 8);
      regRow("BadVAddr", scc.badVirtualAddress, 16);
      snprintf(dec, sizeof(dec), "%u", (u32)(scc.count >> 1)); regRow("Count", scc.count >> 1, 8, dec);
      snprintf(dec, sizeof(dec), "%u", (u32)scc.compare);      regRow("Compare", scc.compare, 8, dec);
      regRow("EPC", scc.epc, 16);
      regRow("ErrorEPC", scc.epcError, 16);
      regRow("LLAddr", scc.ll, 8);
      regRow("WatchLo", scc.watchLo.physicalAddress, 8);
      snprintf(dec, sizeof(dec), "%u", (u32)scc.wired.index);  regRow("Wired", scc.wired.index, 8, dec);
      ImGui::EndTable();
    }

    if(beginDetailTable("##cop0tlb")) {
      auto& t = scc.tlb;
      auto& cpu = ares::Nintendo64::cpu;
      snprintf(dec, sizeof(dec), "VPN2 %07X  ASID %02X  R%u",
        (u32)t.virtualAddress.bit(13, 39), (u32)t.addressSpaceID, (u32)t.region);
      regRow("EntryHi", cpu.getControlRegister(10), 16, dec);
      // EntryLo0/1: PFN + cache algorithm + dirty/valid/global flags.
      auto entryLo = [&](int i, const char* name, u64 reg) {
        snprintf(dec, sizeof(dec), "PFN %06X  C%u  %c%c%c",
          (u32)t.physicalAddress[i].bit(12, 35), (u32)t.cacheAlgorithm[i],
          t.dirty[i] ? 'D' : '-', t.valid[i] ? 'V' : '-', t.global[i] ? 'G' : '-');
        regRow(name, reg, 16, dec);
      };
      entryLo(0, "EntryLo0", cpu.getControlRegister(2));
      entryLo(1, "EntryLo1", cpu.getControlRegister(3));
      snprintf(dec, sizeof(dec), "mask %07X", (u32)t.pageMask);
      regRow("PageMask", cpu.getControlRegister(5), 8, dec);
      ImGui::EndTable();
    }
    // Status: decoded flags (the packed register is reassembled on read in HW).
    if(beginDetailTable("##cop0status")) {
      static const char* ksu[] = {"kernel", "supervisor", "user", "?"};
      // MIPS R4300i exception codes (Cause.ExcCode).
      static const char* exc[32] = {
        "Int", "Mod", "TLBL", "TLBS", "AdEL", "AdES", "IBE", "DBE",
        "Sys", "Bp", "RI", "CpU", "Ov", "Tr", "?", "FPE",
        "?", "?", "?", "?", "?", "?", "?", "Watch",
        "?", "?", "?", "?", "?", "?", "?", "?",
      };
      regRow("Status.IE", scc.status.interruptEnable, 1, scc.status.interruptEnable ? "enabled" : "disabled");
      regRow("Status.EXL", scc.status.exceptionLevel, 1);
      regRow("Status.ERL", scc.status.errorLevel, 1);
      regRow("Status.KSU", scc.status.privilegeMode, 1, ksu[scc.status.privilegeMode & 3]);
      regRow("Status.IM", scc.status.interruptMask, 2);
      regRow("Status.FR", scc.status.floatingPointMode, 1, scc.status.floatingPointMode ? "64-bit FGRs" : "32-bit FGRs");
      regRow("Cause.ExcCode", scc.cause.exceptionCode, 2, exc[scc.cause.exceptionCode & 31]);
      regRow("Cause.IP", scc.cause.interruptPending, 2);
      regRow("Cause.BD", scc.cause.branchDelay, 1, scc.cause.branchDelay ? "in delay slot" : "");
      ImGui::EndTable();
    }
  }
}

static auto drawCpuFpu() -> void {
  auto& fpu = ares::Nintendo64::cpu.fpu;
  if(ImGui::CollapsingHeader("CPU FPU (COP1)")) {
    if(ImGui::BeginTable("##fpu", 3,
         ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
      ImGui::TableSetupColumn("Reg", ImGuiTableColumnFlags_WidthFixed, 50_px);
      ImGui::TableSetupColumn("Hex", ImGuiTableColumnFlags_WidthFixed, 150_px);
      ImGui::TableSetupColumn("Double", ImGuiTableColumnFlags_WidthFixed, 150_px);
      ImGui::PushFont(monoFont);
      for(int i = 0; i < 32; i++) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::Text("f%d", i);
        ImGui::TableNextColumn(); ImGui::Text("%016llX", (unsigned long long)fpu.r[i].u64);
        ImGui::TableNextColumn(); ImGui::Text("%g", fpu.r[i].f64);
      }
      ImGui::PopFont();
      ImGui::EndTable();
    }
    // FCR31 (control/status): round mode + sticky/enable/cause exception bits.
    if(beginDetailTable("##fcr31")) {
      static const char* rm[] = {"nearest", "toward zero", "toward +inf", "toward -inf"};
      regRow("FCR31.RM", fpu.csr.roundMode, 1, rm[fpu.csr.roundMode & 3]);
      regRow("FCR31.Flags", fpu.csr.flag.data, 2, "sticky I/Z/O/U/V bits");
      regRow("FCR31.Enable", fpu.csr.enable.data, 2, "trap-enable I/Z/O/U/V bits");
      regRow("FCR31.Cause", fpu.csr.cause.data, 2, "last-op cause bits");
      regRow("FCR31.C", fpu.csr.compare, 1, fpu.csr.compare ? "true" : "false");
      ImGui::EndTable();
    }
  }
}

static auto drawRspGpr() -> void {
  auto& rsp = ares::Nintendo64::rsp;
  if(ImGui::CollapsingHeader("RSP GPR", ImGuiTreeNodeFlags_DefaultOpen)) {
    if(beginRegTable("##rspgpr", 2)) {
      ImGui::PushFont(monoFont);
      for(int i = 0; i < 32; i += 2) {
        ImGui::TableNextRow();
        for(int c = 0; c < 2; c++) {
          int r = i + c;
          ImGui::TableNextColumn();
          ImGui::TextUnformatted(gprNames[r]);
          ImGui::TableNextColumn();
          ImGui::Text("%08X", rsp.ipu.r[r].u32);
        }
      }
      ImGui::PopFont();
      ImGui::EndTable();
    }
    if(beginDetailTable("##rsppc")) {
      regRow("pc", rsp.ipu.pc, 4);
      ImGui::EndTable();
    }
    // SP_STATUS: the half-on-the-CPU-side status flags that gate execution.
    auto& s = rsp.status;
    ImGui::PushFont(monoFont);
    ImGui::Text("status: halt=%u broken=%u full=%u step=%u intOnBreak=%u sem=%u",
      (u32)s.halted, (u32)s.broken, (u32)s.full, (u32)s.singleStep,
      (u32)s.interruptOnBreak, (u32)s.semaphore);
    ImGui::Text("signals: %u%u%u%u%u%u%u%u",
      (u32)s.signal[7], (u32)s.signal[6], (u32)s.signal[5], (u32)s.signal[4],
      (u32)s.signal[3], (u32)s.signal[2], (u32)s.signal[1], (u32)s.signal[0]);
    ImGui::PopFont();
  }
}

static auto drawRspVu() -> void {
  auto& vpu = ares::Nintendo64::rsp.vpu;
  if(ImGui::CollapsingHeader("RSP VU (Vector)")) {
    if(ImGui::BeginTable("##rspvu", 2,
         ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
      ImGui::TableSetupColumn("Reg", ImGuiTableColumnFlags_WidthFixed, 50_px);
      ImGui::TableSetupColumn("Elements (e0..e7)", ImGuiTableColumnFlags_WidthStretch);
      ImGui::PushFont(monoFont);
      for(int i = 0; i < 32; i++) {
        auto& v = vpu.r[i];
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::Text("v%d", i);
        ImGui::TableNextColumn();
        ImGui::Text("%04X %04X %04X %04X %04X %04X %04X %04X",
          v.element(0), v.element(1), v.element(2), v.element(3),
          v.element(4), v.element(5), v.element(6), v.element(7));
      }
      // Accumulator (48-bit, split high/mid/low) and control registers.
      auto acc = [&](const char* name, ares::Nintendo64::RSP::r128& r) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextUnformatted(name);
        ImGui::TableNextColumn();
        ImGui::Text("%04X %04X %04X %04X %04X %04X %04X %04X",
          r.element(0), r.element(1), r.element(2), r.element(3),
          r.element(4), r.element(5), r.element(6), r.element(7));
      };
      acc("acch", vpu.acch); acc("accm", vpu.accm); acc("accl", vpu.accl);
      ImGui::PopFont();
      ImGui::EndTable();
    }
  }
}

static auto drawRdp() -> void {
  auto& c = ares::Nintendo64::rdp.command;
  if(ImGui::CollapsingHeader("RDP")) {
    char dec[48];
    if(beginDetailTable("##rdp")) {
      snprintf(dec, sizeof(dec), "%u bytes", (u32)(c.end - c.start));
      regRow("DPC_START", c.start, 6);
      regRow("DPC_END", c.end, 6, dec);
      regRow("DPC_CURRENT", c.current, 6);
      regRow("DPC_CLOCK", c.clock, 6);
      regRow("PIPE_BUSY", c.pipeBusy, 6);
      regRow("TMEM_BUSY", c.tmemBusy, 6);
      regRow("BUFFER_BUSY", c.bufferBusy, 6);
      ImGui::EndTable();
    }
    ImGui::PushFont(monoFont);
    ImGui::Text("status: ready=%u freeze=%u crashed=%u flush=%u source=%s",
      (u32)c.ready, (u32)c.freeze, (u32)c.crashed, (u32)c.flush,
      c.source ? "DMEM" : "RDRAM");
    ImGui::PopFont();
  }
}

static auto drawVi() -> void {
  auto& io = ares::Nintendo64::vi.io;
  if(ImGui::CollapsingHeader("VI (Video Interface)")) {
    char dec[48];
    if(beginDetailTable("##vi")) {
      static const char* depth[] = {"blank", "reserved", "16bpp (5/5/5/1)", "32bpp (8/8/8/8)"};
      static const char* aa[] = {
        "AA + resample (always fetch)", "AA + resample (fetch as needed)",
        "resample only (no AA)", "disabled (no AA/resample)",
      };
      regRow("VI_CTRL.colorDepth", io.colorDepth, 1, depth[io.colorDepth & 3]);
      regRow("VI_CTRL.serrate", io.serrate, 1, io.serrate ? "interlaced" : "progressive");
      regRow("VI_CTRL.antialias", io.antialias, 1, aa[io.antialias & 3]);
      regRow("VI_ORIGIN", io.dramAddress, 6);
      snprintf(dec, sizeof(dec), "%u px/line", (u32)io.width);          regRow("VI_WIDTH", io.width, 4, dec);
      snprintf(dec, sizeof(dec), "half-line %u", (u32)io.vcounter);     regRow("VI_V_CURRENT", io.vcounter, 4, dec);
      // X/Y scale are 2.10 unsigned fixed-point: source-pixels stepped per output
      // pixel. e.g. 0x200 = 0.500 (a 320-wide source upscaled to a 640 output).
      snprintf(dec, sizeof(dec), "%.4f src px/out", (f32)io.xscale / 1024.0f); regRow("VI_X_SCALE", io.xscale, 4, dec);
      snprintf(dec, sizeof(dec), "%.4f src px/out", (f32)io.yscale / 1024.0f); regRow("VI_Y_SCALE", io.yscale, 4, dec);
      // H/V video are start/end of the active region (H in pixels, V in half-lines).
      snprintf(dec, sizeof(dec), "%u px wide (%u..%u)", (u32)(io.hend - io.hstart), (u32)io.hstart, (u32)io.hend);
      regRow("VI_H_START", io.hstart, 4);
      regRow("VI_H_END", io.hend, 4, dec);
      snprintf(dec, sizeof(dec), "%u lines (%u..%u half)", (u32)(io.vend - io.vstart) / 2, (u32)io.vstart, (u32)io.vend);
      regRow("VI_V_START", io.vstart, 4);
      regRow("VI_V_END", io.vend, 4, dec);
      ImGui::EndTable();
    }
  }
}

static auto drawAi() -> void {
  auto& io = ares::Nintendo64::ai.io;
  if(ImGui::CollapsingHeader("AI (Audio Interface)")) {
    char dec[48];
    if(beginDetailTable("##ai")) {
      regRow("AI_DRAM_ADDR", io.dmaAddress[0], 6);
      snprintf(dec, sizeof(dec), "%u bytes", (u32)io.dmaLength[0]);  regRow("AI_LENGTH", io.dmaLength[0], 6, dec);
      regRow("AI_DMA_COUNT", io.dmaCount, 2);
      // Playback rate = VID clock / (dacRate + 1). Using the NTSC VI clock; PAL/MPAL
      // differ slightly, so this is approximate for those regions.
      u32 hz = io.dacRate ? 48681812u / ((u32)io.dacRate + 1) : 0;
      snprintf(dec, sizeof(dec), "~%u Hz (NTSC)", hz);              regRow("AI_DACRATE", io.dacRate, 4, dec);
      snprintf(dec, sizeof(dec), "%u-bit samples", (u32)io.bitRate + 1); regRow("AI_BITRATE", io.bitRate, 2, dec);
      regRow("AI_ENABLE", io.dmaEnable, 1, io.dmaEnable ? "on" : "off");
      ImGui::EndTable();
    }
  }
}

static auto drawPi() -> void {
  auto& io = ares::Nintendo64::pi.io;
  if(ImGui::CollapsingHeader("PI (Peripheral Interface)")) {
    char dec[48];
    if(beginDetailTable("##pi")) {
      regRow("PI_DRAM_ADDR", io.dramAddress, 8);
      regRow("PI_CART_ADDR", io.pbusAddress, 8);
      // PI length registers transfer (len + 1) bytes.
      snprintf(dec, sizeof(dec), "%u bytes", (u32)io.readLength + 1);  regRow("PI_RD_LEN", io.readLength, 8, dec);
      snprintf(dec, sizeof(dec), "%u bytes", (u32)io.writeLength + 1); regRow("PI_WR_LEN", io.writeLength, 8, dec);
      regRow("PI_STATUS.dmaBusy", io.dmaBusy, 1);
      regRow("PI_STATUS.ioBusy", io.ioBusy, 1);
      regRow("PI_STATUS.error", io.error, 1);
      regRow("PI_STATUS.interrupt", io.interrupt, 1);
      ImGui::EndTable();
    }
  }
}

static auto drawSi() -> void {
  auto& io = ares::Nintendo64::si.io;
  if(ImGui::CollapsingHeader("SI (Serial Interface)")) {
    if(beginDetailTable("##si")) {
      regRow("SI_DRAM_ADDR", io.dramAddress, 6);
      regRow("SI_PIF_RD_ADDR", io.readAddress, 8);
      regRow("SI_PIF_WR_ADDR", io.writeAddress, 8);
      regRow("SI_STATUS.dmaBusy", io.dmaBusy, 1);
      regRow("SI_STATUS.ioBusy", io.ioBusy, 1);
      regRow("SI_STATUS.interrupt", io.interrupt, 1);
      ImGui::EndTable();
    }
  }
}

static auto drawRi() -> void {
  auto& io = ares::Nintendo64::ri.io;
  if(ImGui::CollapsingHeader("RI (RDRAM Interface)")) {
    if(beginDetailTable("##ri")) {
      regRow("RI_MODE", io.mode, 8);
      regRow("RI_CONFIG", io.config, 8);
      regRow("RI_SELECT", io.select, 8);
      regRow("RI_REFRESH", io.refresh, 8);
      regRow("RI_LATENCY", io.latency, 8);
      ImGui::EndTable();
    }
  }
}

auto DrawRegisterViewer() -> void {
  if(!showRegisterViewer) return;

  ImGui::SetNextWindowSize(ImVec2(500_px, 500_px), ImGuiCond_FirstUseEver);
  if(!ImGui::Begin("Registers", &showRegisterViewer)) {
    ImGui::End();
    settings.general.showRegisterViewer = showRegisterViewer;
    return;
  }

  if(!emulator || emulator->name != "Nintendo 64") {
    ImGui::TextUnformatted("Register viewer only available for Nintendo 64.");
    ImGui::End();
    settings.general.showRegisterViewer = true;
    return;
  }

  // --- Multi-select component picker ---
  // Persisted across restarts via settings (General/RegisterViewerComponents).
  u32& shownMask = settings.general.registerViewerComponents;
  string preview;
  for(int i = 0; i < COMP_COUNT; i++) {
    if(shownMask & (1u << i)) {
      if(preview) preview.append(", ");
      preview.append(compNames[i]);
    }
  }
  if(!preview) preview = "(none)";

  ImGui::SetNextItemWidth(-1);
  if(ImGui::BeginCombo("##components", preview.data())) {
    // Checkboxes (rather than Selectables) so the on/off state of every chip is
    // visible at a glance, not just whichever rows happen to be hovered.
    for(int i = 0; i < COMP_COUNT; i++) {
      bool sel = shownMask & (1u << i);
      if(ImGui::Checkbox(compNames[i], &sel)) {
        if(sel) shownMask |= (1u << i);
        else    shownMask &= ~(1u << i);
      }
    }
    ImGui::EndCombo();
  }

  ImGui::Separator();

  if(ImGui::BeginChild("##regs")) {
    if(shownMask & (1u << COMP_CPU_GPR))  drawCpuGpr();
    if(shownMask & (1u << COMP_CPU_COP0)) drawCpuCop0();
    if(shownMask & (1u << COMP_CPU_FPU))  drawCpuFpu();
    if(shownMask & (1u << COMP_RSP_GPR))  drawRspGpr();
    if(shownMask & (1u << COMP_RSP_VU))   drawRspVu();
    if(shownMask & (1u << COMP_RDP))      drawRdp();
    if(shownMask & (1u << COMP_VI))       drawVi();
    if(shownMask & (1u << COMP_AI))       drawAi();
    if(shownMask & (1u << COMP_PI))       drawPi();
    if(shownMask & (1u << COMP_SI))       drawSi();
    if(shownMask & (1u << COMP_RI))       drawRi();
  }
  ImGui::EndChild();

  ImGui::End();
  settings.general.showRegisterViewer = true;
}

}  // namespace ares::ui
