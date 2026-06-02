#include <ruby/ruby.hpp>
#include <SDL3/SDL.h>

using namespace nall;

namespace ruby {

auto Audio::create() -> bool {
  SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
  return initialize();
}

auto Audio::clear() -> void {
  if(!_ready) return;
  SDL_ClearAudioStream(static_cast<SDL_AudioStream*>(_stream));
}

auto Audio::level() -> f64 {
  if(!_ready) return 0.0;
  return SDL_GetAudioStreamAvailable(static_cast<SDL_AudioStream*>(_stream)) / ((f64)_bufferSize);
}

auto Audio::output(const f64 samples[]) -> void {
  if(!_ready) return;

  auto* stream = static_cast<SDL_AudioStream*>(_stream);

  if(blocking && frequency > 0) {
    auto bytesRemaining = SDL_GetAudioStreamAvailable(stream);
    while(bytesRemaining > _bufferSize) {
      auto bytesToWait = bytesRemaining - _bufferSize;
      auto bytesPerSample = _bitsPerSample / 8.0;
      auto samplesRemaining = bytesToWait / bytesPerSample;
      auto secondsRemaining = samplesRemaining / frequency;
      usleep(secondsRemaining * 1000000);
      bytesRemaining = SDL_GetAudioStreamAvailable(stream);
    }
  }

  std::unique_ptr<f32[]> out = std::make_unique<f32[]>(channels);
  for(auto n : range(channels)) out[n] = samples[n];
  SDL_PutAudioStreamData(stream, &out[0], channels * sizeof(f32));
}

auto Audio::initialize() -> bool {
  terminate();

#if defined(PLATFORM_WINDOWS)
  timeBeginPeriod(1);
#endif

  if(!tryInitWithDriver("pulseaudio")) {
    if(!tryInitWithDriver("pipewire")) {
      tryInitWithDriver(nullptr);
    }
  }

  if(!_ready) {
    fprintf(stderr, "SDL audio: failed to open audio device\n");
  }
  return _ready;
}

auto Audio::tryInitWithDriver(const char* driver) -> bool {
  if(driver) SDL_SetHint(SDL_HINT_AUDIO_DRIVER, driver);

  if(!SDL_InitSubSystem(SDL_INIT_AUDIO)) return false;

  SDL_AudioSpec spec;
  spec.format = SDL_AUDIO_F32;
  spec.channels = 2;
  spec.freq = frequency;
  auto desired_samples = (latency * frequency) / 1000.0f;
  string desired_samples_string = (string)desired_samples;
  SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, desired_samples_string);

  auto* stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
  if(!stream) { SDL_QuitSubSystem(SDL_INIT_AUDIO); return false; }

  auto device = SDL_GetAudioStreamDevice(stream);
  if(!device) { SDL_QuitSubSystem(SDL_INIT_AUDIO); return false; }

  SDL_ResumeAudioDevice(device);
  _stream = stream;
  _device = reinterpret_cast<void*>(static_cast<uintptr_t>(device));
  frequency = spec.freq;
  channels = spec.channels;
  int bufferFrameSize;
  SDL_GetAudioDeviceFormat(device, &spec, &bufferFrameSize);
  _bitsPerSample = SDL_AUDIO_BITSIZE(spec.format);
  _bufferSize = bufferFrameSize * channels * 4;

  _ready = true;
  clear();
  return true;
}

auto Audio::terminate() -> void {
#if defined(PLATFORM_WINDOWS)
  timeEndPeriod(1);
#endif
  _ready = false;
  if(_device) {
    SDL_CloseAudioDevice(static_cast<SDL_AudioDeviceID>(reinterpret_cast<uintptr_t>(_device)));
    _device = nullptr;
  }
  SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

} // namespace ruby
