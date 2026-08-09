//ares-test: JS-scripted headless test runner for the N64 core (Linux only).
//
//  ares-test <script.js> [script-args...] [--timeout sec]
//
//The script drives the emulator through the global `ares` object (see README):
//it boots with no ROM and paused; emulation advances only inside ares.wait*()
//and ares.screenshot() calls, entirely on this thread — no window, no GPU, no
//audio device. Exit codes: 0 = script completed (or ares.exit(0)),
//1 = uncaught JS exception (or ares.exit(1)), 2 = usage error or timeout.

#include "runner.hpp"
#include "js-host.hpp"

#include <nall/main.hpp>

#include <chrono>
#include <cstdlib>
#include <thread>
#include <vector>

using namespace nall;

namespace {

auto usage() -> void {
  print("Usage: ares-test <script.js> [script-args...] [--timeout sec]\n"
        "\n"
        "Runs a JavaScript test script that drives the N64 core headless (no window,\n"
        "no GPU, no audio device) via the global `ares` object. The emulator starts\n"
        "with no ROM loaded and paused; it advances only inside ares.wait*() calls.\n"
        "\n"
        "  --timeout sec   wall-clock watchdog; exit 2 if still running (default: 120)\n"
        "  --help          show this text\n"
        "\n"
        "Script args after the script path are exposed as the ares.args array.\n"
        "ISViewer text (libdragon debugf / libultra osSyncPrintf) is echoed to stdout\n"
        "and queryable via ares.log() / ares.waitLog(marker, maxSeconds).\n");
}

}  //namespace

auto nall::main(Arguments arguments) -> void {
  ares::Memory::FixedAllocator::get();

  if(arguments.take("--help")) { usage(); ::exit(0); }

  u32 timeoutSeconds = 120;
  if(string value; arguments.take("--timeout", value)) timeoutSeconds = value.natural();

  string scriptPath = arguments.take();
  if(!scriptPath) { usage(); ::exit(2); }
  if(!inode::exists(scriptPath)) {
    print(stderr, "[ares-test] error: script not found: ", scriptPath, "\n");
    ::exit(2);
  }

  std::vector<string> scriptArgs;
  while(arguments) scriptArgs.push_back(arguments.take());

  //keep mia's system/save files out of the user's home and the ROM directory
  string workDir = {Path::temporary(), "ares-test/"};
  directory::create(workDir);
  mia::setHomeLocation([workDir]() -> string { return workDir; });
  mia::setSaveLocation([workDir]() -> string { return {workDir, "Saves/"}; });

  ares::platform = &emulatorRunner;

  if(!jsHostInit(scriptArgs)) {
    print(stderr, "[ares-test] error: failed to initialize JS runtime\n");
    ::exit(2);
  }

  //wall-clock watchdog: a hung ROM, a deadlocked core, or a runaway JS loop must
  //not hang CI forever
  std::atomic<bool> finished{false};
  std::thread([&, timeoutSeconds] {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSeconds);
    while(std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
      if(finished.load()) return;
    }
    print(stderr, "[ares-test] error: timeout after ", timeoutSeconds, "s (frame ",
          emulatorRunner.frame(), ")\n");
    fflush(stdout);
    fflush(stderr);
    ::_exit(2);
  }).detach();

  bool ok = jsHostEvalFile(scriptPath);
  finished = true;

  emulatorRunner.closeRom();
  jsHostShutdown();
  fflush(stdout);
  ::exit(ok ? 0 : 1);
}
