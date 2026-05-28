#include "opengl/opengl.hpp"

#include <SDL3/SDL.h>
#include <cstring>

struct SDL3VideoContext {
  SDL_Window* window = nullptr;
  SDL_GLContext glContext = nullptr;
};

struct VideoSDL3 : VideoDriver, OpenGL {
  VideoSDL3& self = *this;
  VideoSDL3(Video& super) : VideoDriver(super) { construct(); }
  ~VideoSDL3() { destruct(); }

  auto create() -> bool override {
    VideoDriver::format = "ARGB24";
    return true;
  }

  auto driver() -> string override { return "OpenGL 3.3 (SDL3)"; }
  auto ready() -> bool override { return _ready; }

  auto hasFullScreen() -> bool override { return true; }
  auto hasMonitor() -> bool override { return true; }
  auto hasContext() -> bool override { return true; }
  auto hasBlocking() -> bool override { return true; }
  auto hasFlush() -> bool override { return true; }
  auto hasShader() -> bool override { return true; }

  auto hasFormats() -> std::vector<string> override { return {"ARGB24"}; }

  auto setFullScreen(bool fullScreen) -> bool override { return initialize(); }
  auto setMonitor(string monitor) -> bool override { return initialize(); }

  auto setContext(uintptr context) -> bool override {
    auto* ctx = reinterpret_cast<SDL3VideoContext*>(context);
    if(ctx) { _window = ctx->window; _glContext = ctx->glContext; }
    return initialize();
  }

  auto setBlocking(bool blocking) -> bool override {
    SDL_GL_SetSwapInterval(blocking ? 1 : 0);
    return true;
  }

  auto setFlush(bool flush) -> bool override { return true; }

  auto setFormat(string format) -> bool override {
    if(format == "ARGB24") { OpenGL::inputFormat = GL_RGBA8; return initialize(); }
    return false;
  }

  auto setShader(string shader) -> bool override {
    OpenGL::setShader(shader);
    return true;
  }

  auto focused() -> bool override { return true; }

  auto clear() -> void override {
    ensureOutputFBO();
    OpenGL::clear(_outputFBO);
  }

  auto size(u32& width, u32& height) -> void override {
    int w, h;
    SDL_GetWindowSize(_window, &w, &h);
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
    _pendingWidth = width;
    _pendingHeight = height;
    _framePending = true;
  }

  auto renderFrame() -> void {
    if(_ready) ensureOutputFBO();
    if(!_framePending || !_cpuBuffer || !_ready) return;

    OpenGL::size(_cpuWidth, _cpuHeight);
    OpenGL::width = _cpuWidth;
    OpenGL::height = _cpuHeight;
    for(u32 y = 0; y < _cpuHeight; y++) {
      auto* src = _cpuBuffer + y * _cpuWidth;
      auto* dst = OpenGL::buffer + (_cpuHeight - 1 - y) * _cpuWidth;
      for(u32 x = 0; x < _cpuWidth; x++) {
        dst[x] = src[x] | 0xff000000;
      }
    }

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, _cpuWidth, _cpuHeight, getFormat(), getType(), buffer);

    ensureOutputFBO();

    u32 targetWidth = absoluteWidth ? absoluteWidth : _pendingWidth;
    u32 targetHeight = absoluteHeight ? absoluteHeight : _pendingHeight;
    int windowW, windowH;
    SDL_GetWindowSize(_window, &windowW, &windowH);
    u32 x = ((u32)windowW - targetWidth) / 2;
    u32 y = ((u32)windowH - targetHeight) / 2;

    if(_chain != NULL) {
      if(!framebuffer || framebufferWidth != targetWidth || framebufferHeight != targetHeight) {
        if(framebuffer) { glDeleteFramebuffers(1, &framebuffer); framebuffer = 0; }
        if(framebufferTexture) { glDeleteTextures(1, &framebufferTexture); framebufferTexture = 0; }
        framebufferWidth = targetWidth; framebufferHeight = targetHeight;
        glGenFramebuffers(1, &framebuffer);
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        glGenTextures(1, &framebufferTexture);
        glBindTexture(GL_TEXTURE_2D, framebufferTexture);
        framebufferFormat = GL_RGB;
        glTexImage2D(GL_TEXTURE_2D, 0, framebufferFormat, framebufferWidth, framebufferHeight,
                     0, framebufferFormat, GL_UNSIGNED_BYTE, nullptr);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, framebufferTexture, 0);
      }
    } else {
      if(!framebuffer || framebufferWidth != _cpuWidth || framebufferHeight != _cpuHeight) {
        if(framebuffer) { glDeleteFramebuffers(1, &framebuffer); framebuffer = 0; }
        if(framebufferTexture) { glDeleteTextures(1, &framebufferTexture); framebufferTexture = 0; }
        framebufferWidth = _cpuWidth; framebufferHeight = _cpuHeight;
        glGenFramebuffers(1, &framebuffer);
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
      }
    }

    OpenGL::clear(_outputFBO);
    OpenGL::absoluteWidth = _pendingWidth;
    OpenGL::absoluteHeight = _pendingHeight;
    OpenGL::outputX = 0;
    OpenGL::outputY = 0;
    OpenGL::outputWidth = windowW;
    OpenGL::outputHeight = windowH;
    OpenGL::render(_cpuWidth, _cpuHeight, outputX + x, outputY + y, targetWidth, targetHeight, _outputFBO);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    _framePending = false;
  }

  auto outputTexture() -> uintptr override {
    return _outputTexture;
  }

  auto poll() -> void override {}

private:
  auto construct() -> void {}
  auto destruct() -> void { terminate(); }

  auto ensureOutputFBO() -> void {
    int w, h;
    SDL_GetWindowSizeInPixels(_window, &w, &h);

    if(_outputFBO == 0 || _outputFBOWidth != w || _outputFBOHeight != h) {
      if(_outputFBO) glDeleteFramebuffers(1, &_outputFBO);
      if(_outputTexture) glDeleteTextures(1, &_outputTexture);

      _outputFBOWidth = w;
      _outputFBOHeight = h;

      glGenFramebuffers(1, &_outputFBO);
      glGenTextures(1, &_outputTexture);

      glBindTexture(GL_TEXTURE_2D, _outputTexture);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

      glBindFramebuffer(GL_FRAMEBUFFER, _outputFBO);
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, _outputTexture, 0);

      glClearColor(0, 0, 0, 1);
      glClear(GL_COLOR_BUFFER_BIT);
    }
  }

  auto initialize() -> bool {
    terminate();
    if(!_window || !_glContext) return false;

    OpenGL::inputFormat = GL_RGBA8;
    if(!OpenGL::initialize(self.shader)) return false;

    _ready = true;
    return _ready;
  }

  auto terminate() -> void {
    _ready = false;
    OpenGL::terminate();

    delete[] _cpuBuffer;
    _cpuBuffer = nullptr;
    _cpuWidth = 0;
    _cpuHeight = 0;

    if(_outputFBO) { glDeleteFramebuffers(1, &_outputFBO); _outputFBO = 0; }
    if(_outputTexture) { glDeleteTextures(1, &_outputTexture); _outputTexture = 0; }
    _outputFBOWidth = 0;
    _outputFBOHeight = 0;
  }

  SDL_Window* _window = nullptr;
  SDL_GLContext _glContext = nullptr;
  bool _ready = false;

  u32* _cpuBuffer = nullptr;
  u32 _cpuWidth = 0;
  u32 _cpuHeight = 0;
  u32 _pendingWidth = 0;
  u32 _pendingHeight = 0;
  bool _framePending = false;

  GLuint _outputFBO = 0;
  GLuint _outputTexture = 0;
  int _outputFBOWidth = 0;
  int _outputFBOHeight = 0;
};
