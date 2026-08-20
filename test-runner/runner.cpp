#include "runner.hpp"

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
