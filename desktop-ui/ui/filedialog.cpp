#include "filedialog.hpp"
#include "../application/application.hpp"

#include <SDL3/SDL_dialog.h>
#include <imgui_impl_sdl3.h>
#include <atomic>
#include <cstdio>
#include <string>

namespace ares::ui {

static std::string dialogResult;
static std::atomic<bool> dialogDone{false};

static void SDLCALL dialogCallback(void* userdata, const char* const* filelist, int filter) {
  dialogResult.clear();
  if(filelist && filelist[0]) {
    dialogResult = filelist[0];
  }
  dialogDone.store(true);
  dialogDone.notify_all();
}

static auto waitForDialog() -> const char* {
  while(!dialogDone.load()) {
    SDL_Event event;
    while(SDL_PollEvent(&event)) {
      ImGui_ImplSDL3_ProcessEvent(&event);
    }
    SDL_Delay(16);
  }
  dialogDone.store(false);
  return dialogResult.empty() ? nullptr : dialogResult.c_str();
}

auto openFileDialog(const char* title, const char* path) -> const char* {
  SDL_ShowOpenFileDialog(dialogCallback, nullptr, AresApp::window, nullptr, 0, path, false);
  return waitForDialog();
}

auto saveFileDialog(const char* title, const char* path) -> const char* {
  SDL_ShowSaveFileDialog(dialogCallback, nullptr, AresApp::window, nullptr, 0, path);
  return waitForDialog();
}

auto selectFolderDialog(const char* title, const char* path) -> const char* {
  SDL_ShowOpenFolderDialog(dialogCallback, nullptr, AresApp::window, path, false);
  return waitForDialog();
}

auto messageBox(const char* title, const char* text, bool error) -> void {
  SDL_ShowSimpleMessageBox(
    error ? SDL_MESSAGEBOX_ERROR : SDL_MESSAGEBOX_INFORMATION,
    title, text, AresApp::window);
}

}  // namespace ares::ui
