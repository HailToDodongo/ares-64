#if defined(PLATFORM_WINDOWS) || defined(PLATFORM_MACOS)
  constexpr u32 layoutVertSize = 14;
#else
  constexpr u32 layoutVertSize = 18;
#endif

struct Settings : Markup::Node {
  using string = nall::string;

  auto load() -> void;
  auto save() -> void;
  auto process(bool load) -> void;

  string filePath;

  struct Video {
    string driver;
    string monitor;
    string format;
    bool exclusive = false;
    bool blocking = false;
    bool forceSRGB = false;
    bool threadedRenderer = true;
    bool nativeFullScreen = false;
    bool flush = false;
    string shader = "None";
    u32 windowWidth = 800;
    u32 windowHeight = 576;
    string output = "Scale";
    u32 fixedScale = 2;
    string aspectCorrection = "Standard";
    bool adaptiveSizing = true;
    bool autoCentering = false;

    f64 luminance = 1.0;
    f64 saturation = 1.0;
    f64 gamma = 1.0;
    bool colorBleed = false;
    bool colorEmulation = true;
    bool deepBlackBoost = false;
    bool interframeBlending = true;
    bool overscan = true;
    bool pixelAccuracy = false;

    string quality = "SD";
    bool supersampling = false;
    bool disableVideoInterfaceProcessing = false;
    bool weaveDeinterlacing = true;
  } video;

  struct Audio {
    string driver;
    string device;
    u32 frequency = 0;
    u32 latency = 0;
    bool exclusive = false;
    bool blocking = true;
    bool dynamic = false;
    bool mute = false;

    f64 volume = 1.0;
    f64 balance = 0.0;
  } audio;

  struct Input {
    string driver;
    string defocus = "Pause";
  } input;

  struct Boot {
    bool fast = false;
    bool debugger = false;
    bool awaitGDBClient = false;
    string prefer = "NTSC-U";
  } boot;

  struct General {
    bool rewind = false;
    bool runAhead = false;
    bool autoSaveMemory = true;
    bool homebrewMode = false;
    bool forceInterpreter = false;
    bool noFilePrompt = false;
    bool showAudioViewer = false;
    bool showManifestViewer = false;
    bool showCheatEditor = false;
    bool showTracerViewer = false;
    bool showRdpViewer = false;
    bool showFramebufferViewer = false;
  } general;

  struct Rewind {
    u32 length = 100;
    u32 frequency = 10;
  } rewind;

  struct Paths {
    string home;
    string firmware;
    string saves;
    string screenshots;
    string debugging;
    string arcadeRoms;
    struct SuperFamicom {
      string gameBoy;
      string bsMemory;
      string sufamiTurbo;
    } superFamicom;
  } paths;

  struct Recent {
    string game[9];
  } recent;

  struct DebugServer {
    u32 port = 9123;
    bool enabled = false;
    bool useIPv4 = false;
  } debugServer;

  struct Nintendo64 {
    bool expansionPak = true;
    u8 controllerPakBankCount = 1;
    string controllerPakBankString = "32KiB (Default)";
  } nintendo64;

  struct GameBoyAdvance {
    bool player = false;
  } gameBoyAdvance;

  struct MegaDrive {
    bool tmss = false;
  } megadrive;
};

// Panel types (hiro widget members removed; ImGui handles all UI)

struct VideoSettings {
  auto construct() -> void {}
  auto setVisible(bool=true) -> VideoSettings& { return *this; }
};

struct AudioSettings {
  auto construct() -> void {}
  auto setVisible(bool=true) -> AudioSettings& { return *this; }
};

struct InputSettings {
  auto construct() -> void {}
  auto eventInput(std::shared_ptr<nall::HID::Device>, u32, u32, s16, s16) -> void {}
  auto refresh() -> void {}
  auto setVisible(bool=true) -> InputSettings& { return *this; }
};

struct HotkeySettings {
  auto construct() -> void {}
  auto eventInput(std::shared_ptr<nall::HID::Device>, u32, u32, s16, s16) -> void {}
  auto refresh() -> void {}
  auto setVisible(bool=true) -> HotkeySettings& { return *this; }
};

struct EmulatorSettings {
  auto construct() -> void {}
  auto setVisible(bool=true) -> EmulatorSettings& { return *this; }
};

struct OptionSettings {
  auto construct() -> void {}
  auto setVisible(bool=true) -> OptionSettings& { return *this; }
};

struct FirmwareSettings {
  auto construct() -> void {}
  auto select(const nall::string&, const nall::string&, const nall::string&) -> bool { return false; }
  auto setVisible(bool=true) -> FirmwareSettings& { return *this; }
};

struct PathSettings {
  auto construct() -> void {}
  auto setVisible(bool=true) -> PathSettings& { return *this; }
};

struct DriverSettings {
  auto construct() -> void {}
  auto videoRefresh() -> void {}
  auto videoDriverUpdate() -> bool { return false; }
  auto audioRefresh() -> void {}
  auto audioDriverUpdate() -> bool { return false; }
  auto inputRefresh() -> void {}
  auto inputDriverUpdate() -> bool { return false; }
  auto setVisible(bool=true) -> DriverSettings& { return *this; }
};

struct DebugSettings {
  auto construct() -> void {}
  auto setVisible(bool=true) -> DebugSettings& { return *this; }
};

struct ImportExportSettings {
  auto construct() -> void {}
  auto setVisible(bool=true) -> ImportExportSettings& { return *this; }
};

struct HomePanel {
  auto construct() -> void {}
};

struct SettingsWindow {
  auto show(const nall::string&) -> void {}
  auto focused() -> bool { return false; }
  auto setDismissable(bool=true) -> SettingsWindow& { return *this; }
  bool initialized = false;

  VideoSettings videoSettings;
  AudioSettings audioSettings;
  InputSettings inputSettings;
  HotkeySettings hotkeySettings;
  EmulatorSettings emulatorSettings;
  OptionSettings optionSettings;
  FirmwareSettings firmwareSettings;
  PathSettings pathSettings;
  DriverSettings driverSettings;
  DebugSettings debugSettings;
  ImportExportSettings importExportSettings;
  HomePanel homePanel;
};

extern Settings settings;
extern SettingsWindow settingsWindow;
extern VideoSettings& videoSettings;
extern AudioSettings& audioSettings;
extern InputSettings& inputSettings;
extern HotkeySettings& hotkeySettings;
extern EmulatorSettings& emulatorSettings;
extern OptionSettings& optionSettings;
extern FirmwareSettings& firmwareSettings;
extern PathSettings& pathSettings;
extern DriverSettings& driverSettings;
extern DebugSettings& debugSettings;
extern ImportExportSettings& importExportSettings;
