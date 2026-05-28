#pragma once

#include <functional>
#include <memory>
#include <vector>
#include <nall/hid.hpp>

struct Input {
  // Lifecycle
  auto create() -> bool;
  auto ready() -> bool { return _ready; }

  // Context (SDL_Window*)
  auto hasContext() -> bool { return true; }
  auto setContext(uintptr context) -> bool;

  // Mouse capture
  auto acquired() -> bool { return _mouseAcquired; }
  auto acquire() -> bool;
  auto release() -> bool;

  // Device polling and rumble
  auto poll() -> std::vector<std::shared_ptr<::nall::HID::Device>>;
  auto rumble(u64 id, u16 strong, u16 weak) -> bool;

  // Input change callback
  auto onChange(const std::function<void(std::shared_ptr<::nall::HID::Device>, u32, u32, s16, s16)>& fn) -> void {
    _onChange = fn;
  }

  // Keyboard capture (set from ImGui frame loop)
  static auto setKeyboardCaptured(bool captured) -> void { _keyboardCaptured = captured; }

  // Internal structs
  struct KeyMapping { int sc; u32 id; };
  struct Joypad {
    std::shared_ptr<::nall::HID::Joypad> hid = std::make_shared<::nall::HID::Joypad>();
    void* handle = nullptr;
  };

private:
  auto initialize() -> bool;
  auto terminate() -> void;
  auto enumerateJoypads() -> void;

  bool _ready = false;
  void* _window = nullptr;
  bool _mouseAcquired = false;
  f32 _lastMouseX = 0, _lastMouseY = 0;

  std::shared_ptr<::nall::HID::Keyboard> _keyboard;
  std::shared_ptr<::nall::HID::Mouse> _mouse;
  std::vector<Joypad> _joypads;
  std::vector<KeyMapping> _keyMap;

  static inline bool _keyboardCaptured = false;
  std::function<void(std::shared_ptr<::nall::HID::Device>, u32, u32, s16, s16)> _onChange;
};
