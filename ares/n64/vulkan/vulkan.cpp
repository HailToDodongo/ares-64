#include <n64/n64.hpp>

namespace ares::Nintendo64 {

Vulkan vulkan;

struct LoggingInterface : Util::LoggingInterface {
  auto log(const char* tag, const char* fmt, va_list va) -> bool {
    char buffer[8192];
    vsnprintf(buffer, sizeof(buffer), fmt, va);
  //print(terminal::color::yellow(tag), buffer);
    return true;
  }
} loggingInterface;

struct Vulkan::Implementation {
  Implementation(u8* data, u32 size);
  ~Implementation();

  ::Vulkan::Context context;
  ::Vulkan::Device device;
  ::RDP::CommandProcessor* processor = nullptr;
  atomic<const char*> crash_error = nullptr;

  struct Validation : public ::RDP::ValidationInterface {
    Implementation& self;
    Validation(Implementation& i) : self(i) {}
    void report_rdp_crash(::RDP::ValidationError err, const char *msg) override {
      self.crash_error = msg;
    }
  } validator{*this};

  //commands are u64 words, but the backend uses u32 swapped words.
  //size and offset are in u64 words.
  u32 buffer[0x10000] = {};
  u32 queueSize = 0;
  u32 queueOffset = 0;

  ::RDP::VIScanoutBuffer scanout;
  std::mutex lock;
  std::condition_variable condition;
  u32 scanoutCount = 0;
  u32 endCount = 0;
};

auto Vulkan::load(Node::Object) -> bool {
  if (vulkan.enable) {
    Util::set_thread_logging_interface(&loggingInterface);
    delete implementation;
    implementation = new Vulkan::Implementation(rdram.ram.data, rdram.ram.size);
    if(!implementation->processor) {
      delete implementation;
      implementation = nullptr;
    }

    if (!implementation) {
      platform->status("Vulkan init failed: No RDP rendering support");
      vulkan.enable = false;
    } else {
      platform->status("Vulkan Enabled: using paraLLEl-RDP");
    }
  } else {
    #if defined(ANGRYLION)
    //angrylion (loaded right after this) provides rendering instead; stay quiet so it
    //isn't reported as "no RDP rendering support".
    if(!angrylion.enable)
    #endif
    platform->status("Vulkan Disabled: No RDP rendering support");
  }

  return true;
}

auto Vulkan::unload() -> void {
  if (implementation) delete implementation;
  implementation = nullptr;
}

auto Vulkan::render() -> bool {
  if(!implementation) return false;

  static constexpr u32 commandLength[64] = {
    1, 1, 1, 1, 1, 1, 1, 1, 4, 6,12,14,12,14,20,22,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
  };

  auto& command = rdp.command;

  u32 current = command.current & ~7;
  u32 end = command.end & ~7;
  u32 length = (end - current) / 8;
  if(current >= end) return true;

  u32* buffer = implementation->buffer;
  u32& queueSize = implementation->queueSize;
  u32& queueOffset = implementation->queueOffset;
  if(queueSize + length >= 0x8000) return true;

  if(!command.source) {
    do {
      buffer[queueSize * 2 + 0] = rdram.ram.read<Word>(current, RBusDevice::DP_DMA); current += 4;
      buffer[queueSize * 2 + 1] = rdram.ram.read<Word>(current, RBusDevice::DP_DMA); current += 4;
      queueSize++;
    } while(--length);
  } else {
    do {
      buffer[queueSize * 2 + 0] = rsp.dmem.read<Word>(current); current += 4;
      buffer[queueSize * 2 + 1] = rsp.dmem.read<Word>(current); current += 4;
#if ARES_DEBUG_TOOLS
      if(system.homebrewMode) {
        rsp.debugger.dmemReadWord(current - 8, 8, "RDP XBUS");
      }
#endif
      queueSize++;
    } while(--length);
  }

#if ARES_DEBUG_TOOLS
  // Flame-chart: this whole render() call is one DP flush, processed at a single
  // wall-clock instant. Stamp it with now() (the master wall clock) + count commands.
  u64 tlStart = cpu.profiler.now();
  u32 tlCount = 0;
#endif

  while(queueOffset < queueSize) {
    u32 op = buffer[queueOffset * 2];
    u32 code = op >> 24 & 63;
    u32 length = commandLength[code];

    if(queueOffset + length > queueSize) {
      command.start = command.current = command.end;
#if ARES_DEBUG_TOOLS
      if(rdp.capture.enabled.load(std::memory_order_relaxed) && tlCount)
        rdp.capture.pushTimeline(tlStart, tlCount);
#endif
      return true;
    }

#if ARES_DEBUG_TOOLS
    if(rdp.capture.enabled.load(std::memory_order_relaxed)) {
      u64 word0 = (u64)buffer[queueOffset * 2] << 32 | buffer[queueOffset * 2 + 1];
      rdp.capture.push(0, (u32)queueOffset, (u8)code, word0, 0, (u8)length);
    }
    tlCount++;
#endif

    if(code >= 8) {
      implementation->processor->enqueue_command(length * 2, buffer + queueOffset * 2);
    }

    if(::RDP::Op(code) == ::RDP::Op::SyncFull) {
      implementation->processor->wait_for_timeline(implementation->processor->signal_timeline());
      rdp.syncFull();
    }

#if ARES_DEBUG_TOOLS
    // Step mode: pause on interesting commands without leaving render()
    if(rdp.capture.stepMode.load(std::memory_order_relaxed)) {
      bool interesting = (code >= 0x08 && code <= 0x0f) || // triangles
                          code == 0x24 || code == 0x25 ||    // tex rects
                          code == 0x29 ||                     // sync full
                          code == 0x36 ||                     // fill rect
                          code == 0x3f;                       // set color image

      if(interesting) {
        // Flush GPU so framebuffer reflects this command
        u64 tl = implementation->processor->signal_timeline();
        implementation->processor->wait_for_timeline(tl);

        // Commit capture for viewer
        rdp.capture.committedCount.store(rdp.capture.writePos.load(std::memory_order_acquire), std::memory_order_release);
        rdp.capture.stepPending.store(false, std::memory_order_release);

        // Wait for next step
        while(rdp.capture.stepMode.load() && rdp.capture.enabled.load() &&
              !rdp.capture.stepPending.load()) {
          if(rsp.capture.requestClear.exchange(false, std::memory_order_acq_rel)) {
            rsp.capture.committedCount.store(0, std::memory_order_release);
            rsp.capture.writePos.store(0, std::memory_order_release);
          }
          usleep(2000);
        }
      }
    }
#endif

    queueOffset += length;
  }

  queueOffset = 0;
  queueSize = 0;
  command.current = command.end;
#if ARES_DEBUG_TOOLS
  if(rdp.capture.enabled.load(std::memory_order_relaxed) && tlCount)
    rdp.capture.pushTimeline(tlStart, tlCount);
#endif
  return true;
}

auto Vulkan::frame() -> void {
  if(!implementation) return;
  implementation->processor->begin_frame_context();
}

auto Vulkan::flush() -> void {
  if(!implementation) return;
  u64 tl = implementation->processor->signal_timeline();
  implementation->processor->wait_for_timeline(tl);
}

auto Vulkan::writeWord(u32 address, u32 data) -> void {
  if(!implementation) return;
  implementation->processor->set_vi_register(::RDP::VIRegister(address), data);
}

auto Vulkan::scanoutAsync(bool field) -> bool {
  if(!implementation) return false;

  { //wait until we're done reading in thread before we clobber the readback buffer
    std::unique_lock<std::mutex> lock{implementation->lock};
    implementation->condition.wait(lock, [this]() {
      return implementation->scanoutCount == implementation->endCount;
    });
  }

  implementation->processor->set_vi_register(::RDP::VIRegister::VCurrentLine, field);

  //0 steps if scanning out at upscaled resolution.
  //each downscale step reduces output resolution to [width, height] * max(1, upscale >> downscale_steps)
  ::RDP::ScanoutOptions options;
  options.downscale_steps = supersampleScanout ? 16 : 0;
  options.persist_frame_on_invalid_input = true;  //this is a compatibility hack, but I'm not sure what for ...
  if(disableVideoInterfaceProcessing) {
    options.vi = {false, false, true, false, false, false};
  }
  if(!supersampleScanout){
    options.blend_previous_frame = weaveDeinterlacing;
    options.upscale_deinterlacing = !weaveDeinterlacing;
  }
  else {
    options.blend_previous_frame = false;
    options.upscale_deinterlacing = true;
  }


  if(implementation->scanout.fence) {
    implementation->scanout.fence->wait();
  }
  implementation->processor->scanout_async_buffer(implementation->scanout, options);
  implementation->scanoutCount++;
  return true;
}

auto Vulkan::mapScanoutRead(const u8*& rgba, u32& width, u32& height) -> void {
  if(!implementation || !implementation->scanout.fence || !implementation->scanout.width || !implementation->scanout.height) {
    rgba = nullptr;
    width = 0;
    height = 0;
  } else {
    implementation->scanout.fence->wait();
    rgba = (const u8*)implementation->device.map_host_buffer(*implementation->scanout.buffer, ::Vulkan::MEMORY_ACCESS_READ_BIT);
    width = implementation->scanout.width;
    height = implementation->scanout.height;
  }
}

auto Vulkan::unmapScanoutRead() -> void {
  if(implementation && implementation->scanout.buffer) {
    implementation->device.unmap_host_buffer(*implementation->scanout.buffer, ::Vulkan::MEMORY_ACCESS_READ_BIT);
  }
}

auto Vulkan::endScanout() -> void {
  if(implementation) {
    //notify main thread that we're done reading
    std::lock_guard<std::mutex> lock{implementation->lock};
    implementation->endCount++;
    implementation->condition.notify_one();
  }
}

auto Vulkan::mapHiddenRDRAM(const u8*& data, u32& size) -> void {
  if(!implementation) { data = nullptr; size = 0; return; }
  data = (const u8*)implementation->processor->begin_read_hidden_rdram();
  size = (u32)implementation->processor->get_hidden_rdram_size();
}

auto Vulkan::unmapHiddenRDRAM() -> void {
  if(implementation) implementation->processor->end_write_hidden_rdram();
}

auto Vulkan::crashed() -> const char* {
  if(implementation) return implementation->crash_error;
  return nullptr;
}

Vulkan::Implementation::Implementation(u8* data, u32 size) {
  if(!::Vulkan::Context::init_loader(nullptr)) return;
  if(!context.init_instance_and_device(nullptr, 0, nullptr, 0, 0)) return;
  device.set_context(context);
  device.init_frame_contexts(3);

  ::RDP::CommandProcessorFlags flags = 0;
  switch(vulkan.internalUpscale) {
  case 2: flags |= ::RDP::COMMAND_PROCESSOR_FLAG_UPSCALING_2X_BIT; break;
  case 4: flags |= ::RDP::COMMAND_PROCESSOR_FLAG_UPSCALING_4X_BIT; break;
  case 8: flags |= ::RDP::COMMAND_PROCESSOR_FLAG_UPSCALING_8X_BIT; break;
  }

  if(vulkan.internalUpscale > 1) {
    flags |= ::RDP::COMMAND_PROCESSOR_FLAG_SUPER_SAMPLED_DITHER_BIT;
    //rasky: this is explicitly disabled because we want to make sure we don't
    // read back the super sampled version, as it can cause artifacts. We want
    // parallelRDP to also produce a 1x render to use for readbacks.
    //flags |= ::RDP::COMMAND_PROCESSOR_FLAG_SUPER_SAMPLED_READ_BACK_BIT;
  }

  processor = new ::RDP::CommandProcessor(device, data, 0, size, size / 2, flags);
  if(!processor->device_is_supported()) {
    delete processor;
    processor = nullptr;
    return;
  }

  processor->set_validation_interface(&validator);
}

Vulkan::Implementation::~Implementation() {
  if(processor) delete processor;
}

}
