//EmulatorRunner: the ares::Platform implementation behind the JS scripting API.
//Single-threaded: the core runs inline on the calling (JS) thread inside the
//run*() primitives. The only cross-thread callback is video(), which arrives on
//the core's screen worker thread — it must never call into JS.

#pragma once

#include <ares/ares.hpp>
#include <n64/n64.hpp>
#include <mia/mia.hpp>

#include <atomic>
#include <map>
#include <vector>

struct EmulatorRunner : ares::Platform {
  //--- configuration ---
  nall::string renderer = "angrylion";  //"angrylion" | "none"
  bool homebrewMode = false;

  auto setRenderer(const nall::string& name) -> void;

  //--- lifecycle -----------------------------------------------------------
  //all return an empty string on success, or an error message
  auto loadRom(const nall::string& path) -> nall::string;
  auto closeRom() -> void;
  auto reset(bool hard) -> void;
  auto loaded() const -> bool { return (bool)root; }

  bool paused = true;

  //--- time ----------------------------------------------------------------
  //run the core until `frames` more video frames have been presented.
  //relies on the process watchdog for hang protection.
  auto runFrames(u32 frames) -> void;
  //frames per emulated second for the loaded system (60 NTSC / 50 PAL)
  auto refreshRate() const -> double { return palSystem ? 50.0 : 60.0; }
  auto frame() const -> u32 { return frameCount.load(); }

  //--- input ---------------------------------------------------------------
  //name: gamepad input node name ("A", "B", "Z", "L", "R", "Start", "Up", ...,
  //"C-Up", ..., "X-Axis", "Y-Axis"); axes take -32767..+32767, buttons 0/1
  auto setInput(u32 port, const nall::string& name, s64 value) -> bool;
  auto clearInputs(u32 port) -> void;

  //--- capture -------------------------------------------------------------
  struct ScreenshotResult {
    u32 width = 0, height = 0;
    std::vector<u8> rgba;         //width*height*4, R,G,B,A byte order, A = 255
  };
  //advances the core to the next presented frame (even when paused) and captures
  //it into memory. Returns error message or "".
  auto screenshot(ScreenshotResult& out) -> nall::string;

  struct AudioRecording {
    u32 frequency = 0;
    std::vector<s16> left, right;
  };
  //rateOverride = 0 records at the AI's current output rate (the stream frequency
  //the game programmed); a non-zero value forces that sample rate instead.
  auto startAudio(u32 rateOverride) -> nall::string;
  auto stopAudio(AudioRecording& out) -> nall::string;

  //resampler target used while not recording (streams must always be drained)
  static constexpr u32 drainFrequency = 48000;

  //--- ISViewer log --------------------------------------------------------
  nall::string logText;  //accumulated (main thread only); echoed to stdout live

  //--- ares::Platform ------------------------------------------------------
  auto attach(ares::Node::Object) -> void override;
  auto detach(ares::Node::Object) -> void override;
  auto pak(ares::Node::Object) -> std::shared_ptr<vfs::directory> override;
  auto event(ares::Event) -> void override;
  auto status(nall::string_view message) -> void override;
  auto log(ares::Node::Debugger::Tracer::Tracer, nall::string_view message) -> void override;
  auto video(ares::Node::Video::Screen, const u32* data, u32 pitch, u32 width, u32 height) -> void override;
  auto audio(ares::Node::Audio::Stream) -> void override;
  auto input(ares::Node::Input::Input) -> void override;

  std::atomic<bool> shutdownRequested{false};

private:
  ares::Node::System root;
  std::shared_ptr<mia::Pak> gamePak;
  std::shared_ptr<mia::Pak> systemPak;
  std::vector<ares::Node::Audio::Stream> streams;
  bool palSystem = false;

  std::atomic<u32> frameCount{0};

  //one emulation slice: applies any pending renderer swap at the frame boundary
  //(mirrors what the desktop UI's worker loop does), then advances the core
  auto runSlice() -> void;

  //per-port input state, keyed by input node name. Written from the JS thread,
  //read by input() on the same thread (the core runs inline) — no locking needed.
  std::map<nall::string, s64> pads[4];

  //input node -> owning controller port, precomputed at loadRom (input() is hot:
  //polled per joybus transaction; also avoids re-walking the node tree there)
  std::map<const void*, u32> inputPortLookup;

  //screenshot handshake with the screen worker thread
  struct {
    std::atomic<bool> pending{false};
    std::atomic<bool> done{false};
    ScreenshotResult result;  //written by video() before done
  } shot;

  bool audioRecording = false;
  u32 recordFrequency = drainFrequency;
  std::vector<s16> wavLeft, wavRight;
};

extern EmulatorRunner emulatorRunner;
