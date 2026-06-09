// RSP live command capture — the dispatch hook that turns hooked RSPQ PCs into
// timed command/overhead rows (see RSPCapture in rsp-capture.hpp; config and
// overlay/argument decoding live in rsp-capture.cpp).

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
  // RSP wall position on the shared timeline, lag-corrected (see RSPCapture::TlSpan).
  //
  // there is no single global tick counter in the N64 core.
  // the scheduler runs the CPU through an interval, then replays the RSP to *catch up* over that same interval. 
  // So at this hook now() is frozen at the END of the interval while the RSP is still somewhere inside it
  u64 wall = cpu.profiler.now() + (u64)(s64)Thread::clock;
  // Clamp monotonic: at a scheduler-slice boundary rsp.clock is re-set very
  // negative, so now()+clock can briefly dip below the previous segment's end
  // (plus a tiny catch-up overshoot), which would render as overlapping spans.
  if(wall < cap.lastWall) wall = cap.lastWall;
  cap.lastWall = wall;
  if(cap.segType != RSPCapture::SegNone) {
    u64 delta = (u32)((u32)now - (u32)cap.segStart);  //log value (clocksTotal, u32 wrap-safe)
    cap.pushTimeline(cap.segWall, wall,
                     cap.segType == RSPCapture::SegCommand ? cap.segOverlay : 0,
                     cap.segType == RSPCapture::SegCommand ? cap.segCommand : 0,
                     cap.segType != RSPCapture::SegCommand,
                     cap.segType == RSPCapture::SegLoop        ? RSPCapture::OverheadLoop
                   : cap.segType == RSPCapture::SegOverlayLoad ? RSPCapture::OverheadOvlLoad
                   : cap.segType == RSPCapture::SegFetch       ? RSPCapture::OverheadFetch
                   :                                             RSPCapture::OverheadNone);
    if(cap.segType == RSPCapture::SegCommand) {
      cap.push(delta, cap.segSeq, cap.frameNumber, cap.segOverlay, cap.segCommand, cap.segWordCount, cap.segWords,
               cap.segBytesIn, cap.segBytesOut);

      // Step mode: flush GPU so the framebuffer is current, then spin until the
      // UI advances. Only meaningful at real command boundaries.
      if(cap.stepMode.load(std::memory_order_relaxed)) {
        vulkan.render();
        vulkan.flush();
        rdp.capture.committedCount.store(rdp.capture.writePos.load(std::memory_order_acquire), std::memory_order_release);
        while(!cap.stepPending.load(std::memory_order_acquire)
              && cap.stepMode.load(std::memory_order_relaxed)) {
          if(cap.requestClear.exchange(false, std::memory_order_acq_rel)) {
            cap.committedCount.store(0, std::memory_order_release);
            cap.writePos.store(0, std::memory_order_release);
            // Also wipe the RDP command log so both viewers clear together.
            rdp.capture.committedCount.store(0, std::memory_order_release);
            rdp.capture.writePos.store(0, std::memory_order_release);
          }
          usleep(1000);
        }
        cap.stepPending.store(false, std::memory_order_release);
      }
    } else {
      u8 overhead = cap.segType == RSPCapture::SegLoop        ? RSPCapture::OverheadLoop
                  : cap.segType == RSPCapture::SegOverlayLoad ? RSPCapture::OverheadOvlLoad
                  :                                             RSPCapture::OverheadFetch;
      cap.pushOverhead(delta, cap.segSeq, overhead, cap.segBytesIn, cap.segBytesOut);
    }
  }

  // Open the new segment. Reserve its capture-order slot *now*, at the start of
  // the segment, so the row (emitted only when the next segment begins) still
  // sorts ahead of any RDP commands it kicks during its execution. segWall is this
  // hook's wall, i.e. the previous segment's end
  cap.segType = newType;
  cap.segStart = now;
  cap.segWall = wall;
  cap.segSeq = captureSequence++;
  cap.segBytesIn = 0;
  cap.segBytesOut = 0;
  if(newType == RSPCapture::SegCommand) rspReadPendingCommand(*this);
}
