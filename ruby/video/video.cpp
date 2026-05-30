#include <ruby/ruby.hpp>
#include <cstring>

using namespace nall;

namespace ruby {

// ── driver name helpers ────────────────────────────────────────────────

auto Video::hasDrivers() -> std::vector<string> {
  return {"SDL3 GPU", "None"};
}

auto Video::hasDriver(string driver) -> bool {
  auto drivers = hasDrivers();
  return std::ranges::find(drivers, driver) != drivers.end();
}

// ── create / context ───────────────────────────────────────────────────

auto Video::create(string driver) -> bool {
  terminate();
  return true;   // SDL3 GPU is the only backend; always "ready" after setContext
}

auto Video::setContext(uintptr context) -> bool {
  struct SDL3Context { SDL_Window* window; SDL_GPUDevice* gpu; };
  auto* ctx = reinterpret_cast<SDL3Context*>(context);
  if(ctx) { _window = ctx->window; _gpu = ctx->gpu; }
  return initialize();
}

// ── settings ───────────────────────────────────────────────────────────

auto Video::setBlocking(bool blocking) -> bool {
  _blocking = blocking;
  if(_gpu && _window) {
    SDL_SetGPUSwapchainParameters(_gpu, _window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR,
      _blocking ? SDL_GPU_PRESENTMODE_VSYNC : SDL_GPU_PRESENTMODE_IMMEDIATE);
  }
  return true;
}

auto Video::setFlush(bool flush) -> bool { _flush = flush; return true; }
auto Video::setMonitor(string monitor) -> bool { _monitor = monitor; return initialize(); }
auto Video::setFullScreen(bool fullScreen) -> bool { _fullScreen = fullScreen; return initialize(); }

// ── format ─────────────────────────────────────────────────────────────

auto Video::hasFormat(string format) -> bool {
  auto formats = hasFormats();
  return std::ranges::find(formats, format) != formats.end();
}

auto Video::setFormat(string format) -> bool {
  _format = format;
  return initialize();
}

// ── window ─────────────────────────────────────────────────────────────

auto Video::focused() -> bool {
  return _window && (SDL_GetWindowFlags(_window) & SDL_WINDOW_INPUT_FOCUS);
}

auto Video::size() -> Size {
  Size s;
  int iw = 0, ih = 0;
  if(_window) SDL_GetWindowSize(_window, &iw, &ih);
  s.width = iw; s.height = ih;
  return s;
}

// ── framebuffer acquire / output (worker thread) ───────────────────────

auto Video::acquire(u32 width, u32 height) -> Acquire {
  if(width == 0 || height == 0 || width > 4096 || height > 4096) return {};
  if(!_cpuBuffer || _cpuWidth != width || _cpuHeight != height) {
    delete[] _cpuBuffer;
    _cpuBuffer = new u32[width * height]();
    _cpuWidth = width;
    _cpuHeight = height;
  }
  return {_cpuBuffer, (u32)(width * sizeof(u32))};
}

auto Video::release() -> void {}

auto Video::output(u32 width, u32 height) -> void {
  if(width > 4096 || height > 4096) return;
  _framePending = true;
}

// ── render (UI thread) ─────────────────────────────────────────────────

auto Video::renderFrame() -> void {
  std::lock_guard<std::recursive_mutex> lock(_mutex);
  if(!_framePending || !_cpuBuffer || !_ready) return;

  if(!ensureTexture(_cpuWidth, _cpuHeight)) return;

  u32 pixels = _cpuWidth * _cpuHeight;
  void* mapped = SDL_MapGPUTransferBuffer(_gpu, _transfer, true);
  if(!mapped) return;
  // CPU buffer is 0xAARRGGBB; texture is RGBA8 so swap R/B, force opaque.
  auto* dst = reinterpret_cast<u32*>(mapped);
  for(u32 i = 0; i < pixels; i++) {
    u32 p = _cpuBuffer[i];
    dst[i] = ((p >> 16) & 0xff) | (p & 0x0000ff00) | ((p & 0xff) << 16) | 0xff000000;
  }
  SDL_UnmapGPUTransferBuffer(_gpu, _transfer);

  SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(_gpu);
  SDL_GPUCopyPass* copyPass = SDL_BeginGPUCopyPass(cmd);

  SDL_GPUTextureTransferInfo src = {};
  src.transfer_buffer = _transfer;
  src.pixels_per_row = _cpuWidth;
  src.rows_per_layer = _cpuHeight;

  SDL_GPUTextureRegion dstRegion = {};
  dstRegion.texture = _texture;
  dstRegion.w = _cpuWidth;
  dstRegion.h = _cpuHeight;
  dstRegion.d = 1;

  SDL_UploadToGPUTexture(copyPass, &src, &dstRegion, true);
  SDL_EndGPUCopyPass(copyPass);
  SDL_SubmitGPUCommandBuffer(cmd);

  _framePending = false;
}

// ── internal ───────────────────────────────────────────────────────────

auto Video::initialize() -> bool {
  terminate();
  if(!_window || !_gpu) return false;
  _ready = true;
  return _ready;
}

auto Video::terminate() -> void {
  _ready = false;
  if(_gpu) SDL_WaitForGPUIdle(_gpu);
  releaseTexture();

  delete[] _cpuBuffer;
  _cpuBuffer = nullptr;
  _cpuWidth = 0;
  _cpuHeight = 0;
  _framePending = false;
}

auto Video::ensureTexture(u32 w, u32 h) -> bool {
  if(!_gpu || w == 0 || h == 0) return false;
  if(_texture && _texWidth == w && _texHeight == h) return true;

  releaseTexture();

  SDL_GPUTextureCreateInfo texInfo = {};
  texInfo.type = SDL_GPU_TEXTURETYPE_2D;
  texInfo.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
  texInfo.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
  texInfo.width = w;
  texInfo.height = h;
  texInfo.layer_count_or_depth = 1;
  texInfo.num_levels = 1;
  texInfo.sample_count = SDL_GPU_SAMPLECOUNT_1;
  _texture = SDL_CreateGPUTexture(_gpu, &texInfo);
  if(!_texture) return false;

  SDL_GPUTransferBufferCreateInfo xferInfo = {};
  xferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
  xferInfo.size = w * h * sizeof(u32);
  _transfer = SDL_CreateGPUTransferBuffer(_gpu, &xferInfo);
  if(!_transfer) { releaseTexture(); return false; }

  _texWidth = w;
  _texHeight = h;
  return true;
}

auto Video::releaseTexture() -> void {
  if(_transfer) { SDL_ReleaseGPUTransferBuffer(_gpu, _transfer); _transfer = nullptr; }
  if(_texture) { SDL_ReleaseGPUTexture(_gpu, _texture); _texture = nullptr; }
  _texWidth = 0;
  _texHeight = 0;
}

// ── monitor enumeration (SDL3) ─────────────────────────────────────────

auto Video::hasMonitors() -> std::vector<Monitor> {
  std::vector<Monitor> monitors;

  int count = 0;
  auto* displayIDs = SDL_GetDisplays(&count);
  if(!displayIDs || count <= 0) {
    Monitor fallback;
    fallback.name = "Primary";
    fallback.primary = true;
    fallback.width = 640;
    fallback.height = 480;
    monitors.push_back(fallback);
    return monitors;
  }

  SDL_DisplayID primaryID = SDL_GetPrimaryDisplay();
  for(int i = 0; i < count; i++) {
    Monitor m;
    m.primary = (displayIDs[i] == primaryID);

    SDL_Rect rect;
    if(SDL_GetDisplayBounds(displayIDs[i], &rect)) {
      m.x = rect.x; m.y = rect.y;
      m.width = rect.w; m.height = rect.h;
    } else {
      m.width = 640; m.height = 480;
    }

    const char* name = SDL_GetDisplayName(displayIDs[i]);
    m.name = {1 + monitors.size(), ": ", name ? name : "Display"};
    monitors.push_back(m);
  }
  SDL_free(displayIDs);

  // primary first
  std::vector<Monitor> sorted;
  for(auto& m : monitors) { if(m.primary) sorted.push_back(m); }
  for(auto& m : monitors) { if(!m.primary) sorted.push_back(m); }
  return sorted;
}

auto Video::monitor(string name) -> Monitor {
  auto monitors = hasMonitors();
  for(auto& m : monitors) { if(m.name == name) return m; }
  for(auto& m : monitors) { if(m.primary) return m; }
  if(monitors.size() == 1) return monitors[0];

  Monitor fallback;
  fallback.name = "Primary";
  fallback.primary = true;
  fallback.width = 640;
  fallback.height = 480;
  return fallback;
}

auto Video::hasMonitor(string name) -> bool {
  for(auto& m : hasMonitors()) { if(m.name == name) return true; }
  return false;
}

} // namespace ruby
