#include "application.hpp"
#include "../desktop-ui.hpp"  // settings
#include "../ui/log.hpp"
#include "../ui/ui.hpp"

#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlgpu3.h>

#include <cstdio>

namespace ares::ui {

bool uiScaleDirty = false;

// Effective UI scale: the user override (Settings -> Video -> Interface) when
// enabled, otherwise the DPI auto-detected at startup.
auto effectiveUiScale() -> float {
  if(settings.general.dpiOverride && settings.general.dpiScalePercent > 0)
    return settings.general.dpiScalePercent / 100.0f;
  return dpiScaleDetected > 0.0f ? dpiScaleDetected : 1.0f;
}

// (Re)build the ImGui dark theme at the given UI scale and publish it to dpiScale
// (the _px literal). The style is rebuilt from defaults every call so ScaleAllSizes
// never compounds, which makes this safe to re-run live when the scale changes.
// Fonts are loaded once at their base sizes; FontScaleMain scales them globally and
// ImGui 1.92 rasterizes glyphs on demand, so no font-atlas rebuild is needed.
auto applyUiScale(float scale) -> void {
  if(!(scale > 0.0f)) scale = 1.0f;
  ImGuiStyle& style = ImGui::GetStyle();
  style = ImGuiStyle();  // reset to unscaled defaults
  ImVec4* colors = style.Colors;

  colors[ImGuiCol_WindowBg] = ImVec4(0.07f, 0.07f, 0.09f, 1.00f);
  colors[ImGuiCol_MenuBarBg] = ImVec4(0.12f, 0.12f, 0.15f, 1.00f);
  colors[ImGuiCol_PopupBg] = ImVec4(0.18f, 0.18f, 0.22f, 1.00f);
  colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.40f, 0.40f, 0.40f, 0.60f);
  colors[ImGuiCol_Header] = ImVec4(0.18f, 0.18f, 0.22f, 1.00f);
  colors[ImGuiCol_HeaderHovered] = ImVec4(0.30f, 0.30f, 0.40f, 1.00f);
  colors[ImGuiCol_HeaderActive] = ImVec4(0.25f, 0.25f, 0.35f, 1.00f);
  colors[ImGuiCol_Button] = ImVec4(0.20f, 0.22f, 0.27f, 1.00f);
  colors[ImGuiCol_ButtonHovered] = ImVec4(0.30f, 0.32f, 0.40f, 1.00f);
  colors[ImGuiCol_ButtonActive] = ImVec4(0.35f, 0.38f, 0.50f, 1.00f);
  colors[ImGuiCol_FrameBg] = ImVec4(0.15f, 0.15f, 0.18f, 1.00f);
  colors[ImGuiCol_FrameBgHovered] = ImVec4(0.22f, 0.22f, 0.27f, 1.00f);
  colors[ImGuiCol_FrameBgActive] = ImVec4(0.25f, 0.25f, 0.30f, 1.00f);
  colors[ImGuiCol_Tab] = ImVec4(0.18f, 0.18f, 0.22f, 1.00f);
  colors[ImGuiCol_TabHovered] = ImVec4(0.35f, 0.35f, 0.50f, 1.00f);
  colors[ImGuiCol_TabSelected] = ImVec4(0.25f, 0.25f, 0.38f, 1.00f);
  colors[ImGuiCol_TabUnfocused] = ImVec4(0.13f, 0.13f, 0.17f, 1.00f);
  colors[ImGuiCol_TabUnfocusedActive] = ImVec4(0.20f, 0.20f, 0.25f, 1.00f);
  colors[ImGuiCol_TabSelectedOverline] = ImVec4(1.0f, 0.5f, 0.0f, 1.0f);
  colors[ImGuiCol_DragDropTarget] = ImVec4(1.0f, 0.5f, 0.0f, 1.0f);
  colors[ImGuiCol_TitleBg] = ImVec4(0.12f, 0.12f, 0.15f, 1.00f);
  colors[ImGuiCol_TitleBgActive] = ImVec4(0.15f, 0.15f, 0.20f, 1.00f);
  colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.10f, 0.10f, 0.12f, 1.00f);
  colors[ImGuiCol_Border] = ImVec4(0.20f, 0.20f, 0.25f, 0.50f);
  colors[ImGuiCol_BorderShadow] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
  colors[ImGuiCol_Text] = ImVec4(0.90f, 0.90f, 0.95f, 1.00f);
  colors[ImGuiCol_TextDisabled] = ImVec4(0.50f, 0.50f, 0.55f, 1.00f);
  colors[ImGuiCol_CheckMark] = ImVec4(0.50f, 0.70f, 1.00f, 1.00f);
  colors[ImGuiCol_SliderGrab] = ImVec4(0.50f, 0.70f, 1.00f, 1.00f);
  colors[ImGuiCol_SliderGrabActive] = ImVec4(0.60f, 0.80f, 1.00f, 1.00f);
  colors[ImGuiCol_ResizeGrip] = ImVec4(0.50f, 0.70f, 1.00f, 0.50f);
  colors[ImGuiCol_ResizeGripHovered] = ImVec4(0.60f, 0.80f, 1.00f, 0.75f);
  colors[ImGuiCol_ResizeGripActive] = ImVec4(0.70f, 0.90f, 1.00f, 1.00f);
  colors[ImGuiCol_ScrollbarBg] = ImVec4(0.10f, 0.10f, 0.12f, 1.00f);
  colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.30f, 0.30f, 0.35f, 1.00f);
  colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.40f, 0.40f, 0.50f, 1.00f);
  colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.45f, 0.45f, 0.55f, 1.00f);

  float rounding = 3.0f;
  style.TabBarOverlineSize = 2.0f;
  style.WindowRounding = rounding;
  style.FrameRounding = rounding;
  style.GrabRounding = rounding;
  style.PopupRounding = rounding;
  style.ScrollbarRounding = rounding;
  style.TabRounding = 0;
  style.WindowPadding = ImVec2(10, 10);
  style.FramePadding = ImVec2(6, 4);
  style.ItemSpacing = ImVec2(8, 6);
  style.PopupBorderSize = 0.0f;

  style.ScaleAllSizes(scale);   // widget metrics (padding, rounding, ...)
  style.FontScaleMain = scale;  // fonts (loaded at base size; scaled globally)
  dpiScale = scale;             // _px literal (raw-pixel drawing in the viewers)
}

}  // namespace ares::ui

auto AresApp::initialize() -> bool {
  if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
    fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
    return false;
  }

  SDL_DisableScreenSaver();

  window = SDL_CreateWindow("Ares 64", 1024, 768,
                            SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
  if (!window) {
    fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
    SDL_Quit();
    return false;
  }

  gpu = SDL_CreateGPUDevice(
    SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_MSL | SDL_GPU_SHADERFORMAT_DXIL,
    false, nullptr);
  if (!gpu) {
    fprintf(stderr, "SDL_CreateGPUDevice failed: %s\n", SDL_GetError());
    SDL_DestroyWindow(window);
    SDL_Quit();
    return false;
  }

  if (!SDL_ClaimWindowForGPUDevice(gpu, window)) {
    fprintf(stderr, "SDL_ClaimWindowForGPUDevice failed: %s\n", SDL_GetError());
    SDL_DestroyGPUDevice(gpu);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return false;
  }

  // Default to vsync; the video driver may switch this via setBlocking().
  SDL_SetGPUSwapchainParameters(gpu, window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR,
                                SDL_GPU_PRESENTMODE_VSYNC);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
  // Persist docking layout across sessions
  static char iniPath[1024];
  char* prefPath = SDL_GetPrefPath("ares-imgui", "ares");
  if(prefPath) {
    snprintf(iniPath, sizeof(iniPath), "%simgui.ini", prefPath);
    SDL_free(prefPath);
    io.IniFilename = iniPath;
  }

  // DPI scaling: detect the ratio of framebuffer pixels to window size. The theme
  // and scale are applied after fonts load (below) via applyUiScale, and can be
  // changed live from Settings -> Video -> Interface.
  int fbW, fbH, winW, winH;
  SDL_GetWindowSizeInPixels(window, &fbW, &winH);
  SDL_GetWindowSize(window, &winW, &winH);
  float detected = (winW > 0) ? (float)fbW / (float)winW : 1.0f;
  if(!(detected > 0.0f)) detected = 1.0f;  // guard against bad detection
  ares::ui::dpiScaleDetected = detected;   // remember the auto-detected value

  if (!ImGui_ImplSDL3_InitForSDLGPU(window)) {
    fprintf(stderr, "ImGui_ImplSDL3_InitForSDLGPU failed\n");
    ImGui::DestroyContext();
    SDL_DestroyGPUDevice(gpu);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return false;
  }

  ImGui_ImplSDLGPU3_InitInfo initInfo = {};
  initInfo.Device = gpu;
  initInfo.ColorTargetFormat = SDL_GetGPUSwapchainTextureFormat(gpu, window);
  initInfo.MSAASamples = SDL_GPU_SAMPLECOUNT_1;
  initInfo.SwapchainComposition = SDL_GPU_SWAPCHAINCOMPOSITION_SDR;
  initInfo.PresentMode = SDL_GPU_PRESENTMODE_VSYNC;
  if (!ImGui_ImplSDLGPU3_Init(&initInfo)) {
    fprintf(stderr, "ImGui_ImplSDLGPU3_Init failed\n");
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyGPUDevice(gpu);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return false;
  }

  // Load fonts at their base (unscaled) sizes; applyUiScale scales them live via
  // FontScaleMain. Each font keeps its own base size (proportional 14.5, mono 16),
  // and the global scale preserves that ratio.
  const char* basePath = SDL_GetBasePath();
  if(basePath) {
    nall::string fontPath = {basePath, "Altinn-DINExp.ttf"};
    auto imguiFont = io.Fonts->AddFontFromFileTTF(fontPath.data(), 14.5f);
    assert(imguiFont);

    // Monospaced font for numeric columns in viewer tables.
    nall::string monoPath = {basePath, "GoogleSansCode.ttf"};
    ares::ui::monoFont = io.Fonts->AddFontFromFileTTF(monoPath.data(), 16.0f);
    if(!ares::ui::monoFont) {
      ares::ui::monoFont = imguiFont;  // fallback to proportional
    }
  }

  // Build the theme at the effective scale (detected, or the saved user override).
  ares::ui::applyUiScale(ares::ui::effectiveUiScale());

  running = true;
  return true;
}

auto AresApp::run() -> void {
  Uint64 lastPresentNs = 0;
  while (running) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      ImGui_ImplSDL3_ProcessEvent(&event);

      switch (event.type) {
      case SDL_EVENT_QUIT:
        running = false;
        break;
      case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        if (event.window.windowID == SDL_GetWindowID(window)) {
          running = false;
        }
        break;
      case SDL_EVENT_KEY_DOWN:
        if(event.key.key == SDLK_F11) {
          bool fs = SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN;
          SDL_SetWindowFullscreen(window, !fs);
        }
        break;
      case SDL_EVENT_DROP_FILE: {
        if (event.drop.data) {
          SDL_free(const_cast<char*>(event.drop.data));
        }
        break;
      }
      }
    }

    if (!running) break;

    // Live UI rescale: a settings change sets this flag; apply it before NewFrame so
    // the new style/font scale takes effect cleanly for the whole frame.
    if (ares::ui::uiScaleDirty) {
      ares::ui::uiScaleDirty = false;
      ares::ui::applyUiScale(ares::ui::effectiveUiScale());
    }

    ImGui_ImplSDLGPU3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();

    // Full-window dockspace with embedded menu bar
    {
      ImGuiViewport* vp = ImGui::GetMainViewport();
      ImGui::SetNextWindowPos(vp->Pos);
      ImGui::SetNextWindowSize(vp->Size);
      ImGui::SetNextWindowViewport(vp->ID);
      ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
      ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
      ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));

      ImGuiWindowFlags dockFlags = ImGuiWindowFlags_NoDocking |
          ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
          ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
          ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;
      // In play mode there is no menu bar and no docked tool windows: the game output
      // is drawn straight into this window, so skip both the menu bar and the dockspace.
      if (!ares::ui::playMode) dockFlags |= ImGuiWindowFlags_MenuBar;

      ImGui::Begin("MainDockSpace", nullptr, dockFlags);
      ImGui::PopStyleVar(3);
      
      ImGui::DockSpace(ImGui::GetID("MainDockSpace"), ImVec2(0, 0),
                       ares::ui::playMode ? ImGuiDockNodeFlags_KeepAliveOnly : ImGuiDockNodeFlags_None);

      if (onMain) onMain();

      ImGui::End();
    }

    ImGui::Render();

    // Frame pacing: sleep on the CPU until the next display refresh before the
    // vsync present. Without this, SDL_WaitAndAcquireGPUSwapchainTexture blocks the
    // main thread *on the GPU* for the whole vsync interval, and that GPU-held wait
    // contends with the emulator's own Vulkan renderer (parallel-RDP) on the worker
    // thread, dropping it below full speed (e.g. 56 instead of 60 fps). Yielding on
    // the CPU first means we reach the present right at the vblank, so the swapchain
    // image is already available and the GPU wait is negligible. (SDL_DelayNS never
    // under-sleeps, so we never wake early and re-introduce the GPU stall.)
    {
      float hz = 60.0f;
      if (auto* mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(window))) {
        if (mode->refresh_rate > 1.0f) hz = mode->refresh_rate;
      }
      Uint64 frameNs = (Uint64)(1.0e9f / hz);
      Uint64 now = SDL_GetTicksNS();
      if (lastPresentNs != 0) {
        Uint64 target = lastPresentNs + frameNs;
        if (now < target) SDL_DelayNS(target - now);
      }
    }

    ImDrawData* drawData = ImGui::GetDrawData();
    bool minimized = drawData->DisplaySize.x <= 0.0f || drawData->DisplaySize.y <= 0.0f;

    SDL_GPUCommandBuffer* commandBuffer = SDL_AcquireGPUCommandBuffer(gpu);
    SDL_GPUTexture* swapchainTexture = nullptr;
    SDL_WaitAndAcquireGPUSwapchainTexture(commandBuffer, window, &swapchainTexture, nullptr, nullptr);

    if (swapchainTexture && !minimized) {
      // Upload ImGui vertex/index data before the render pass (mandatory for this backend).
      ImGui_ImplSDLGPU3_PrepareDrawData(drawData, commandBuffer);

      SDL_GPUColorTargetInfo targetInfo = {};
      targetInfo.texture = swapchainTexture;
      targetInfo.clear_color = {0.0f, 0.0f, 0.0f, 1.0f};
      targetInfo.load_op = SDL_GPU_LOADOP_CLEAR;
      targetInfo.store_op = SDL_GPU_STOREOP_STORE;

      SDL_GPURenderPass* renderPass = SDL_BeginGPURenderPass(commandBuffer, &targetInfo, 1, nullptr);
      ImGui_ImplSDLGPU3_RenderDrawData(drawData, commandBuffer, renderPass);
      SDL_EndGPURenderPass(renderPass);
    }

    SDL_SubmitGPUCommandBuffer(commandBuffer);
    lastPresentNs = SDL_GetTicksNS();
  }

  // Shutdown is handled separately via AresApp::shutdown()
  // so that ruby drivers can clean up GL resources first.
}

auto AresApp::shutdown() -> void {
  SDL_WaitForGPUIdle(gpu);
  ImGui_ImplSDLGPU3_Shutdown();
  ImGui_ImplSDL3_Shutdown();
  ImGui::DestroyContext();

  SDL_ReleaseWindowFromGPUDevice(gpu, window);
  SDL_DestroyGPUDevice(gpu);
  SDL_DestroyWindow(window);
  SDL_Quit();
}

auto AresApp::quit() -> void {
  running = false;
}

auto AresApp::processEvents() -> void {
  SDL_Event event;
  while (SDL_PollEvent(&event)) {
    ImGui_ImplSDL3_ProcessEvent(&event);
  }
}

auto AresApp::focused() -> bool {
  return SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS;
}
