# ares-64 MCP bridge

Exposes a running **ares-64** emulator's live N64 debug state to an MCP client
(e.g. Claude Code) so an AI agent can inspect and control the emulator while
debugging an N64 game.

## Architecture

```
Claude Code ──stdio MCP──> ares_mcp.py ──TCP line-JSON──> ares.exe telemetry server
                                          127.0.0.1:9123    (program/telemetry-server.cpp)
                                                            reads ares::Nintendo64::{cpu,rsp,rdp,...}
```

The emulator side is compiled into `ares.exe`. The Python process here is a thin
bridge: each MCP tool sends one JSON request line and returns the JSON reply.

## 1. Run ares with the telemetry server

```
ares.exe game.z64 --mcp                 # default port 9123
ares.exe game.z64 --telemetry-port 9200 # custom port
```

The server is **off by default** (no flag = normal play, no socket opened).

## 2. Install the bridge deps

```
pip install -r requirements.txt
```

## 3. Register the MCP server with Claude Code

```
claude mcp add ares64 -- python /path/to/ares-64/tools/mcp/ares_mcp.py
```

Restart Claude Code, then `/mcp` should list `ares64`.

Config via env vars: `ARES_TELEMETRY_HOST` (default `127.0.0.1`),
`ARES_TELEMETRY_PORT` (default `9123`), `ARES_TELEMETRY_TIMEOUT` (default `5`s).

## Tools

Read: `emu_status`, `cpu_registers`, `rsp_registers`, `rdp_state`,
`peripheral_registers`, `memory_regions`, `read_memory`, `cpu_profiler_top`,
`rsp_capture`, `rdp_capture`, `screenshot`, `render_probe`.

Control: `pause`, `resume`, `step`, `write_memory`, `profiler_control`,
`profiler_clear`, `capture_control`.

Audio: `audio_capture`, `audio_status`, `audio_save`.

RSP debugger: `rsp_debug`, `rsp_break`, `rsp_step`, `rsp_run`, `rsp_stop`.

Controller injection: `controller_set`, `controller_tap`, `controller_release`,
`controller_state`.

Notes:
- `read_memory`/`write_memory` use **region-relative** offsets (the same as the
  ares Memory Editor), not KSEG virtual addresses. List regions with
  `memory_regions`. `write_memory` is destructive (debugging only).
- `rsp_capture`/`rdp_capture` need capture enabled first via `capture_control`.
- `screenshot` saves a PNG of the N64 video output, server-side. Default
  `source="framebuffer"` reconstructs it straight from RDRAM (VI_ORIGIN + VI
  regs) and always works, including headless/background launches. `source=
  "presented"` grabs the exact post-Vulkan frame but collapses to 1×1 when the
  GPU scanout is invalid (background launch) — use the default for debugging.
- `render_probe` answers "is it actually rendering?" in one call: it samples VI
  config, the RDP DPC clock and the frame counter, and reports whether each
  advanced **since the previous probe** — so call it twice a moment apart.
  `rendering` is true only when VI is configured AND all three advanced (a
  configured VI alone is not enough).
- Controller injection drives the N64 pad from the MCP, overriding the host
  controller, so a game can be driven headless (e.g. to reach real gameplay for
  validation). `controller_set` takes the **absolute** held state (buttons not
  passed are released; `x`/`y` are the stick, host scale −32767..+32767). A button
  must be held across at least one game poll, so for a discrete press use
  `controller_tap` (press → hold `hold_ms` → release). Call `controller_release`
  to return control to the host. Note: a game's attract/demo ignores injected
  input (the AI drives it) — enter a real level to validate.
- Breakpoints are **not** in this bridge yet; ares' built-in GDB stub already
  provides hardware/software breakpoints over its own port — connect gdb for that.

## Quick manual test (no MCP)

```
printf '{"id":1,"cmd":"status"}\n' | ncat 127.0.0.1 9123
```
