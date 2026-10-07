#pragma once

// N64 telemetry server — a small line-delimited JSON-over-TCP endpoint that
// exposes the same live debug state the desktop-ui debug windows show (CPU/RSP/
// RDP/peripheral registers, RDRAM/DMEM/IMEM, the CPU profiler and the RSP/RDP
// command captures), plus basic control (pause/resume/frame-advance, memory
// write, profiler/capture toggles). It is the in-emulator half of the MCP
// bridge (tools/mcp/ares_mcp.py).
//
// Like the GDB stub, this is pumped from the emulator run loop via poll(): the
// socket worker threads only buffer bytes, and TCP::Socket::update() invokes
// onData() on whichever thread calls it. Because poll() is called from the
// emulator worker thread, command handlers read core state directly with no
// extra locking (we are already on the thread that owns the core).

#include <nall/tcptext/tcp-socket.hpp>
#include <string>
#include <vector>

namespace nall::N64Telemetry {

// Controller-override bit layout (matches the N64 Gamepad node names). Driven by
// the input.* telemetry commands and applied in Program::input()
// (desktop-ui/program/platform.cpp) so the MCP can drive the game with no host
// controller — needed to validate gameplay headless.
enum : u32 {
  Pad_A=1<<0, Pad_B=1<<1, Pad_Z=1<<2, Pad_Start=1<<3,
  Pad_Up=1<<4, Pad_Down=1<<5, Pad_Left=1<<6, Pad_Right=1<<7,
  Pad_CUp=1<<8, Pad_CDown=1<<9, Pad_CLeft=1<<10, Pad_CRight=1<<11,
  Pad_L=1<<12, Pad_R=1<<13,
};

struct InputOverride {
  bool active = false;  // when false, host controller input passes through unchanged
  u32  buttons = 0;     // held buttons, Pad_* bit mask
  s32  x = 0, y = 0;    // stick axes, host scale -32767..+32767 (0 = centered)
};
extern InputOverride inputOverride;

struct Server : nall::TCP::Socket {
  // Pump buffered socket data on the caller's thread. Call once per emulator
  // run-loop iteration (see desktop-ui/program/program.cpp).
  auto poll() -> void { update(); }

protected:
  auto onData(const std::vector<u8>& data) -> void override;
  auto onConnect() -> void override {}
  auto onDisconnect() -> void override { rxBuffer.clear(); }

private:
  std::string rxBuffer;                              // accumulates partial lines
  auto handleLine(const std::string& line) -> void;  // dispatch one request
  auto send(const std::string& text) -> void;        // append a '\n'-terminated reply
};

extern Server server;

}
