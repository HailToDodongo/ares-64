//QuickJS host for ares-test. Follows the pyrite64 jsNodeHost.cpp pattern: raw
//QuickJS C API, global-script eval mode, no event loop — every binding is
//synchronous and the emulator advances inline inside the wait*() calls.

#include "js-host.hpp"
#include "runner.hpp"

#include <quickjs.h>

#include <nall/decode/png.hpp>
#include <nall/decode/wav.hpp>
#include <nall/encode/png.hpp>
#include <nall/encode/wav.hpp>
#include <nall/hash/sha256.hpp>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace nall;

namespace {

JSRuntime* g_rt = nullptr;
JSContext* g_ctx = nullptr;

auto jsStr(JSContext* c, JSValueConst v) -> string {
  const char* s = JS_ToCString(c, v);
  string result = s ? s : "";
  if(s) JS_FreeCString(c, s);
  return result;
}

//print an error value (consumed) with its stack trace
auto printErrorValue(JSContext* c, JSValue e) -> void {
  string msg = jsStr(c, e);
  JSValue stack = JS_GetPropertyStr(c, e, "stack");
  if(JS_IsString(stack)) msg.append("\n", jsStr(c, stack));
  JS_FreeValue(c, stack);
  JS_FreeValue(c, e);
  print(stderr, "[ares-test] uncaught exception: ", msg, "\n");
}

//print the pending uncaught exception with its stack trace
auto printException(JSContext* c) -> void {
  printErrorValue(c, JS_GetException(c));
}

//--- ES module support -----------------------------------------------------

//resolve "./x.js" / "../x.js" / "x.js" relative to the importing module's path;
//absolute paths pass through. The normalized path is also the module cache key.
auto moduleNormalize(JSContext* c, const char* base, const char* name, void*) -> char* {
  string basePath = base;
  string resolved;
  if(name[0] == '/') {
    resolved = name;
  } else {
    resolved = {Location::dir(basePath), name};
  }

  //lexically collapse "." and ".." segments so the cache key is canonical
  bool absolute = resolved.beginsWith("/");
  std::vector<string> stack;
  for(auto& segment : nall::split(resolved, "/")) {
    if(!segment || segment == ".") continue;
    if(segment == ".." && !stack.empty() && stack.back() != "..") { stack.pop_back(); continue; }
    stack.push_back(segment);
  }
  string canonical = absolute ? "/" : "";
  for(u32 i = 0; i < stack.size(); i++) {
    if(i) canonical.append("/");
    canonical.append(stack[i]);
  }

  char* out = (char*)js_malloc(c, canonical.size() + 1);
  if(out) memcpy(out, canonical.data(), canonical.size() + 1);
  return out;
}

auto moduleLoader(JSContext* c, const char* name, void*) -> JSModuleDef* {
  string source = string::read(name);
  if(!source) {
    JS_ThrowReferenceError(c, "could not load module '%s'", name);
    return nullptr;
  }
  JSValue func = JS_Eval(c, source.data(), source.size(), name,
                         JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
  if(JS_IsException(func)) return nullptr;
  auto module = (JSModuleDef*)JS_VALUE_GET_PTR(func);
  JS_FreeValue(c, func);  //the module definition stays owned by the context
  return module;
}

auto setFn(JSContext* c, JSValue obj, const char* name, JSCFunction* fn, int len) -> void {
  JS_SetPropertyStr(c, obj, name, JS_NewCFunction(c, fn, name, len));
}

auto throwError(JSContext* c, const string& message) -> JSValue {
  return JS_ThrowPlainError(c, "%s", message.data());
}

//shared guard for calls that advance emulation
auto requireRunnable(JSContext* c) -> JSValue {
  if(!emulatorRunner.loaded()) return throwError(c, "no ROM loaded (call ares.loadRom first)");
  if(emulatorRunner.paused) return throwError(c, "emulator is paused (call ares.resume first)");
  return JS_UNDEFINED;
}

//--- console ---------------------------------------------------------------

auto js_console_log(JSContext* c, JSValueConst, int argc, JSValueConst* argv) -> JSValue {
  string line;
  for(int i = 0; i < argc; i++) {
    if(i) line.append(" ");
    if(JS_IsObject(argv[i]) && !JS_IsFunction(c, argv[i])) {
      JSValue json = JS_JSONStringify(c, argv[i], JS_UNDEFINED, JS_UNDEFINED);
      if(!JS_IsException(json) && !JS_IsUndefined(json)) {
        line.append(jsStr(c, json));
        JS_FreeValue(c, json);
        continue;
      }
      JS_FreeValue(c, json);
    }
    line.append(jsStr(c, argv[i]));
  }
  print(line, "\n");
  return JS_UNDEFINED;
}

//--- lifecycle -------------------------------------------------------------

auto js_loadRom(JSContext* c, JSValueConst, int argc, JSValueConst* argv) -> JSValue {
  if(argc < 1) return throwError(c, "loadRom(path) requires a path");
  if(auto error = emulatorRunner.loadRom(jsStr(c, argv[0]))) return throwError(c, error);
  return JS_UNDEFINED;
}

auto js_closeRom(JSContext* c, JSValueConst, int, JSValueConst*) -> JSValue {
  emulatorRunner.closeRom();
  return JS_UNDEFINED;
}

auto js_reset(JSContext* c, JSValueConst, int argc, JSValueConst* argv) -> JSValue {
  if(!emulatorRunner.loaded()) return throwError(c, "no ROM loaded");
  bool hard = argc >= 1 && JS_ToBool(c, argv[0]);
  emulatorRunner.reset(hard);
  return JS_UNDEFINED;
}

auto js_pause(JSContext*, JSValueConst, int, JSValueConst*) -> JSValue {
  emulatorRunner.paused = true;
  return JS_UNDEFINED;
}

auto js_resume(JSContext*, JSValueConst, int, JSValueConst*) -> JSValue {
  emulatorRunner.paused = false;
  return JS_UNDEFINED;
}

auto js_isPaused(JSContext*, JSValueConst, int, JSValueConst*) -> JSValue {
  return JS_NewBool(g_ctx, emulatorRunner.paused);
}

auto js_setRenderer(JSContext* c, JSValueConst, int argc, JSValueConst* argv) -> JSValue {
  if(argc < 1) return throwError(c, "setRenderer(name) requires a name");
  if(emulatorRunner.loaded()) return throwError(c, "setRenderer must be called before loadRom");
  string name = jsStr(c, argv[0]);
  if(name != "angrylion" && name != "none") {
    return throwError(c, {"unknown renderer (angrylion|none): ", name});
  }
  emulatorRunner.renderer = name;
  return JS_UNDEFINED;
}

auto js_setHomebrew(JSContext* c, JSValueConst, int argc, JSValueConst* argv) -> JSValue {
  if(emulatorRunner.loaded()) return throwError(c, "setHomebrew must be called before loadRom");
  emulatorRunner.homebrewMode = argc < 1 || JS_ToBool(c, argv[0]);
  return JS_UNDEFINED;
}

//--- time ------------------------------------------------------------------

auto js_wait(JSContext* c, JSValueConst, int argc, JSValueConst* argv) -> JSValue {
  JSValue guard = requireRunnable(c);
  if(JS_IsException(guard)) return guard;
  double seconds = 0;
  if(argc < 1 || JS_ToFloat64(c, &seconds, argv[0]) < 0) return JS_EXCEPTION;
  if(seconds < 0) return throwError(c, "wait(seconds): seconds must be >= 0");
  emulatorRunner.runFrames((u32)std::llround(seconds * emulatorRunner.refreshRate()));
  return JS_UNDEFINED;
}

auto js_waitFrames(JSContext* c, JSValueConst, int argc, JSValueConst* argv) -> JSValue {
  JSValue guard = requireRunnable(c);
  if(JS_IsException(guard)) return guard;
  u32 frames = 1;
  if(argc >= 1) {
    int64_t n = 0;
    if(JS_ToInt64(c, &n, argv[0]) < 0) return JS_EXCEPTION;
    if(n < 0) return throwError(c, "waitFrames(n): n must be >= 0");
    frames = (u32)n;
  }
  emulatorRunner.runFrames(frames);
  return JS_UNDEFINED;
}

auto js_waitVI(JSContext* c, JSValueConst, int, JSValueConst*) -> JSValue {
  JSValue guard = requireRunnable(c);
  if(JS_IsException(guard)) return guard;
  emulatorRunner.runFrames(1);
  return JS_UNDEFINED;
}

auto js_frameCount(JSContext*, JSValueConst, int, JSValueConst*) -> JSValue {
  return JS_NewInt64(g_ctx, emulatorRunner.frame());
}

//--- controller ------------------------------------------------------------

auto controllerPort(JSContext* c, JSValueConst self) -> int {
  JSValue v = JS_GetPropertyStr(c, self, "port");
  int32_t port = 0;
  JS_ToInt32(c, &port, v);
  JS_FreeValue(c, v);
  return port - 1;  //JS API is 1-based
}

auto js_ctl_hold(JSContext* c, JSValueConst self, int argc, JSValueConst* argv) -> JSValue {
  if(argc < 1) return throwError(c, "hold(button) requires a button name");
  string name = jsStr(c, argv[0]);
  if(!emulatorRunner.setInput(controllerPort(c, self), name, 1)) {
    return throwError(c, {"unknown button: ", name});
  }
  return JS_DupValue(c, self);
}

auto js_ctl_release(JSContext* c, JSValueConst self, int argc, JSValueConst* argv) -> JSValue {
  if(argc < 1) return throwError(c, "release(button) requires a button name");
  string name = jsStr(c, argv[0]);
  if(!emulatorRunner.setInput(controllerPort(c, self), name, 0)) {
    return throwError(c, {"unknown button: ", name});
  }
  return JS_DupValue(c, self);
}

auto js_ctl_stick(JSContext* c, JSValueConst self, int argc, JSValueConst* argv) -> JSValue {
  double x = 0, y = 0;
  if(argc >= 1 && JS_ToFloat64(c, &x, argv[0]) < 0) return JS_EXCEPTION;
  if(argc >= 2 && JS_ToFloat64(c, &y, argv[1]) < 0) return JS_EXCEPTION;
  x = max(-1.0, min(+1.0, x));
  y = max(-1.0, min(+1.0, y));
  int port = controllerPort(c, self);
  emulatorRunner.setInput(port, "X-Axis", (s64)std::llround(x * 32767.0));
  emulatorRunner.setInput(port, "Y-Axis", (s64)std::llround(y * 32767.0));
  return JS_DupValue(c, self);
}

auto js_ctl_clear(JSContext* c, JSValueConst self, int, JSValueConst*) -> JSValue {
  emulatorRunner.clearInputs(controllerPort(c, self));
  return JS_DupValue(c, self);
}

auto js_controller(JSContext* c, JSValueConst, int argc, JSValueConst* argv) -> JSValue {
  int32_t port = 1;
  if(argc >= 1 && JS_ToInt32(c, &port, argv[0]) < 0) return JS_EXCEPTION;
  if(port < 1 || port > 4) return throwError(c, "controller(port): port must be 1..4");
  JSValue obj = JS_NewObject(c);
  JS_SetPropertyStr(c, obj, "port", JS_NewInt32(c, port));
  setFn(c, obj, "hold", js_ctl_hold, 1);
  setFn(c, obj, "release", js_ctl_release, 1);
  setFn(c, obj, "stick", js_ctl_stick, 2);
  setFn(c, obj, "clear", js_ctl_clear, 0);
  return obj;
}

//--- capture: image and audio objects --------------------------------------
//Both factories produce plain objects carrying their data as an ArrayBuffer plus
//save(path) and compare(other, tolerance?) methods, so captured and loaded media
//are interchangeable (snapshot testing: screenshot vs loadImage golden).

//plain C++ views used by load/compare
struct ImagePixels {
  u32 width = 0, height = 0;
  std::vector<u8> rgba;
};
struct AudioSamples {
  u32 frequency = 0;
  std::vector<s16> frames;  //interleaved LRLR...
};

//read {width, height, data} back out of an image object
auto getImagePixels(JSContext* c, JSValueConst obj, u32& width, u32& height, u8*& bytes, size_t& size) -> bool {
  int32_t w = 0, h = 0;
  JSValue wv = JS_GetPropertyStr(c, obj, "width");
  JSValue hv = JS_GetPropertyStr(c, obj, "height");
  JS_ToInt32(c, &w, wv);
  JS_ToInt32(c, &h, hv);
  JS_FreeValue(c, wv);
  JS_FreeValue(c, hv);
  JSValue data = JS_GetPropertyStr(c, obj, "data");
  bytes = JS_GetArrayBuffer(c, &size, data);
  JS_FreeValue(c, data);  //the ArrayBuffer stays alive via `obj`
  width = w;
  height = h;
  return bytes && w > 0 && h > 0 && size == (size_t)w * h * 4;
}

//read {frequency, data} back out of an audio object
auto getAudioSamples(JSContext* c, JSValueConst obj, u32& frequency, const s16*& frames, size_t& count) -> bool {
  int32_t f = 0;
  JSValue fv = JS_GetPropertyStr(c, obj, "frequency");
  JS_ToInt32(c, &f, fv);
  JS_FreeValue(c, fv);
  JSValue data = JS_GetPropertyStr(c, obj, "data");
  size_t size = 0;
  u8* bytes = JS_GetArrayBuffer(c, &size, data);
  JS_FreeValue(c, data);
  frequency = f;
  frames = (const s16*)bytes;
  count = size / 4;
  return bytes && f > 0 && size % 4 == 0;
}

auto decodePng(const string& path, ImagePixels& out) -> string {
  Decode::PNG png;
  if(!png.load(path)) return {"failed to load PNG: ", path};
  auto& info = png.info;
  if(info.bitDepth != 8) return {"unsupported PNG bit depth (want 8): ", path};
  u32 width = info.width, height = info.height;
  out.width = width;
  out.height = height;
  out.rgba.resize((size_t)width * height * 4);
  //optipng may have rewritten our RGB output as grayscale or palette; accept all
  //8-bit color types so saved goldens always round-trip
  for(u32 y : range(height)) {
    const u8* src = png.data + (size_t)y * info.pitch;
    u8* dst = out.rgba.data() + (size_t)y * width * 4;
    for(u32 x : range(width)) {
      u8 r, g, b, a = 255;
      switch(info.colorType) {
      case 0: r = g = b = src[x]; break;                                        //grayscale
      case 2: r = src[x*3+0]; g = src[x*3+1]; b = src[x*3+2]; break;            //RGB
      case 3: r = info.palette[src[x]][0]; g = info.palette[src[x]][1];
              b = info.palette[src[x]][2]; break;                               //palette
      case 4: r = g = b = src[x*2+0]; a = src[x*2+1]; break;                    //gray+alpha
      case 6: r = src[x*4+0]; g = src[x*4+1]; b = src[x*4+2]; a = src[x*4+3]; break;  //RGBA
      default: return {"unsupported PNG color type: ", path};
      }
      dst[x*4+0] = r; dst[x*4+1] = g; dst[x*4+2] = b; dst[x*4+3] = a;
    }
  }
  return {};
}

auto decodeWav(const string& path, AudioSamples& out) -> string {
  Decode::WAV wav;
  if(!wav.open(path)) return {"failed to load WAV (PCM only): ", path};
  if(wav.bitrate != 16 || (wav.channels != 1 && wav.channels != 2)) {
    wav.close();
    return {"unsupported WAV format (want 16-bit mono/stereo): ", path};
  }
  out.frequency = wav.frequency;
  out.frames.resize((size_t)wav.samples * 2);
  for(size_t i = 0; i < wav.samples; i++) {
    u64 frame = wav.read();
    s16 left = (s16)(frame & 0xffff);
    s16 right = wav.channels == 2 ? (s16)(frame >> 16 & 0xffff) : left;
    out.frames[i*2+0] = left;
    out.frames[i*2+1] = right;
  }
  wav.close();
  return {};
}

//hash over the RGB byte stream (alpha excluded): the golden-comparison key, and
//identical for captured and loaded images
auto imageSha256(const std::vector<u8>& rgba) -> string {
  std::vector<u8> rgb;
  rgb.reserve(rgba.size() / 4 * 3);
  for(size_t i = 0; i < rgba.size(); i += 4) {
    rgb.push_back(rgba[i + 0]);
    rgb.push_back(rgba[i + 1]);
    rgb.push_back(rgba[i + 2]);
  }
  Hash::SHA256 hash{{rgb.data(), rgb.size()}};
  return hash.digest();
}

auto js_img_save(JSContext* c, JSValueConst self, int argc, JSValueConst* argv) -> JSValue;
auto js_img_compare(JSContext* c, JSValueConst self, int argc, JSValueConst* argv) -> JSValue;
auto js_audio_save(JSContext* c, JSValueConst self, int argc, JSValueConst* argv) -> JSValue;
auto js_audio_compare(JSContext* c, JSValueConst self, int argc, JSValueConst* argv) -> JSValue;

auto makeImageObject(JSContext* c, u32 width, u32 height, const std::vector<u8>& rgba) -> JSValue {
  JSValue obj = JS_NewObject(c);
  JS_SetPropertyStr(c, obj, "width", JS_NewInt32(c, width));
  JS_SetPropertyStr(c, obj, "height", JS_NewInt32(c, height));
  JS_SetPropertyStr(c, obj, "sha256", JS_NewString(c, imageSha256(rgba)));
  JS_SetPropertyStr(c, obj, "data", JS_NewArrayBufferCopy(c, rgba.data(), rgba.size()));
  setFn(c, obj, "save", js_img_save, 1);
  setFn(c, obj, "compare", js_img_compare, 2);
  return obj;
}

auto makeAudioObject(JSContext* c, u32 frequency, const std::vector<s16>& frames) -> JSValue {
  JSValue obj = JS_NewObject(c);
  JS_SetPropertyStr(c, obj, "samples", JS_NewInt64(c, frames.size() / 2));
  JS_SetPropertyStr(c, obj, "frequency", JS_NewInt32(c, frequency));
  JS_SetPropertyStr(c, obj, "data",
    JS_NewArrayBufferCopy(c, (const u8*)frames.data(), frames.size() * sizeof(s16)));
  setFn(c, obj, "save", js_audio_save, 1);
  setFn(c, obj, "compare", js_audio_compare, 2);
  return obj;
}

//image.save(path): encode the RGBA buffer carried on `this` as a PNG
auto js_img_save(JSContext* c, JSValueConst self, int argc, JSValueConst* argv) -> JSValue {
  if(argc < 1) return throwError(c, "save(path) requires a path");
  string path = jsStr(c, argv[0]);
  u32 width, height; u8* bytes; size_t size;
  if(!getImagePixels(c, self, width, height, bytes, size)) {
    return throwError(c, "save: image data buffer is missing or has the wrong size");
  }
  //repack RGBA bytes into the ARGB words the PNG encoder expects
  std::vector<u32> argb((size_t)width * height);
  for(size_t i = 0; i < argb.size(); i++) {
    argb[i] = (u32)bytes[i * 4 + 0] << 16 | (u32)bytes[i * 4 + 1] << 8 | (u32)bytes[i * 4 + 2];
  }
  if(!Encode::PNG::RGB8(path, argb.data(), width * sizeof(u32), width, height)) {
    return throwError(c, {"failed to write PNG: ", path});
  }
  return JS_UNDEFINED;
}

//image.compare(other, tolerance?): other is an image object or a PNG path.
//tolerance = max per-channel delta (RGB) still considered equal (default 0).
//-> {match, reason?, diffPixels, totalPixels, maxDelta, avgDelta}
auto js_img_compare(JSContext* c, JSValueConst self, int argc, JSValueConst* argv) -> JSValue {
  if(argc < 1) return throwError(c, "compare(other) requires an image object or PNG path");

  u32 widthA, heightA; u8* bytesA; size_t sizeA;
  if(!getImagePixels(c, self, widthA, heightA, bytesA, sizeA)) {
    return throwError(c, "compare: image data buffer is missing or has the wrong size");
  }

  ImagePixels loaded;
  u32 widthB, heightB; const u8* bytesB;
  if(JS_IsString(argv[0])) {
    if(auto error = decodePng(jsStr(c, argv[0]), loaded)) return throwError(c, error);
    widthB = loaded.width; heightB = loaded.height; bytesB = loaded.rgba.data();
  } else {
    u8* b; size_t size;
    if(!getImagePixels(c, argv[0], widthB, heightB, b, size)) {
      return throwError(c, "compare: other is not an image object or PNG path");
    }
    bytesB = b;
  }

  int32_t tolerance = 0;
  if(argc >= 2 && JS_ToInt32(c, &tolerance, argv[1]) < 0) return JS_EXCEPTION;

  JSValue obj = JS_NewObject(c);
  if(widthA != widthB || heightA != heightB) {
    JS_SetPropertyStr(c, obj, "match", JS_NewBool(c, false));
    JS_SetPropertyStr(c, obj, "reason", JS_NewString(c,
      string{"size mismatch: ", widthA, "x", heightA, " vs ", widthB, "x", heightB}));
    return obj;
  }

  u64 diffPixels = 0, totalDelta = 0;
  u32 maxDelta = 0;
  size_t pixels = (size_t)widthA * heightA;
  for(size_t i = 0; i < pixels; i++) {
    u32 delta = 0;  //max channel delta of this pixel, alpha ignored
    for(u32 ch : range(3)) {
      u32 d = abs((int)bytesA[i * 4 + ch] - (int)bytesB[i * 4 + ch]);
      delta = max(delta, d);
    }
    if(delta > (u32)tolerance) diffPixels++;
    maxDelta = max(maxDelta, delta);
    totalDelta += delta;
  }

  JS_SetPropertyStr(c, obj, "match", JS_NewBool(c, diffPixels == 0));
  JS_SetPropertyStr(c, obj, "diffPixels", JS_NewInt64(c, diffPixels));
  JS_SetPropertyStr(c, obj, "totalPixels", JS_NewInt64(c, pixels));
  JS_SetPropertyStr(c, obj, "maxDelta", JS_NewInt32(c, maxDelta));
  JS_SetPropertyStr(c, obj, "avgDelta", JS_NewFloat64(c, pixels ? (double)totalDelta / pixels : 0.0));
  return obj;
}

//audio.save(path): write the interleaved s16 buffer carried on `this` as a WAV
auto js_audio_save(JSContext* c, JSValueConst self, int argc, JSValueConst* argv) -> JSValue {
  if(argc < 1) return throwError(c, "save(path) requires a path");
  string path = jsStr(c, argv[0]);
  u32 frequency; const s16* frames; size_t count;
  if(!getAudioSamples(c, self, frequency, frames, count)) {
    return throwError(c, "save: audio data buffer is missing or malformed");
  }
  //de-interleave LRLR... s16 frames for the WAV encoder
  std::vector<s16> left(count), right(count);
  for(size_t i = 0; i < count; i++) {
    left[i] = frames[i * 2 + 0];
    right[i] = frames[i * 2 + 1];
  }
  if(!Encode::WAV::stereo<s16>(path, {left.data(), count}, {right.data(), count}, frequency)) {
    return throwError(c, {"failed to write WAV: ", path});
  }
  return JS_UNDEFINED;
}

//audio.compare(other, tolerance?): other is an audio object or a WAV path.
//tolerance = max per-sample delta (s16 units) still considered equal (default 0).
//-> {match, reason?, diffSamples, totalSamples, maxDelta, avgDelta}
auto js_audio_compare(JSContext* c, JSValueConst self, int argc, JSValueConst* argv) -> JSValue {
  if(argc < 1) return throwError(c, "compare(other) requires an audio object or WAV path");

  u32 frequencyA; const s16* framesA; size_t countA;
  if(!getAudioSamples(c, self, frequencyA, framesA, countA)) {
    return throwError(c, "compare: audio data buffer is missing or malformed");
  }

  AudioSamples loaded;
  u32 frequencyB; const s16* framesB; size_t countB;
  if(JS_IsString(argv[0])) {
    if(auto error = decodeWav(jsStr(c, argv[0]), loaded)) return throwError(c, error);
    frequencyB = loaded.frequency; framesB = loaded.frames.data(); countB = loaded.frames.size() / 2;
  } else {
    if(!getAudioSamples(c, argv[0], frequencyB, framesB, countB)) {
      return throwError(c, "compare: other is not an audio object or WAV path");
    }
  }

  int32_t tolerance = 0;
  if(argc >= 2 && JS_ToInt32(c, &tolerance, argv[1]) < 0) return JS_EXCEPTION;

  JSValue obj = JS_NewObject(c);
  if(frequencyA != frequencyB) {
    JS_SetPropertyStr(c, obj, "match", JS_NewBool(c, false));
    JS_SetPropertyStr(c, obj, "reason", JS_NewString(c,
      string{"frequency mismatch: ", frequencyA, " vs ", frequencyB}));
    return obj;
  }
  if(countA != countB) {
    JS_SetPropertyStr(c, obj, "match", JS_NewBool(c, false));
    JS_SetPropertyStr(c, obj, "reason", JS_NewString(c,
      string{"sample count mismatch: ", (u64)countA, " vs ", (u64)countB}));
    return obj;
  }

  u64 diffSamples = 0, totalDelta = 0;
  u32 maxDelta = 0;
  for(size_t i = 0; i < countA; i++) {
    u32 delta = 0;  //max channel delta of this frame
    for(u32 ch : range(2)) {
      u32 d = abs((int)framesA[i * 2 + ch] - (int)framesB[i * 2 + ch]);
      delta = max(delta, d);
    }
    if(delta > (u32)tolerance) diffSamples++;
    maxDelta = max(maxDelta, delta);
    totalDelta += delta;
  }

  JS_SetPropertyStr(c, obj, "match", JS_NewBool(c, diffSamples == 0));
  JS_SetPropertyStr(c, obj, "diffSamples", JS_NewInt64(c, diffSamples));
  JS_SetPropertyStr(c, obj, "totalSamples", JS_NewInt64(c, countA));
  JS_SetPropertyStr(c, obj, "maxDelta", JS_NewInt32(c, maxDelta));
  JS_SetPropertyStr(c, obj, "avgDelta", JS_NewFloat64(c, countA ? (double)totalDelta / countA : 0.0));
  return obj;
}

//--- capture: ares-level entry points --------------------------------------

auto js_screenshot(JSContext* c, JSValueConst, int, JSValueConst*) -> JSValue {
  EmulatorRunner::ScreenshotResult result;
  if(auto error = emulatorRunner.screenshot(result)) return throwError(c, error);
  return makeImageObject(c, result.width, result.height, result.rgba);
}

auto js_loadImage(JSContext* c, JSValueConst, int argc, JSValueConst* argv) -> JSValue {
  if(argc < 1) return throwError(c, "loadImage(path) requires a path");
  ImagePixels image;
  if(auto error = decodePng(jsStr(c, argv[0]), image)) return throwError(c, error);
  return makeImageObject(c, image.width, image.height, image.rgba);
}

auto js_startAudio(JSContext* c, JSValueConst, int argc, JSValueConst* argv) -> JSValue {
  //default records at the AI's current output rate; pass a number to force one
  int64_t rate = 0;
  if(argc >= 1 && !JS_IsUndefined(argv[0])) {
    if(JS_ToInt64(c, &rate, argv[0]) < 0) return JS_EXCEPTION;
    if(rate < 1000 || rate > 192000) return throwError(c, "startAudio(rate): rate must be 1000..192000");
  }
  if(auto error = emulatorRunner.startAudio((u32)rate)) return throwError(c, error);
  return JS_UNDEFINED;
}

auto js_stopAudio(JSContext* c, JSValueConst, int, JSValueConst*) -> JSValue {
  EmulatorRunner::AudioRecording rec;
  if(auto error = emulatorRunner.stopAudio(rec)) return throwError(c, error);
  //interleave to LRLR... s16 for the JS-visible buffer
  std::vector<s16> frames(rec.left.size() * 2);
  for(size_t i = 0; i < rec.left.size(); i++) {
    frames[i * 2 + 0] = rec.left[i];
    frames[i * 2 + 1] = rec.right[i];
  }
  return makeAudioObject(c, rec.frequency, frames);
}

auto js_loadAudio(JSContext* c, JSValueConst, int argc, JSValueConst* argv) -> JSValue {
  if(argc < 1) return throwError(c, "loadAudio(path) requires a path");
  AudioSamples audio;
  if(auto error = decodeWav(jsStr(c, argv[0]), audio)) return throwError(c, error);
  return makeAudioObject(c, audio.frequency, audio.frames);
}

//--- ISViewer log ----------------------------------------------------------

auto js_log(JSContext* c, JSValueConst, int, JSValueConst*) -> JSValue {
  return JS_NewStringLen(c, emulatorRunner.logText.data(), emulatorRunner.logText.size());
}

auto js_clearLog(JSContext*, JSValueConst, int, JSValueConst*) -> JSValue {
  emulatorRunner.logText = {};
  return JS_UNDEFINED;
}

auto js_waitLog(JSContext* c, JSValueConst, int argc, JSValueConst* argv) -> JSValue {
  JSValue guard = requireRunnable(c);
  if(JS_IsException(guard)) return guard;
  if(argc < 1) return throwError(c, "waitLog(marker, maxSeconds) requires a marker");
  string marker = jsStr(c, argv[0]);
  if(!marker) return throwError(c, "waitLog: marker must be non-empty");
  double maxSeconds = 10.0;
  if(argc >= 2 && JS_ToFloat64(c, &maxSeconds, argv[1]) < 0) return JS_EXCEPTION;
  u32 maxFrames = (u32)std::llround(maxSeconds * emulatorRunner.refreshRate());
  for(u32 frame = 0; frame <= maxFrames; frame++) {
    if(emulatorRunner.logText.find(marker)) return JS_NewBool(c, true);
    if(frame == maxFrames || emulatorRunner.shutdownRequested.load()) break;
    emulatorRunner.runFrames(1);
  }
  return JS_NewBool(c, false);
}

//--- misc ------------------------------------------------------------------

auto js_exit(JSContext* c, JSValueConst, int argc, JSValueConst* argv) -> JSValue {
  int32_t code = 0;
  if(argc >= 1) JS_ToInt32(c, &code, argv[0]);
  emulatorRunner.closeRom();
  fflush(stdout);
  fflush(stderr);
  ::exit(code);
}

}  //namespace

auto jsHostInit(const std::vector<string>& scriptArgs) -> bool {
  g_rt = JS_NewRuntime();
  if(!g_rt) return false;
  g_ctx = JS_NewContext(g_rt);
  if(!g_ctx) { JS_FreeRuntime(g_rt); g_rt = nullptr; return false; }
  JS_SetModuleLoaderFunc(g_rt, moduleNormalize, moduleLoader, nullptr);

  JSValue global = JS_GetGlobalObject(g_ctx);

  JSValue console = JS_NewObject(g_ctx);
  setFn(g_ctx, console, "log", js_console_log, 1);
  setFn(g_ctx, console, "error", js_console_log, 1);
  JS_SetPropertyStr(g_ctx, global, "console", console);

  JSValue ares = JS_NewObject(g_ctx);
  setFn(g_ctx, ares, "loadRom", js_loadRom, 1);
  setFn(g_ctx, ares, "closeRom", js_closeRom, 0);
  setFn(g_ctx, ares, "reset", js_reset, 1);
  setFn(g_ctx, ares, "pause", js_pause, 0);
  setFn(g_ctx, ares, "resume", js_resume, 0);
  setFn(g_ctx, ares, "isPaused", js_isPaused, 0);
  setFn(g_ctx, ares, "setRenderer", js_setRenderer, 1);
  setFn(g_ctx, ares, "setHomebrew", js_setHomebrew, 1);
  setFn(g_ctx, ares, "wait", js_wait, 1);
  setFn(g_ctx, ares, "waitFrames", js_waitFrames, 1);
  setFn(g_ctx, ares, "waitVI", js_waitVI, 0);
  setFn(g_ctx, ares, "frameCount", js_frameCount, 0);
  setFn(g_ctx, ares, "controller", js_controller, 1);
  setFn(g_ctx, ares, "screenshot", js_screenshot, 0);
  setFn(g_ctx, ares, "loadImage", js_loadImage, 1);
  setFn(g_ctx, ares, "startAudio", js_startAudio, 1);
  setFn(g_ctx, ares, "stopAudio", js_stopAudio, 0);
  setFn(g_ctx, ares, "loadAudio", js_loadAudio, 1);
  setFn(g_ctx, ares, "log", js_log, 0);
  setFn(g_ctx, ares, "clearLog", js_clearLog, 0);
  setFn(g_ctx, ares, "waitLog", js_waitLog, 2);
  setFn(g_ctx, ares, "exit", js_exit, 1);

  JSValue args = JS_NewArray(g_ctx);
  for(u32 i = 0; i < scriptArgs.size(); i++) {
    JS_SetPropertyUint32(g_ctx, args, i, JS_NewString(g_ctx, scriptArgs[i]));
  }
  JS_SetPropertyStr(g_ctx, ares, "args", args);

  JS_SetPropertyStr(g_ctx, global, "ares", ares);
  JS_FreeValue(g_ctx, global);
  return true;
}

auto jsHostEvalFile(const string& path) -> bool {
  //string::read null-terminates, which JS_Eval requires of its input buffer
  string source = string::read(path);
  if(!source) {
    print(stderr, "[ares-test] error: cannot read script: ", path, "\n");
    return false;
  }

  //scripts using import/export are evaluated as ES modules (relative imports are
  //resolved against the importing file); everything else stays a plain global
  //script for byte-compatibility with non-module scripts
  bool isModule = JS_DetectModule(source.data(), source.size());
  JSValue result = JS_Eval(g_ctx, source.data(), source.size(), path.data(),
                           isModule ? JS_EVAL_TYPE_MODULE : JS_EVAL_TYPE_GLOBAL);
  bool ok = !JS_IsException(result);
  if(!ok) printException(g_ctx);

  if(ok && isModule) {
    //module evaluation is promise-based per spec; our API is fully synchronous, so
    //draining the job queue completes the graph, then the promise holds any error
    for(JSContext* pending = nullptr; JS_ExecutePendingJob(g_rt, &pending) > 0;);
    if(JS_PromiseState(g_ctx, result) == JS_PROMISE_REJECTED) {
      printErrorValue(g_ctx, JS_PromiseResult(g_ctx, result));
      ok = false;
    }
  }

  JS_FreeValue(g_ctx, result);
  return ok;
}

auto jsHostShutdown() -> void {
  if(g_ctx) JS_FreeContext(g_ctx);
  if(g_rt) JS_FreeRuntime(g_rt);
  g_ctx = nullptr;
  g_rt = nullptr;
}
