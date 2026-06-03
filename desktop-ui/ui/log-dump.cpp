#include "ui.hpp"

#include "../desktop-ui.hpp"
#include <n64/n64.hpp>
#include <cstdio>
#include <vector>
#include <algorithm>

namespace ares::ui {

LogDumpState logDump;

// One RCP master-clock tick is 1/187.5 of a microsecond (see rsp-viewer.cpp).
static constexpr f64 ticksPerMicrosecond = 187.5;

static auto dumpRspStats(ares::Nintendo64::RSPCapture& cap, u32 count, u32 frame) -> void {
  struct OvlStat { string name; u32 count; u64 time; };
  struct CmdStat { string ovl, cmd; u32 count; u64 time; };
  std::vector<OvlStat> ovl;
  std::vector<CmdStat> cmds;
  u64 ohTime[4] = {}; u32 ohCnt[4] = {};  // synthetic overhead rows (loop/ucode/fetch)
  u64 totalTime = 0;

  auto findOvl = [&](const string& n) -> OvlStat& {
    for(auto& o : ovl) if(o.name == n) return o;
    ovl.push_back({n, 0, 0}); return ovl.back();
  };
  auto findCmd = [&](const string& o, const string& c) -> CmdStat& {
    for(auto& x : cmds) if(x.ovl == o && x.cmd == c) return x;
    cmds.push_back({o, c, 0, 0}); return cmds.back();
  };

  for(u32 k = 0; k < count; k++) {
    auto& c = cap.commands[k];
    totalTime += c.cycle;
    if(c.isOverhead) {
      u8 t = c.overheadType < 4 ? c.overheadType : 0;
      ohTime[t] += c.cycle; ohCnt[t]++;
      continue;
    }
    u8 slot = c.overlayId & 15;
    auto& nm = cap.overlayNameMap[slot];
    string oname = nm ? string{nm} : string{hex(slot)};
    auto& o = findOvl(oname); o.count++; o.time += c.cycle;
    auto& cn = cap.commandNameMap[slot][c.commandId];
    string cname = cn ? string{cn} : hex(c.commandId, 2L);
    auto& cs = findCmd(oname, cname); cs.count++; cs.time += c.cycle;
  }

  static const char* ohNames[] = {"?", "RSPQ_Loop", "DMA ucode", "DMA cmd."};
  for(u32 t = 1; t < 4; t++) if(ohCnt[t]) ovl.push_back({string{ohNames[t]}, ohCnt[t], ohTime[t]});

  std::sort(ovl.begin(),  ovl.end(),  [](auto& a, auto& b) { return a.time > b.time; });
  std::sort(cmds.begin(), cmds.end(), [](auto& a, auto& b) { return a.time > b.time; });

  f64 invTotal = totalTime ? 100.0 / (f64)totalTime : 0.0;

  printf("--- frame %u RSP totals: By Overlay ---\n", frame);
  printf("Overlay\tCount\tus\t%%\n");
  for(auto& o : ovl)
    printf("%s\t%u\t%.2f\t%.1f\n", o.name.data(), o.count, (f64)o.time / ticksPerMicrosecond, (f64)o.time * invTotal);
  printf("Total\t%u\t%.2f\t100.0\n", count, (f64)totalTime / ticksPerMicrosecond);

  printf("--- frame %u RSP totals: By Command ---\n", frame);
  printf("Overlay\tCommand\tCount\tus\t%%\n");
  for(auto& c : cmds)
    printf("%s\t%s\t%u\t%.2f\t%.1f\n", c.ovl.data(), c.cmd.data(), c.count,
           (f64)c.time / ticksPerMicrosecond, (f64)c.time * invTotal);
}

auto LogDumpOnFrame(u32 rspCount, u32 rdpCount) -> bool {
  if(!logDump.active()) return false;

  u32 frame = logDump.seenFrames++;
  if(frame < logDump.startFrame) return false;          // still skipping
  if(logDump.dumpedFrames >= logDump.frameCount) return false;  // already done

  u32 rspN = logDump.rsp ? rspCount : 0;
  u32 rdpN = logDump.rdp ? rdpCount : 0;

  auto& rsp = ares::Nintendo64::rsp.capture;
  auto& rdp = ares::Nintendo64::rdp.capture;

  printf("=== frame %u (%u RSP + %u RDP commands) ===\n", frame, rspN, rdpN);
  printf("Src\t#\tus\tOvl\tCommand\tData\tHex\n");

  static const char* overheadNames[] = {"?", "RSPQ_Loop", "DMA ucode", "DMA cmd."};
  static const char* overheadDesc[] = {
    "",
    "RSPQ_Loop / command overhead",
    "Load/Save ucode + state",
    "DMA new commands",
  };

  // Linear merge of the two already-ordered buffers by global capture sequence.
  u32 i = 0, j = 0;
  while(i < rspN || j < rdpN) {
    bool takeRsp;
    if(i >= rspN)      takeRsp = false;
    else if(j >= rdpN) takeRsp = true;
    else               takeRsp = rsp.commands[i].seq <= rdp.commands[j].seq;

    if(takeRsp) {
      auto& cmd = rsp.commands[i];
      f64 us = (f64)cmd.cycle / ticksPerMicrosecond;
      string ovl, name, data;
      if(cmd.isOverhead) {
        u8 t = cmd.overheadType < 4 ? cmd.overheadType : 0;
        ovl = "RSPQ";
        name = overheadNames[t];
        data = overheadDesc[t];
      } else {
        u8 slot = cmd.overlayId & 15;
        auto& ovlName = rsp.overlayNameMap[slot];
        ovl = ovlName ? string{ovlName} : string{hex(cmd.overlayId)};
        auto& nm = rsp.commandNameMap[slot][cmd.commandId];
        name = nm ? string{nm} : hex(cmd.commandId, 2L);
        data = rsp.formatArgs(cmd.overlayId, cmd.commandId, cmd.words, cmd.wordCount);
        if(!data) {
          for(u32 w = 0; w < min<u32>(cmd.wordCount, 6); w++) {
            if(w > 0) data.append(" ");
            data.append(hex(cmd.words[w], 8L));
          }
          if(cmd.wordCount > 6) data.append(" ...");
        }
      }
      data.replace("\n", " | ");
      // Src  #  us  Ovl  Command  Data  Hex(empty)
      printf("RSP\t%u\t%.2f\t%s\t%s\t%s\t\n", i, us, ovl.data(), name.data(), data.data());
      i++;
    } else {
      auto& cmd = rdp.commands[j];
      auto rname = ares::Nintendo64::rdpCommandName(cmd.opcode);
      auto desc = ares::Nintendo64::rdpCommandDescription(cmd.opcode, cmd.word0);
      desc.replace("\n", " | ");
      // Src  #  us(empty)  Ovl(empty)  Command  Data  Hex
      printf("RDP\t%u\t\t\t%s\t%s\t%016llX\n", j, rname, desc.data(), (unsigned long long)cmd.word0);
      j++;
    }
  }

  // Append the overlay/command totals (the RSP viewer's stats panel).
  if(logDump.rsp && rspN) dumpRspStats(rsp, rspN, frame);
  fflush(stdout);

  return ++logDump.dumpedFrames >= logDump.frameCount;  // true => time to quit
}

}  // namespace ares::ui
