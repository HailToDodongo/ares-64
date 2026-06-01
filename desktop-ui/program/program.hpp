struct AudioCapture {
  static constexpr u32 bufferSize = 48000 * 4;  // ~4 seconds at 48kHz
  f32 left[bufferSize] = {};
  f32 right[bufferSize] = {};
  std::atomic<u32> writePos{0};
  bool enabled = true;
  auto push(f64 l, f64 r) -> void {
    if(!enabled) return;
    auto pos = writePos.load(std::memory_order_relaxed);
    left[pos] = (f32)l;
    right[pos] = (f32)r;
    writePos.store((pos + 1) % bufferSize, std::memory_order_release);
  }
};

struct Program : ares::Platform {
  auto create() -> void;
  auto main() -> void;
  auto quit() -> void;

  //platform.cpp
  auto attach(ares::Node::Object) -> void override;
  auto detach(ares::Node::Object) -> void override;
  auto pak(ares::Node::Object) -> std::shared_ptr<vfs::directory> override;
  auto event(ares::Event) -> void override;
  auto log(ares::Node::Debugger::Tracer::Tracer tracer, string_view message) -> void override;
  auto status(string_view message) -> void override;
  auto video(ares::Node::Video::Screen, const u32* data, u32 pitch, u32 width, u32 height) -> void override;
  auto audio(ares::Node::Audio::Stream) -> void override;
  auto input(ares::Node::Input::Input) -> void override;
  auto cheat(u32 address) -> maybe<u32> override;

  //load.cpp
  auto identify(const string& filename) -> std::shared_ptr<Emulator>;
  auto load(std::shared_ptr<Emulator> emulator, string location = {}) -> bool;
  auto load(string location) -> bool;
  auto unload() -> void;

  //states.cpp
  auto stateSave(u32 slot) -> bool;
  auto stateLoad(u32 slot) -> bool;
  auto undoStateSave() -> bool;
  auto undoStateLoad() -> bool;
  auto clearUndoStates() -> void;

  //status.cpp
  auto updateMessage() -> void;
  auto showMessage(const string&) -> void;
  auto error(const string&) -> void;

  //utility.cpp
  auto pause(bool) -> void;
  auto mute() -> void;
  auto runAheadUpdate() -> void;
  auto captureScreenshot(const u32* data, u32 pitch, u32 width, u32 height) -> void;
  auto openFile(hiro::BrowserDialog&) -> string;
  auto selectFolder(hiro::BrowserDialog&) -> string;
  auto saveFile(hiro::BrowserDialog& dialog) -> string;

  //drivers.cpp
  auto videoDriverUpdate() -> void;
  auto videoMonitorUpdate() -> void;
  auto videoFormatUpdate() -> void;
  auto videoFullScreenToggle() -> void;
  auto videoPseudoFullScreenToggle() -> void;

  auto audioDriverUpdate() -> void;
  auto audioDeviceUpdate() -> void;
  auto audioFrequencyUpdate() -> void;
  auto audioLatencyUpdate() -> void;

  auto inputDriverUpdate() -> void;

  auto driverInitFailed(nall::string& driver, const char* kind, auto&& updateSettingsWindow) -> void;
  
  bool startFullScreen = false;
  bool startPseudoFullScreen = false;
  bool kiosk = false;
  std::vector<string> startGameLoad;
  bool noFilePrompt = false;
  bool settingsWindowConstructed = false;
  bool gameBrowserWindowConstructed = false;
  bool toolsWindowConstructed = false;

  string startSystem;
  string startSaveStateSlot;

  AudioCapture audioCapture;
  std::vector<ares::Node::Video::Screen> screens;
  std::vector<ares::Node::Audio::Stream> streams;

  // "Frame" advances whole frames until the VI framebuffer origin (dramAddress)
  // is swapped to a different buffer — a real new presented frame — bounded by a
  // frame cap so a static screen can't hang the step. RSP/RDP step on commands.
  enum class StepType : u32 { None, Frame, RSP, RDP };
  StepType stepType = StepType::Frame;
  // Incremented on every RSP/RDP step the user fires, so both command viewers
  // can detect "a step happened" even if it added no rows to their own table.
  u32 stepSequence = 0;

  bool paused = false;
  bool fastForwarding = false;
  bool rewinding = false;
  bool runAhead = false;
  bool requestFrameAdvance = false;
  bool requestScreenshot = false;
  bool keyboardCaptured = false;
  atomic<bool> pendingKioskExit = false;

  struct State {
    u32 slot = 1;
    u32 undoSlot = 1;
  } state;

  //rewind.cpp
  struct Rewind {
    enum class Mode : u32 { Playing, Rewinding } mode = Mode::Playing;
    std::vector<serializer> history;
    u32 length = 0;
    u32 frequency = 0;
    u32 counter = 0;
  } rewind;
  auto rewindSetMode(Rewind::Mode) -> void;
  auto rewindReset() -> void;
  auto rewindRun() -> void;

  struct Message {
    u64 timestamp = 0;
    string text;
  } message;

  class Guard;

  std::vector<Message> messages;
  string configuration;
  atomic<u64> vblanksPerSecond = 0;

  bool _isRunning = false;
  bool _imguiMode = true;  // ImGui is now the only UI path
  uintptr _videoContext = 0;

  /// Mutex used to manage access to the input system. Polling occurs on the main thread while the results are read by the emulation thread.
  std::recursive_mutex inputMutex;
  
private:
  /// Routine used while the emulator thread is waiting for work to complete that modifies its state.
  auto waitForInterrupts() -> void;
  /// The main run loop for the emulator thread spawned on startup.
  auto emulatorRunLoop(uintptr_t) -> void;

  atomic<bool> _quitting = false;
  atomic<bool> _needsResize = false;
  atomic<bool> _quitRequested = false;  // quit requested by emulator thread

  /// Mutex used to manage access to the status message queue.
  std::recursive_mutex _messageMutex;

  /// Mutex used to manage interrupts to the emulator run loop. Acquired when setting either of the `_interruptWaiting` or `_interruptWorking` booleans.
  std::mutex _programMutex;
  /// The emulator run loop condition variable. This variable is signaled to indicate other threads may access the program mutex, as well as to indicate that the emulator run loop can continue.
  std::condition_variable _programConditionVariable;
  /// Boolean indicating whether another thread is waiting to modify the emulator program state.
  std::atomic<bool> _interruptWaiting = false;
  /// Boolean indicating whether a non-emulator worker thread is in the process of modifying the emulator program state.
  bool _interruptWorking = false;
  /// Counter used to allow for possible recursive creation of `Program::Guard` instances (which can happen in some UI paths).
  u32 _interruptDepth = 0;
  /// Thread-local variable used to allow the emulator worker thread to create `Program::Guard` instances without causing a deadlock.
  static inline thread_local bool _programThread = false;
  /// Debug accessor for `_programThread` since debuggers cannot read TLS values correctly.
  NALL_USED auto getProgramThreadValue() -> bool {
    return _programThread;
  }
  /// Unique lock used by `Program::Guard` instances to synchronize interrupts. Generally this should not be used directly, with synchronization happening via `Program::Guard` construction. However, it is still exposed for exceptions such as the exit handler in `Program::quit()`.
  std::unique_lock<std::mutex> lock{_programMutex, std::defer_lock};
};

extern Program program;
