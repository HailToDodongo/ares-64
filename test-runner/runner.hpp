//EmulatorRunner: the ares::Platform implementation behind the JS scripting API.
//Single-threaded: the core runs inline on the calling (JS) thread inside the
//run*() primitives, and the headless build compiles ares with
//ARES_VIDEO_SYNCHRONOUS so video() is delivered on that same thread rather than
//by the screen worker — which is what makes "the last presented frame" a
//well-defined, deterministic thing to read.

#pragma once

#include <ares/ares.hpp>
#include <n64/n64.hpp>
#include <mia/mia.hpp>

#include <atomic>
#include <map>
#include <mutex>
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
  //run the core until `frames` more VI ticks have elapsed. A tick is one VI
  //field when the display is active; when the VI is off (a ROM that never calls
  //display_init — typical for headless self-test ROMs) the core still raises the
  //tick at the same ~60Hz cadence, so waits always terminate. Relies on the
  //process watchdog for genuine hangs.
  auto runFrames(u32 frames) -> void;
  //run for N seconds of emulated time, measured on the CPU clock rather than by
  //counting VI ticks — the tick rate is not constant (it roughly doubles while
  //the display is off), so only this reflects real in-game time. Granularity is
  //still one tick, since that is where the core can be stopped.
  auto runSeconds(double seconds) -> void;
  //the in-game clock: monotonic emulated CPU cycles, and its rate
  auto cycles() const -> s64;
  auto cyclesPerSecond() const -> double;
  //frames per emulated second for the loaded system (60 NTSC / 50 PAL)
  auto refreshRate() const -> double { return palSystem ? 50.0 : 60.0; }
  //VI ticks (see runFrames); equals presented frames while the display is on
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
  //Returns the most recently presented frame — a pure read that advances nothing,
  //so it is safe from a callback and does not perturb timing. If no frame has been
  //presented yet, it advances until one arrives when mayAdvance is set, and fails
  //otherwise. Returns an error message or "".
  auto screenshot(ScreenshotResult& out, bool mayAdvance) -> nall::string;

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
  //called with each completed line (newline stripped) as the ROM prints it, from
  //inside the emulation loop. Installed by the script host; null when unused.
  void (*onLogLine)(const nall::string& line) = nullptr;

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
  //Set by the script host when a callback throws: the run loops return at the
  //next tick boundary so the exception surfaces immediately instead of after the
  //remainder of the wait has been emulated. Cleared when it is rethrown.
  std::atomic<bool> abortRun{false};
  auto stopRequested() const -> bool { return shutdownRequested.load() || abortRun.load(); }

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

  //Most recent presented frame, kept so screenshot() never has to advance.
  //Written by video() on the screen worker thread, read by the script thread.
  std::mutex frameMutex;
  std::vector<u32> lastFrame;  //tightly packed ARGB8888, converted on demand
  u32 lastWidth = 0, lastHeight = 0;
  std::atomic<bool> haveFrame{false};

  nall::string logLine;  //partial line awaiting its newline, for onLogLine
  bool audioRecording = false;
  u32 recordFrequency = drainFrequency;
  std::vector<s16> wavLeft, wavRight;
};

extern EmulatorRunner emulatorRunner;
