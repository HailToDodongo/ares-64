#include "ui.hpp"

#include "../desktop-ui.hpp"
#include <n64/n64.hpp>
#include <cstdio>

namespace ares::ui {

LogDumpState logDump;

// One RCP master-clock tick is 1/187.5 of a microsecond (see rsp-viewer.cpp).
static constexpr f64 ticksPerMicrosecond = 187.5;

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
  fflush(stdout);

  return ++logDump.dumpedFrames >= logDump.frameCount;  // true => time to quit
}

}  // namespace ares::ui
