#pragma once

struct Audio {
  auto create() -> bool;
  auto ready() -> bool { return _ready; }

  auto hasBlocking() -> bool { return true; }
  auto hasDynamic() -> bool { return true; }

  auto setBlocking(bool block) -> bool { blocking = block; clear(); return true; }
  auto setDynamic(bool dyn) -> bool { dynamic = dyn; return true; }
  auto setFrequency(u32 freq) -> bool { if(freq) frequency = freq; return initialize(); }
  auto setLatency(u32 lat) -> bool { if(lat) latency = lat; return initialize(); }
  auto setChannels(u32 ch) -> bool { channels = ch; return initialize(); }

  auto clear() -> void;
  auto level() -> f64;
  auto output(const f64 samples[]) -> void;

  bool blocking = true;
  bool dynamic = false;
  u32 channels = 2;
  u32 frequency = 48000;
  u32 latency = 20;

private:
  auto initialize() -> bool;
  auto terminate() -> void;
  auto tryInitWithDriver(const char* driver) -> bool;

  bool _ready = false;
  void* _device = nullptr;
  void* _stream = nullptr;
  u32 _bufferSize = 0;
  double _bitsPerSample = 0;
};
