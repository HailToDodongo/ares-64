// SDL3 GPU is the only video backend.
#include <SDL3/SDL.h>
#include <ruby/video/sdl3.cpp>

#include <memory>

namespace ruby {

auto Video::setFullScreen(bool fullScreen) -> bool {
  lock_guard<recursive_mutex> lock(mutex);
  if(instance->fullScreen == fullScreen) return true;
  if(!instance->hasFullScreen()) return false;
  if(!instance->setFullScreen(instance->fullScreen = fullScreen)) return false;
  return true;
}

auto Video::setMonitor(string monitor) -> bool {
  lock_guard<recursive_mutex> lock(mutex);
  if(instance->monitor == monitor) return true;
  if(!instance->hasMonitor()) return false;
  if(!instance->setMonitor(instance->monitor = monitor)) return false;
  return true;
}

auto Video::setContext(uintptr context) -> bool {
  lock_guard<recursive_mutex> lock(mutex);
  if(instance->context == context) return true;
  if(!instance->hasContext()) return false;
  if(!instance->setContext(instance->context = context)) return false;
  return true;
}

auto Video::setBlocking(bool blocking) -> bool {
  lock_guard<recursive_mutex> lock(mutex);
  if(instance->blocking == blocking) return true;
  if(!instance->hasBlocking()) return false;
  if(!instance->setBlocking(instance->blocking = blocking)) return false;
  return true;
}

auto Video::setFlush(bool flush) -> bool {
  lock_guard<recursive_mutex> lock(mutex);
  if(instance->flush == flush) return true;
  if(!instance->hasFlush()) return false;
  if(!instance->setFlush(instance->flush = flush)) return false;
  return true;
}

auto Video::setFormat(string format) -> bool {
  lock_guard<recursive_mutex> lock(mutex);
  if(instance->format == format) return true;
  if(!instance->hasFormat(format)) return false;
  if(!instance->setFormat(instance->format = format)) return false;
  return true;
}

//

auto Video::focused() -> bool {
  lock_guard<recursive_mutex> lock(mutex);
  return instance->focused();
}

auto Video::clear() -> void {
  lock_guard<recursive_mutex> lock(mutex);
  return instance->clear();
}

auto Video::size() -> Size {
  lock_guard<recursive_mutex> lock(mutex);
  Size result;
  instance->size(result.width, result.height);
  return result;
}

auto Video::acquire(u32 width, u32 height) -> Acquire {
  lock_guard<recursive_mutex> lock(mutex);
  Acquire result;
  if(instance->acquire(result.data, result.pitch, width, height)) return result;
  return {};
}

auto Video::release() -> void {
  lock_guard<recursive_mutex> lock(mutex);
  return instance->release();
}

auto Video::output(u32 width, u32 height) -> void {
  lock_guard<recursive_mutex> lock(mutex);
  return instance->output(width, height);
}

auto Video::poll() -> void {
  lock_guard<recursive_mutex> lock(mutex);
  return instance->poll();
}

//

auto Video::onUpdate(const std::function<void (u32, u32)>& onUpdate) -> void {
  lock_guard<recursive_mutex> lock(mutex);
  update = onUpdate;
}

auto Video::doUpdate(u32 width, u32 height) -> void {
  lock_guard<recursive_mutex> lock(mutex);
  if(update) return update(width, height);
}

//

auto Video::create(string driver) -> bool {
  lock_guard<recursive_mutex> lock(mutex);
  self.instance.reset();
  if(!driver) driver = optimalDriver();

  if(driver == "SDL3 GPU") self.instance = std::make_unique<VideoSDL3>(*this);

  if(!self.instance) self.instance = std::make_unique<VideoDriver>(*this);

  return self.instance->create();
}

auto Video::hasDrivers() -> std::vector<string> {
  return {"SDL3 GPU", "None"};
}

auto Video::optimalDriver() -> string {
  return "SDL3 GPU";
}

auto Video::safestDriver() -> string {
  return "SDL3 GPU";
}

// Monitor enumeration via SDL3 (replaces the old native Windows/macOS/X11 paths).
auto Video::hasMonitors() -> std::vector<Monitor> {
  std::vector<Monitor> monitors;

  int count = 0;
  auto* displayIDs = SDL_GetDisplays(&count);
  if(!displayIDs || count <= 0) {
    // fallback if SDL_GetDisplays fails (e.g. video subsystem not initialized)
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
    Monitor monitor;
    SDL_DisplayID id = displayIDs[i];
    monitor.primary = (id == primaryID);

    SDL_Rect rect;
    if(SDL_GetDisplayBounds(id, &rect)) {
      monitor.x = rect.x;
      monitor.y = rect.y;
      monitor.width = rect.w;
      monitor.height = rect.h;
    } else {
      monitor.width = 640;
      monitor.height = 480;
    }

    const char* name = SDL_GetDisplayName(id);
    monitor.name = {1 + monitors.size(), ": ", name ? name : "Display"};

    monitors.push_back(monitor);
  }

  SDL_free(displayIDs);

  // Sort: primary first
  std::vector<Monitor> sorted;
  for(auto& m : monitors) { if(m.primary) sorted.push_back(m); }
  for(auto& m : monitors) { if(!m.primary) sorted.push_back(m); }
  return sorted;
}

auto Video::monitor(string name) -> Monitor {
  auto monitors = Video::hasMonitors();
  for(auto& monitor : monitors) {
    if(monitor.name == name) return monitor;
  }
  // fall back to primary
  for(auto& monitor : monitors) {
    if(monitor.primary) return monitor;
  }
  if(monitors.size() == 1) return monitors[0];
  Monitor fallback;
  fallback.name = "Primary";
  fallback.primary = true;
  fallback.width = 640;
  fallback.height = 480;
  return fallback;
}

}
