#!/usr/bin/env python3
"""MCP bridge for the ares-64 N64 telemetry server.

Exposes the live debug state of a running ares-64 emulator (CPU/RSP/RDP/
peripheral registers, RDRAM/DMEM/IMEM, CPU profiler, RSP/RDP command captures)
plus basic control (pause/resume/frame-advance, memory write, profiler/capture
toggles) as MCP tools.

The emulator side is a small line-delimited JSON-over-TCP server compiled into
ares.exe (desktop-ui/program/telemetry-server.cpp). Launch ares with `--mcp`
(default port 9123) or `--telemetry-port N`, e.g.:

    ares.exe game.z64 --mcp

Config: ARES_TELEMETRY_HOST (default 127.0.0.1), ARES_TELEMETRY_PORT (default 9123).
"""

import json
import os
import socket
import threading
from typing import Optional

from mcp.server.fastmcp import FastMCP

HOST = os.environ.get("ARES_TELEMETRY_HOST", "127.0.0.1")
PORT = int(os.environ.get("ARES_TELEMETRY_PORT", "9123"))
TIMEOUT = float(os.environ.get("ARES_TELEMETRY_TIMEOUT", "5"))

mcp = FastMCP("ares64")

_id = 0
_lock = threading.Lock()
# The telemetry server accepts a single client and closes its listen socket once
# connected, so we keep one persistent connection and reuse it for every request
# (reconnecting only if it drops). `_rx` holds any bytes read past one reply line.
_sock = None
_rx = bytearray()


class AresError(RuntimeError):
    pass


def _connect():
    global _sock, _rx
    if _sock is not None:
        return
    try:
        _sock = socket.create_connection((HOST, PORT), timeout=TIMEOUT)
        _sock.settimeout(TIMEOUT)
        _rx = bytearray()
    except OSError as e:
        _sock = None
        raise AresError(
            f"cannot reach ares telemetry server at {HOST}:{PORT} ({e}). "
            f"Is ares running with --mcp (or --telemetry-port {PORT})?"
        )


def _drop():
    global _sock
    if _sock is not None:
        try:
            _sock.close()
        except OSError:
            pass
    _sock = None


def _roundtrip(line: bytes) -> bytes:
    """Send one request line, return one reply line (bytes, no newline)."""
    global _rx
    _connect()
    try:
        _sock.sendall(line)
        while b"\n" not in _rx:
            chunk = _sock.recv(65536)
            if not chunk:
                raise OSError("connection closed by ares")
            _rx += chunk
    except OSError as e:
        _drop()
        raise AresError(f"telemetry connection lost ({e}); is ares still running?")
    nl = _rx.index(b"\n")
    resp = bytes(_rx[:nl])
    del _rx[: nl + 1]
    return resp


def query(cmd: str, **args) -> dict:
    """Send one request to the ares telemetry server and return its `data`."""
    global _id
    with _lock:
        _id += 1
        req = {"id": _id, "cmd": cmd}
        if args:
            req["args"] = args
        resp_line = _roundtrip((json.dumps(req) + "\n").encode())

    if not resp_line:
        raise AresError("empty response from ares")
    resp = json.loads(resp_line.decode())
    if not resp.get("ok", False):
        raise AresError(resp.get("error", "unknown error"))
    return resp.get("data", {})


def _addr(value) -> int:
    """Accept an int or a hex/decimal string (e.g. 0x1000)."""
    if isinstance(value, int):
        return value
    return int(str(value), 0)


# --- read tools --------------------------------------------------------------

@mcp.tool()
def emu_status() -> dict:
    """Emulator status: loaded game, system, paused state, VPS, GDB-halt, frame."""
    return query("status")


@mcp.tool()
def cpu_registers() -> dict:
    """R4300i CPU registers: GPR[32], lo/hi/pc, COP0 (Status/Cause/EPC/BadVAddr/...), FPU/FCR31."""
    return query("cpu.regs")


@mcp.tool()
def rsp_registers() -> dict:
    """RSP registers: scalar GPR[32]+pc, SP_STATUS flags, vector unit VU[32] + accumulators."""
    return query("rsp.regs")


@mcp.tool()
def rdp_state() -> dict:
    """RDP command/status registers (DPC_START/END/CURRENT/CLOCK, pipe/tmem/buffer busy, flags)."""
    return query("rdp.state")


@mcp.tool()
def peripheral_registers() -> dict:
    """VI/AI/PI/SI/RI peripheral registers, decoded like the ares Register viewer."""
    return query("periph.regs")


@mcp.tool()
def memory_regions() -> dict:
    """List debug memory regions (name + size): RDRAM, RSP DMEM/IMEM, etc."""
    return {"regions": query("mem.regions")}


@mcp.tool()
def read_memory(region: str, addr: str, length: int = 64) -> dict:
    """Read `length` bytes (region-relative `addr`) from a memory region; returns hex.

    region: name (or substring) from memory_regions, e.g. "RDRAM", "DMEM".
    addr:   region-relative byte offset as a decimal or hex string, e.g. "0x1000".
    """
    return query("mem.read", region=region, addr=_addr(addr), len=int(length))


@mcp.tool()
def cpu_profiler_top(window: str = "continuous", limit: int = 32) -> dict:
    """Top CPU functions by exclusive cycles (callCount, incl/excl cycles, wait, bus bytes).

    window: "continuous" (totals since clear) or "frame" (last completed frame).
    """
    return query("profiler.top", window=window, limit=int(limit))


@mcp.tool()
def rsp_capture(limit: int = 4096) -> dict:
    """Captured RSP command stream for the current frame (overlay, command, args, cycle, DMA bytes).

    Requires capture enabled (capture_control). Mirrors the ares RSP command logger.
    """
    return query("rsp.capture", limit=int(limit))


@mcp.tool()
def rdp_capture(limit: int = 4096) -> dict:
    """Captured RDP command stream for the current frame (opcode name, word0/word1, seq)."""
    return query("rdp.capture", limit=int(limit))


@mcp.tool()
def screenshot(path: Optional[str] = None, source: str = "framebuffer") -> dict:
    """Save a PNG of the N64 video output, server-side. Returns {path, width, height}.

    source="framebuffer" (default): reconstruct the framebuffer straight from RDRAM
    (VI_ORIGIN + VI regs). ROBUST — always works, including headless/background
    launches where the GPU scanout is invalid. This is what you want for debugging.

    source="presented": the exact post-Vulkan presented frame. Matches the on-screen
    output including AA/upscale, BUT collapses to 1x1 when the GPU scanout is invalid
    (e.g. ares launched in the background), so it is unreliable for headless use.

    `path` must be writable by ares.exe; defaults to a temp file. Works running or paused.
    """
    if not path:
        import tempfile, os as _os
        path = _os.path.join(tempfile.gettempdir(), "ares_screenshot.png")
    cmd = "video.screenshot" if source == "presented" else "video.framebuffer"
    return query(cmd, path=path)


# --- control tools -----------------------------------------------------------

@mcp.tool()
def pause() -> dict:
    """Pause emulation (the game freezes; debug queries still work)."""
    return query("ctrl.pause")


@mcp.tool()
def resume() -> dict:
    """Resume emulation after a pause."""
    return query("ctrl.resume")


@mcp.tool()
def step() -> dict:
    """Advance exactly one presented frame (works while paused)."""
    return query("ctrl.step")


@mcp.tool()
def write_memory(region: str, addr: str, hex_bytes: str) -> dict:
    """Write bytes (hex string, e.g. "deadbeef") to region-relative `addr`. DESTRUCTIVE.

    addr: decimal or hex string, e.g. "0x300000". Overwrites live game memory
    (debugging only).
    """
    return query("mem.write", region=region, addr=_addr(addr), hex=hex_bytes)


@mcp.tool()
def profiler_control(enable: bool = True) -> dict:
    """Enable/disable the CPU profiler instrumentation."""
    return query("profiler.enable", on=bool(enable))


@mcp.tool()
def profiler_clear() -> dict:
    """Clear accumulated CPU profiler stats."""
    return query("profiler.clear")


@mcp.tool()
def capture_control(rsp: Optional[bool] = None, rdp: Optional[bool] = None) -> dict:
    """Enable/disable RSP and/or RDP command capture. Returns the resulting state."""
    args = {}
    if rsp is not None:
        args["rsp"] = bool(rsp)
    if rdp is not None:
        args["rdp"] = bool(rdp)
    return query("capture.enable", **args)


# --- RSP instruction-level debugger (single-step + breakpoints) --------------
# Halts the RSP between individual instructions so you can trace microcode (e.g.
# a vertex-pipeline hang) register by register. Engaging debug mode forces the
# RSP interpreter; the whole N64 freezes at each stop. Typical flow:
#   rsp_debug(True) -> rsp_break(addr=0x...) -> rsp_run() -> rsp_registers()
#   -> rsp_step() -> rsp_registers() ... -> rsp_debug(False)
# After rsp_step/rsp_run the emulator advances as the reply is sent; read the new
# state with rsp_registers (curInstr/curInstrHex show the instruction at PC, and
# the "debug" block reports active/stopped/curPc/instrCount/breakpoints).

@mcp.tool()
def rsp_debug(enable: bool = True) -> dict:
    """Engage/disengage the RSP instruction debugger. Forces the RSP interpreter.

    When enabled the RSP freezes at the next instruction; set breakpoints
    (rsp_break) then rsp_run, or rsp_step directly. Returns active/stopped/curPc.
    """
    return query("rsp.debug", on=bool(enable))


@mcp.tool()
def rsp_break(action: str = "list", addr: Optional[str] = None) -> dict:
    """Manage RSP IMEM breakpoints (offsets, masked to 0xFFF).

    action: "add" | "remove" | "clear" | "list". addr: decimal/hex string for add/remove.
    Returns the current breakpoint list.
    """
    args = {"action": action}
    if addr is not None:
        args["addr"] = _addr(addr)
    return query("rsp.break", **args)


@mcp.tool()
def rsp_step(count: int = 1) -> dict:
    """Execute `count` RSP instructions then re-freeze (requires rsp_debug active).

    Ack only; read rsp_registers afterwards for the resulting PC/regs/curInstr.
    """
    return query("rsp.step", count=int(count))


@mcp.tool()
def rsp_run() -> dict:
    """Resume the RSP freely until it hits a breakpoint (requires rsp_debug active)."""
    return query("rsp.run")


@mcp.tool()
def rsp_stop() -> dict:
    """Request the RSP to re-freeze at the next instruction boundary (requires rsp_debug active)."""
    return query("rsp.stop")


# --- controller injection ----------------------------------------------------
# Drive the N64 pad straight from the MCP, overriding the host controller, so a
# game can be driven headless (e.g. to reach real gameplay for validation). Each
# controller_set defines the FULL held state: buttons not passed are released and
# the stick defaults to centered. Call controller_release to hand control back to
# the host. A button needs to be held across at least one game poll, so for a
# discrete press use controller_tap (set -> wait -> release).

@mcp.tool()
def controller_set(
    a: bool = False, b: bool = False, z: bool = False, start: bool = False,
    up: bool = False, down: bool = False, left: bool = False, right: bool = False,
    c_up: bool = False, c_down: bool = False, c_left: bool = False, c_right: bool = False,
    l: bool = False, r: bool = False,
    x: int = 0, y: int = 0,
) -> dict:
    """Set the injected N64 controller state (absolute: unlisted buttons are released).

    Buttons are booleans; x/y are the analog stick, host scale -32767..+32767
    (0 = centered, +x = right, +y = up). Activates the override until
    controller_release. Returns {active, buttons, x, y}.
    """
    return query(
        "input.set",
        a=a, b=b, z=z, start=start, up=up, down=down, left=left, right=right,
        cup=c_up, cdown=c_down, cleft=c_left, cright=c_right, l=l, r=r,
        x=int(x), y=int(y),
    )


@mcp.tool()
def controller_release() -> dict:
    """Release the injected controller; the host controller regains control."""
    return query("input.release")


@mcp.tool()
def controller_state() -> dict:
    """Read the current injected controller state {active, buttons, x, y}."""
    return query("input.state")


@mcp.tool()
def controller_tap(button: str, hold_ms: int = 150) -> dict:
    """Tap one button: press it, hold `hold_ms`, then release everything.

    button: one of a,b,z,start,up,down,left,right,c_up,c_down,c_left,c_right,l,r.
    Convenience for menu navigation / discrete presses. Leaves the override active
    but neutral (all released); call controller_release to return to the host.
    """
    import time
    key = button.strip().lower().replace("-", "_")
    valid = {"a","b","z","start","up","down","left","right",
             "c_up","c_down","c_left","c_right","l","r"}
    if key not in valid:
        raise AresError(f"unknown button {button!r}; valid: {sorted(valid)}")
    query("input.set", **{key: True})
    time.sleep(max(0.0, hold_ms / 1000.0))
    return query("input.set")  # all released, override stays active


# --- render / liveness probe -------------------------------------------------

@mcp.tool()
def render_probe() -> dict:
    """One-shot 'is the game actually rendering?' check.

    Samples VI (config + origin), the RDP DPC clock and the frame counter, and
    reports deltas since the PREVIOUS render_probe call, so call it twice a moment
    apart. Returns {viActive, viOrigin, dpcClock, frame, vps, paused,
    originChanged, dpcAdvanced, frameAdvanced, rendering}. The delta fields are
    null on the first call (no baseline). `rendering` is true only when VI is
    configured AND origin/DPC/frame all advanced — the reliable ground-truth
    signal (a configured VI alone is not sufficient).
    """
    return query("render.probe")


# --- audio capture -----------------------------------------------------------

@mcp.tool()
def audio_capture(seconds: float = 3.0) -> dict:
    """Start recording the emulator's mixed audio output (what the host hears).

    Records from the frontend's post-mix tap (same source as the audio viewer),
    at the host output rate. The tap is a ~4 s ring buffer, so `seconds` is
    clamped to that. Poll audio_status until done, then audio_save.
    """
    return query("audio.capture", seconds=seconds)


@mcp.tool()
def audio_status() -> dict:
    """Progress of the current audio capture: {active, count, cap, rate, done}."""
    return query("audio.status")


@mcp.tool()
def audio_save(path: str) -> dict:
    """Write the captured audio to a 16-bit PCM stereo WAV at `path` (server-side)."""
    return query("audio.save", path=path)


if __name__ == "__main__":
    mcp.run()
