#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>
#include <cstring>

struct SDL3VideoContext {
  SDL_Window* window = nullptr;
  SDL_GPUDevice* gpu = nullptr;
};

//SDL3 GPU video driver (Vulkan/Metal/D3D12 backend, selected by SDL).
//The N64 framebuffer is uploaded into an SDL_GPUTexture each frame and handed
//to the ImGui SDL_GPU renderer (via outputTexture()) to be drawn as an image
//inside the viewport dock.
struct VideoSDL3 : VideoDriver {
  VideoSDL3& self = *this;
  VideoSDL3(Video& super) : VideoDriver(super) { construct(); }
  ~VideoSDL3() { destruct(); }

  auto create() -> bool override {
    VideoDriver::format = "ARGB24";
    return true;
  }

  auto driver() -> string override { return "SDL3 GPU"; }
  auto ready() -> bool override { return _ready; }

  auto hasFullScreen() -> bool override { return true; }
  auto hasMonitor() -> bool override { return true; }
  auto hasContext() -> bool override { return true; }
  auto hasBlocking() -> bool override { return true; }
  auto hasFlush() -> bool override { return true; }

  auto hasFormats() -> std::vector<string> override { return {"ARGB24"}; }

  auto setFullScreen(bool fullScreen) -> bool override { return initialize(); }
  auto setMonitor(string monitor) -> bool override { return initialize(); }

  auto setContext(uintptr context) -> bool override {
    auto* ctx = reinterpret_cast<SDL3VideoContext*>(context);
    if(ctx) { _window = ctx->window; _gpu = ctx->gpu; }
    return initialize();
  }

  auto setBlocking(bool blocking) -> bool override {
    if(_gpu && _window) {
      SDL_SetGPUSwapchainParameters(_gpu, _window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR,
        blocking ? SDL_GPU_PRESENTMODE_VSYNC : SDL_GPU_PRESENTMODE_IMMEDIATE);
    }
    return true;
  }

  auto setFlush(bool flush) -> bool override { return true; }

  auto setFormat(string format) -> bool override {
    if(format == "ARGB24") return initialize();
    return false;
  }

  auto focused() -> bool override { return true; }

  auto clear() -> void override {}

  auto size(u32& width, u32& height) -> void override {
    int w = 0, h = 0;
    if(_window) SDL_GetWindowSize(_window, &w, &h);
    width = w; height = h;
  }

  auto acquire(u32*& data, u32& pitch, u32 width, u32 height) -> bool override {
    if(width == 0 || height == 0 || width > 4096 || height > 4096) {
      data = nullptr;
      pitch = 0;
      return false;
    }
    if(!_cpuBuffer || _cpuWidth != width || _cpuHeight != height) {
      delete[] _cpuBuffer;
      _cpuBuffer = new u32[width * height]();
      _cpuWidth = width;
      _cpuHeight = height;
    }
    data = _cpuBuffer;
    pitch = width * sizeof(u32);
    return true;
  }

  auto release() -> void override {}

  auto output(u32 width, u32 height) -> void override {
    if(width > 4096 || height > 4096) return;
    _framePending = true;
  }

  //Called once per UI frame (from the viewport draw). Uploads the current CPU
  //framebuffer into the GPU texture so ImGui can sample it. The upload is
  //submitted on its own command buffer, ordered before the swapchain render.
  auto renderFrame() -> void {
    if(!_framePending || !_cpuBuffer || !_ready) return;

    if(!ensureTexture(_cpuWidth, _cpuHeight)) return;

    u32 pixels = _cpuWidth * _cpuHeight;
    void* mapped = SDL_MapGPUTransferBuffer(_gpu, _transfer, true);
    if(!mapped) return;
    //CPU buffer is 0xAARRGGBB; texture is RGBA8 (bytes R,G,B,A) so swap R/B and force opaque.
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
    src.offset = 0;
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

  auto outputTexture() -> uintptr override {
    return (uintptr)_texture;
  }

  auto outputSize(u32& w, u32& h) -> void override {
    w = _cpuWidth;
    h = _cpuHeight;
  }

  auto poll() -> void override {}

private:
  auto construct() -> void {}
  auto destruct() -> void { terminate(); }

  auto ensureTexture(u32 w, u32 h) -> bool {
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

  auto releaseTexture() -> void {
    if(_transfer) { SDL_ReleaseGPUTransferBuffer(_gpu, _transfer); _transfer = nullptr; }
    if(_texture) { SDL_ReleaseGPUTexture(_gpu, _texture); _texture = nullptr; }
    _texWidth = 0;
    _texHeight = 0;
  }

  auto initialize() -> bool {
    terminate();
    if(!_window || !_gpu) return false;
    _ready = true;
    return _ready;
  }

  auto terminate() -> void {
    _ready = false;
    if(_gpu) SDL_WaitForGPUIdle(_gpu);
    releaseTexture();

    delete[] _cpuBuffer;
    _cpuBuffer = nullptr;
    _cpuWidth = 0;
    _cpuHeight = 0;
    _framePending = false;
  }

  SDL_Window* _window = nullptr;
  SDL_GPUDevice* _gpu = nullptr;
  bool _ready = false;

  u32* _cpuBuffer = nullptr;
  u32 _cpuWidth = 0;
  u32 _cpuHeight = 0;
  bool _framePending = false;

  SDL_GPUTexture* _texture = nullptr;
  SDL_GPUTransferBuffer* _transfer = nullptr;
  u32 _texWidth = 0;
  u32 _texHeight = 0;
};
