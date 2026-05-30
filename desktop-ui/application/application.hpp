#pragma once

#include <nall/string.hpp>
#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>
#include <imgui.h>
#include <functional>

struct SDL3VideoContext {
  SDL_Window* window = nullptr;
  SDL_GPUDevice* gpu = nullptr;
};

struct AresApp {
  static auto initialize() -> bool;
  static auto run() -> void;
  static auto shutdown() -> void;
  static auto quit() -> void;
  static auto processEvents() -> void;
  static auto focused() -> bool;

  static inline SDL_Window* window = nullptr;
  static inline SDL_GPUDevice* gpu = nullptr;
  static inline SDL3VideoContext videoContext;
  static inline bool running = false;
  static inline std::function<void()> onMain;
};
