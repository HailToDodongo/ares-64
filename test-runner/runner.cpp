#include "runner.hpp"

#include <algorithm>
#include <cmath>

using namespace nall;

EmulatorRunner emulatorRunner;

//--- lifecycle -------------------------------------------------------------

auto EmulatorRunner::loadRom(const string& path) -> string {
  if(root) closeRom();

  if(!inode::exists(path)) return {"ROM not found: ", path};

  gamePak = mia::Medium::create("Nintendo 64");
  if(gamePak->load(path) != successful) return {"failed to load ROM: ", path};
  if(gamePak->pak->attribute("dd").boolean()) return "64DD disks are not supported";

  systemPak = mia::System::create("Nintendo 64");
  if(systemPak->load() != successful) {
    return "missing PIF firmware (Firmware/Nintendo 64/pif.*.rom next to the executable)";
  }

  //deterministic, GPU-free configuration
  ares::Nintendo64::option("Quality", "SD");
  ares::Nintendo64::option("Supersampling", "false");
  ares::Nintendo64::option("Enable GPU acceleration", "false");
  ares::Nintendo64::option("RDP Renderer", renderer);
  ares::Nintendo64::option("Homebrew Mode", homebrewMode);
  ares::Nintendo64::option("Deterministic Entropy", "true");
  ares::Nintendo64::option("Recompiler", "true");
  ares::Nintendo64::option("Expansion Pak", "true");

  //the region list can contain several entries (e.g. "NTSC-U,PAL"): prefer NTSC
  string regionList = gamePak->pak->attribute("region");
  palSystem = regionList.find("PAL") && !regionList.find("NTSC");
  string systemName = {"[Nintendo] Nintendo 64 (", palSystem ? "PAL" : "NTSC", ")"};

  if(!ares::Nintendo64::load(root, systemName)) {
    return {"core refused to load: ", systemName};
  }

  if(auto port = root->find<ares::Node::Port>("Cartridge Slot")) {
    port->allocate();
    port->connect();
  } else {
    return "no cartridge slot found";
  }

  inputPortLookup.clear();
  for(u32 id : range(4)) {
    if(auto port = root->find<ares::Node::Port>({"Controller Port ", 1 + id})) {
      port->allocate("Gamepad");
      port->connect();
      if(auto peripheral = port->connected()) {
        for(auto& input : peripheral->find<ares::Node::Input::Input>()) {
          inputPortLookup[input.get()] = id;
        }
      }
    }
  }

  frameCount = 0;
  logText = {};
  logLine = {};
  haveFrame = false;
  //store absolute: RSP capture auto-detection derives sibling paths from it,
  //which breaks for bare relative names once dirname() is empty
  romPath = path.beginsWith("/") ? path : nall::string{nall::Path::active(), path};
  rspProfInited = false;
  rspAggregating = false;
  rspFrozen = false;
  rspLostRows = 0;
  rspAgg.clear();
  root->power();
  return {};
}

auto EmulatorRunner::closeRom() -> void {
  if(!root) return;
  root->unload();  //joins the screen worker thread; no save files are written
  root.reset();
  gamePak.reset();
  systemPak.reset();
  streams.clear();
  for(auto& pad : pads) pad.clear();
  inputPortLookup.clear();
  paused = true;
}

auto EmulatorRunner::reset(bool hard) -> void {
  if(!root) return;
  //hard = cold boot (RDRAM refilled), soft = reset button
  root->power(!hard);
  frameCount = 0;
}

auto EmulatorRunner::setRenderer(const string& name) -> void {
  renderer = name;  //used by subsequent loadRom calls
  if(!root) return;

  using Renderer = ares::Nintendo64::System::Renderer;
  ares::Nintendo64::system.requestRenderer(name == "angrylion" ? Renderer::Angrylion : Renderer::None);
  ares::Nintendo64::system.applyPendingRenderer();
}

//--- time ------------------------------------------------------------------

auto EmulatorRunner::runSlice() -> void {
  ares::Nintendo64::system.applyPendingRenderer();
  //one call advances exactly one VI tick: CPU::main() runs until VI::refreshed
  //and consumes it. The VI raises it per field while the display is active and
  //on an equivalent cadence while it is off, so this always returns — including
  //for ROMs that never enable the display (they simply never present a frame).
  root->run();
  ++frameCount;
}

auto EmulatorRunner::runFrames(u32 frames) -> void {
  if(!root) return;
  for(u32 n = 0; n < frames && !stopRequested(); n++) runSlice();
}

auto EmulatorRunner::cycles() const -> s64 {
  return ares::Nintendo64::cpu.profile.cpuCycles;
}

auto EmulatorRunner::cyclesPerSecond() const -> double {
  //Thread clocks run at twice the CPU rate; profile.cpuCycles counts the halved value
  return ares::Nintendo64::system.frequency() / 2.0;
}

auto EmulatorRunner::runSeconds(double seconds) -> void {
  if(!root || seconds <= 0) return;
  s64 target = cycles() + (s64)(seconds * cyclesPerSecond());
  while(cycles() < target && !stopRequested()) runSlice();
}

//--- input -----------------------------------------------------------------

auto EmulatorRunner::setInput(u32 port, const string& name, s64 value) -> bool {
  if(port >= 4) return false;
  static const char* names[] = {
    "X-Axis", "Y-Axis", "Up", "Down", "Left", "Right", "B", "A",
    "C-Up", "C-Down", "C-Left", "C-Right", "L", "R", "Z", "Start",
  };
  bool known = false;
  for(auto valid : names) known = known || name == valid;
  if(!known) return false;
  pads[port][name] = value;
  return true;
}

auto EmulatorRunner::clearInputs(u32 port) -> void {
  if(port < 4) pads[port].clear();
}

auto EmulatorRunner::input(ares::Node::Input::Input node) -> void {
  auto lookup = inputPortLookup.find(node.get());
  if(lookup == inputPortLookup.end()) return;
  u32 port = lookup->second;

  auto it = pads[port].find(node->name());
  s64 value = it != pads[port].end() ? it->second : 0;
  if(auto button = node->cast<ares::Node::Input::Button>()) button->setValue(value != 0);
  if(auto axis = node->cast<ares::Node::Input::Axis>()) axis->setValue(value);
}

//--- capture ---------------------------------------------------------------

auto EmulatorRunner::screenshot(ScreenshotResult& out, bool mayAdvance) -> string {
  if(!root) return "no ROM loaded";

  //nothing presented yet (e.g. straight after loadRom): run until the first frame
  if(!haveFrame.load()) {
    if(!mayAdvance) return "no frame captured yet and emulation cannot be advanced here";
    u32 deadline = frameCount.load() + 4;
    while(!haveFrame.load() && !stopRequested() && frameCount.load() < deadline) runSlice();
    if(!haveFrame.load()) {
      if(abortRun.load()) return {};  //a callback threw; the host rethrows it
      if(shutdownRequested.load()) return "shutdown while waiting for frame";
      return "no frame presented: the ROM has not enabled the display (VI inactive)";
    }
  }

  std::lock_guard<std::mutex> lock(frameMutex);
  out.width = lastWidth;
  out.height = lastHeight;
  out.rgba.clear();
  out.rgba.reserve((u64)lastWidth * lastHeight * 4);
  for(u32 pixel : lastFrame) {
    out.rgba.push_back(pixel >> 16);
    out.rgba.push_back(pixel >>  8);
    out.rgba.push_back(pixel >>  0);
    out.rgba.push_back(255);
  }
  return {};
}

auto EmulatorRunner::video(ares::Node::Video::Screen, const u32* data, u32 pitch, u32 width, u32 height) -> void {
  //Keep every presented frame so screenshot() is a pure read. This is one packed
  //copy per frame (~600 KiB at 640x240); the RGBA conversion happens on demand.
  std::lock_guard<std::mutex> lock(frameMutex);
  lastWidth = width;
  lastHeight = height;
  lastFrame.resize((size_t)width * height);
  for(u32 y : range(height)) {
    memory::copy<u32>(lastFrame.data() + (size_t)y * width, data + y * (pitch >> 2), width);
  }
  haveFrame = true;
}

auto EmulatorRunner::startAudio(u32 rateOverride) -> string {
  if(audioRecording) return "audio recording already active";
  if(streams.empty()) return "no ROM loaded";
  //default to the AI's current output rate (the game-programmed DAC frequency);
  //if the game changes the rate mid-recording, the resamplers keep producing a
  //continuous stream at this fixed rate (a WAV has a single sample rate).
  recordFrequency = rateOverride ? rateOverride : (u32)std::llround(streams[0]->frequency());
  if(recordFrequency == 0) recordFrequency = drainFrequency;
  for(auto& stream : streams) stream->setResamplerFrequency(recordFrequency);
  wavLeft.clear();
  wavRight.clear();
  audioRecording = true;
  return {};
}

auto EmulatorRunner::stopAudio(AudioRecording& out) -> string {
  if(!audioRecording) return "no audio recording active";
  audioRecording = false;
  out.frequency = recordFrequency;
  out.left = std::move(wavLeft);
  out.right = std::move(wavRight);
  wavLeft.clear();
  wavRight.clear();
  for(auto& stream : streams) stream->setResamplerFrequency(drainFrequency);
  return {};
}

auto EmulatorRunner::audio(ares::Node::Audio::Stream) -> void {
  if(streams.empty()) return;
  while(true) {
    for(auto& stream : streams) {
      if(!stream->pending()) return;
    }
    f64 samples[2] = {0.0, 0.0};
    for(auto& stream : streams) {
      f64 buffer[2];
      u32 channels = stream->read(buffer);
      if(channels == 1) {
        samples[0] += buffer[0];
        samples[1] += buffer[0];
      } else {
        samples[0] += buffer[0];
        samples[1] += buffer[1];
      }
    }
    if(!audioRecording) continue;  //streams must still be drained while not recording
    for(u32 c : range(2)) samples[c] = max(-1.0, min(+1.0, samples[c]));
    wavLeft .push_back((s16)(samples[0] * 32767.0));
    wavRight.push_back((s16)(samples[1] * 32767.0));
  }
}

//--- remaining platform callbacks ------------------------------------------

auto EmulatorRunner::attach(ares::Node::Object node) -> void {
  if(auto stream = node->cast<ares::Node::Audio::Stream>()) {
    streams = root->find<ares::Node::Audio::Stream>();
    stream->setResamplerFrequency(audioRecording ? recordFrequency : drainFrequency);
  }
}

auto EmulatorRunner::detach(ares::Node::Object node) -> void {
  if(auto stream = node->cast<ares::Node::Audio::Stream>()) {
    std::erase(streams, stream);
  }
}

auto EmulatorRunner::pak(ares::Node::Object node) -> std::shared_ptr<vfs::directory> {
  if(node->name() == "Nintendo 64" && systemPak) return systemPak->pak;
  if(node->name() == "Nintendo 64 Cartridge" && gamePak) return gamePak->pak;
  return {};
}

auto EmulatorRunner::event(ares::Event event) -> void {
  if(event == ares::Event::Shutdown) shutdownRequested = true;
}

auto EmulatorRunner::status(string_view message) -> void {
  print("[ares-test] ", message, "\n");
}

auto EmulatorRunner::log(ares::Node::Debugger::Tracer::Tracer tracer, string_view message) -> void {
#if ARES_DEBUG_TOOLS
  //RSP instruction tracer (rspTraceCommand): divert into the trace buffer so it
  //never mixes into the ISViewer log
  if((void*)tracer.get() == (void*)ares::Nintendo64::rsp.debugger.tracer.instruction.get()) {
    if(!rspTraceCapturing) return;
    if(rspTraceText.size() >= 16 * 1024 * 1024) {  //hard cap: stop the trace
      if(!rspTraceTruncated) {
        rspTraceTruncated = true;
        auto& cap = ares::Nintendo64::rsp.capture;
        cap.traceOvl = cap.traceCmd = -1; cap.traceRemaining = 0;
      }
      return;
    }
    string line{message};
    line.replace("\x1b[A", "");  //the stall-marker column uses cursor up/down
    line.replace("\x1b[B", "");
    rspTraceText.append(line, "\n");
    return;
  }
#endif
  if(!tracer->terminal()) return;
  print(message);  //unbuffered: the live ISViewer echo
  logText.append(message);
  //bound the accumulated text so runaway printf loops can't exhaust memory
  if(logText.size() > 4 * 1024 * 1024) logText = logText.slice(logText.size() - 1024 * 1024);

  if(!onLogLine) return;
  //The ISViewer tracer notifies per character, so reassemble lines here and hand
  //over only completed ones (without the newline).
  for(u32 n = 0; n < message.size(); n++) {
    char c = message.data()[n];
    if(c == '\r') continue;
    if(c == '\n') {
      onLogLine(logLine);
      logLine = {};
      continue;
    }
    logLine.append(c);
    //a ROM printing without newlines must not grow this without bound
    if(logLine.size() >= 64 * 1024) {
      onLogLine(logLine);
      logLine = {};
    }
  }
}

//--- RSP profiling ----------------------------------------------------------

auto EmulatorRunner::rspProfileInit() -> nall::string {
#if !ARES_DEBUG_TOOLS
  return "ares was built without ARES_ENABLE_DEBUG_TOOLS; RSP profiling is unavailable";
#else
  if(!root) return "no ROM loaded";
  if(rspProfInited) return {};
  auto& cap = ares::Nintendo64::rsp.capture;
  if(!cap.configLoaded) {
    if(!cap.autoDetect(romPath)) {
      return {"RSP capture setup failed for ", romPath,
              " — needs the libdragon .elf next to the ROM (or in a build/ dir) and "
              "rspq-libdragon.json (next to the ELF or the ares-test binary); "
              "the searched paths were printed above"};
    }
    //the dispatch-loop blocks may have been recompiled before the hook PCs were
    //known: flush the RSP recompiler so they pick up the capture hooks
    ares::Nintendo64::rsp.recompiler.reset();
  }
  cap.enabled.store(true, std::memory_order_release);
  rspDrainPos = cap.writePos.load(std::memory_order_acquire);  //skip any backlog
  rspProfInited = true;
  return {};
#endif
}

#if ARES_DEBUG_TOOLS
auto EmulatorRunner::rspDrain(s32 mOvl, s32 mCmd, u32& remaining) -> bool {
  auto& cap = ares::Nintendo64::rsp.capture;
  u32 wp = cap.writePos.load(std::memory_order_acquire);
  u32 avail = wp - rspDrainPos;
  if(avail > cap.maxCommands) {  //ring overflowed since the last drain
    rspLostRows += avail - cap.maxCommands;
    rspDrainPos = wp - cap.maxCommands;
  }
  while(rspDrainPos != wp) {
    auto& r = cap.commands[rspDrainPos % cap.maxCommands];
    rspDrainPos++;
    if(rspAggregating && !rspFrozen) {
      u32 key = r.isOverhead ? (0x10000u | r.overheadType)
                             : ((u32)r.overlayId << 8 | r.commandId);
      auto& a = rspAgg[key];
      a.overlayId = r.overlayId; a.commandId = r.commandId;
      a.overhead = r.isOverhead; a.overheadType = r.overheadType;
      a.count++; a.clocks += r.cycle;
      a.bytesIn += r.bytesIn; a.bytesOut += r.bytesOut;
    }
    if(mOvl >= 0 && !r.isOverhead && r.overlayId == (u16)mOvl && r.commandId == (u8)mCmd) {
      if(remaining) remaining--;
      if(remaining == 0) {
        if(rspAggregating) rspFrozen = true;  //window ends exactly at this row
        return true;
      }
    }
  }
  return false;
}
#else
auto EmulatorRunner::rspDrain(s32, s32, u32&) -> bool { return false; }
#endif

auto EmulatorRunner::rspProfileStart() -> nall::string {
  if(auto err = rspProfileInit()) return err;
  //note: the drain cursor is NOT advanced here. If a rspWaitCommand() aligned
  //it to a marker row, the window begins right after that marker even when more
  //rows were committed in the same emulation slice.
  rspAgg.clear();
  rspLostRows = 0;
  rspAggregating = true;
  rspFrozen = false;
  return {};
}

auto EmulatorRunner::rspProfileTable(RspProfileTable& out) -> nall::string {
#if !ARES_DEBUG_TOOLS
  return "ares was built without ARES_ENABLE_DEBUG_TOOLS; RSP profiling is unavailable";
#else
  if(!rspProfInited || !rspAggregating) return "no profile window open; call rspProfileStart() first";
  u32 none = 0;
  rspDrain(-1, -1, none);  //pull the remaining committed rows (no-op when frozen)
  auto& cap = ares::Nintendo64::rsp.capture;
  cap.refreshOverlayNames();  //resolve runtime-registered overlay/command names
  static const char* overheadNames[] = {"", "rspq: dispatch loop", "rspq: overlay switch", "rspq: buffer fetch", "unknown task"};
  out = {};
  for(auto& [key, a] : rspAgg) {
    RspProfileRow row = a;
    if(row.overhead) {
      row.name = overheadNames[row.overheadType <= 4 ? row.overheadType : 4];
      out.overheadClocks += row.clocks;
    } else {
      if(row.overlayId < 16) {
        row.overlayName = cap.overlayNameMap[row.overlayId];
        row.name = cap.commandNameMap[row.overlayId][row.commandId];
      }
      if(!row.name) row.name = {"cmd 0x", nall::hex(row.commandId, 2L)};
      out.commandClocks += row.clocks;
    }
    out.rows.push_back(row);
  }
  std::sort(out.rows.begin(), out.rows.end(),
            [](const RspProfileRow& x, const RspProfileRow& y) { return x.clocks > y.clocks; });
  out.lostRows = rspLostRows;
  return {};
#endif
}

auto EmulatorRunner::rspWaitCommand(const nall::string& name, s32 ovl, s32 cmd,
                                    u32 count, u32 timeoutFrames) -> nall::string {
#if !ARES_DEBUG_TOOLS
  return "ares was built without ARES_ENABLE_DEBUG_TOOLS; RSP profiling is unavailable";
#else
  if(auto err = rspProfileInit()) return err;
  if(count == 0) return {};
  auto& cap = ares::Nintendo64::rsp.capture;
  u32 remaining = count;
  s32 mOvl = name ? -1 : ovl, mCmd = name ? -1 : cmd;
  for(u32 frame = 0;; frame++) {
    if(name && mOvl < 0) {
      //resolve the name against runtime-registered overlays (repeats until the
      //overlay shows up; a command cannot execute before its overlay registers)
      cap.refreshOverlayNames();
      for(u32 o = 0; o < 16 && mOvl < 0; o++) {
        for(u32 i = 0; i < 256; i++) {
          if(cap.commandNameMap[o][i] == name) { mOvl = o; mCmd = (s32)i; break; }
        }
      }
    }
    if(rspDrain(mOvl, mCmd, remaining)) return {};
    if(frame >= timeoutFrames) {
      nall::string what = name ? name : nall::string{"overlay ", ovl, " command ", cmd};
      return {"waitRspCommand: timeout after ", timeoutFrames, " frames waiting for '", what,
              "' (", count - remaining, "/", count, " seen",
              (name && mOvl < 0) ? "; the command name never resolved — check rspq-libdragon.json / overlay registration" : "",
              ")"};
    }
    if(stopRequested()) return "waitRspCommand: interrupted";
    runSlice();
  }
#endif
}

auto EmulatorRunner::rspTraceCommand(const nall::string& name, s32 ovl, s32 cmd,
                                     u32 occurrences, u32 timeoutFrames,
                                     nall::string& outText, u32& outCount,
                                     bool& outTruncated) -> nall::string {
#if !ARES_DEBUG_TOOLS
  return "ares was built without ARES_ENABLE_DEBUG_TOOLS; RSP tracing is unavailable";
#else
  if(auto err = rspProfileInit()) return err;
  if(occurrences == 0) return {};
  auto& rsp = ares::Nintendo64::rsp;
  auto& cap = rsp.capture;

  //resolve a command name against runtime-registered overlays (may need frames)
  s32 mOvl = name ? -1 : ovl, mCmd = name ? -1 : cmd;
  for(u32 frame = 0; name && mOvl < 0; frame++) {
    cap.refreshOverlayNames();
    for(u32 o = 0; o < 16 && mOvl < 0; o++) {
      for(u32 i = 0; i < 256; i++) {
        if(cap.commandNameMap[o][i] == name) { mOvl = o; mCmd = (s32)i; break; }
      }
    }
    if(mOvl >= 0) break;
    if(frame >= timeoutFrames) {
      return {"rspTrace: command name '", name, "' never resolved — check rspq-libdragon.json / overlay registration"};
    }
    if(stopRequested()) return "rspTrace: interrupted";
    runSlice();
  }

  rspTraceText = {};
  rspTraceTruncated = false;
  rspTraceCapturing = true;
  cap.traceActive = false;
  cap.traceRemaining = occurrences;
  cap.traceOvl = mOvl; cap.traceCmd = mCmd;

  nall::string err;
  for(u32 frame = 0;; frame++) {
    if(cap.traceOvl < 0 && !cap.traceActive) break;  //all occurrences captured (or cap hit)
    if(frame >= timeoutFrames) {
      err = {"rspTrace: timeout after ", timeoutFrames, " frames (",
             occurrences - cap.traceRemaining, "/", occurrences, " occurrences traced)"};
      break;
    }
    if(stopRequested()) { err = "rspTrace: interrupted"; break; }
    runSlice();
  }

  outCount = occurrences - cap.traceRemaining;
  //disarm + safety stop (also covers the timeout-with-open-segment case)
  cap.traceOvl = cap.traceCmd = -1; cap.traceRemaining = 0;
  if(cap.traceActive) { rsp.debugger.tracer.instruction->setEnabled(false); cap.traceActive = false; }
  rspTraceCapturing = false;
  outTruncated = rspTraceTruncated;
  outText = std::move(rspTraceText);
  rspTraceText = {};
  return err;
#endif
}
