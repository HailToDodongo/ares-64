#pragma once

#include <nall/string.hpp>
#include <functional>
#include <mutex>
#include <ranges>
#include <vector>

struct Video {
  // driver management
  auto create(string driver = "") -> bool;
  auto driver() -> string { return "SDL3 GPU"; }
  auto ready() -> bool { return _ready; }
  auto reset() -> void { terminate(); }

  static auto hasDrivers() -> std::vector<string>;
  static auto hasDriver(string driver) -> bool;
  static auto optimalDriver() -> string { return "SDL3 GPU"; }
  static auto safestDriver() -> string { return "SDL3 GPU"; }

  // context (SDL_Window + SDL_GPUDevice from application)
  auto hasContext() -> bool { return true; }
  auto setContext(uintptr context) -> bool;

  // settings
  auto hasBlocking() -> bool { return true; }
  auto setBlocking(bool blocking) -> bool;
  auto blocking() -> bool { return _blocking; }

  auto hasFlush() -> bool { return true; }
  auto setFlush(bool flush) -> bool;
  auto flush() -> bool { return _flush; }

  auto hasMonitor() -> bool { return true; }
  auto setMonitor(string monitor) -> bool;
  auto monitor() -> string { return _monitor; }

  auto hasFullScreen() -> bool { return true; }
  auto setFullScreen(bool fullScreen) -> bool;
  auto fullScreen() -> bool { return _fullScreen; }

  // format (always ARGB24)
  auto hasFormats() -> std::vector<string> { return {"ARGB24"}; }
  auto hasFormat(string format) -> bool;
  auto setFormat(string format) -> bool;
  auto format() -> string { return _format; }

  // window
  auto focused() -> bool;
  auto clear() -> void {}

  struct Size { u32 width = 0, height = 0; };
  auto size() -> Size;

  // framebuffer (worker thread writes, UI thread reads)
  struct Acquire {
    explicit operator bool() const { return data; }
    u32* data = nullptr;
    u32 pitch = 0;
  };
  auto acquire(u32 width, u32 height) -> Acquire;
  auto release() -> void;
  auto output(u32 width, u32 height) -> void;
  auto renderFrame() -> void;
  auto outputTexture() -> uintptr { return (uintptr)_texture; }
  auto outputSize(u32& w, u32& h) -> void { w = _cpuWidth; h = _cpuHeight; }
  auto poll() -> void {}

  // threading (callers bracket acquire/release/output with lock/unlock)
  auto lock() -> void { _mutex.lock(); }
  auto unlock() -> void { _mutex.unlock(); }

  // monitor enumeration (static, SDL3-based)
  struct Monitor {
    string name;
    bool primary = false;
    s32 x = 0, y = 0;
    s32 width = 0, height = 0;
  };
  static auto hasMonitors() -> std::vector<Monitor>;
  static auto monitor(string name) -> Monitor;
  static auto hasMonitor(string name) -> bool;

private:
  auto initialize() -> bool;
  auto terminate() -> void;
  auto ensureTexture(u32 w, u32 h) -> bool;
  auto releaseTexture() -> void;

  SDL_Window* _window = nullptr;
  SDL_GPUDevice* _gpu = nullptr;
  bool _ready = false;

  string _monitor = "Primary";
  bool _fullScreen = false;
  bool _blocking = true;
  bool _flush = false;
  string _format = "ARGB24";

  // shared framebuffer state (guarded by _mutex)
  u32* _cpuBuffer = nullptr;
  u32 _cpuWidth = 0, _cpuHeight = 0;
  bool _framePending = false;

  SDL_GPUTexture* _texture = nullptr;
  SDL_GPUTransferBuffer* _transfer = nullptr;
  u32 _texWidth = 0, _texHeight = 0;

  std::recursive_mutex _mutex;
};
