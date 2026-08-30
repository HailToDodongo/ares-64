# ares-test - JavaScript scripting

`ares-test` is a headless build of the N64 core driven entirely by a JavaScript
script. It needs **no window, no GPU and no audio device**, so it runs in bare CI
containers, and it is designed for automated ROM testing: boot a ROM, press
buttons, wait, then assert on screenshots, audio or the ROM's own log output.

Scripts run on an embedded [QuickJS-NG](https://github.com/quickjs-ng/quickjs)
runtime (`thirdparty/quickjs-ng`).

## Build & run

```sh
cmake --preset linux-headless
cmake --build build_headless --target ares-test -j10

./build_headless/test-runner/ares-test my-test.js [script-args...] [--timeout sec]
```

| Option | Description |
| --- | --- |
| `<script.js>` | The test script to run (required, first positional argument) |
| `[script-args...]` | Passed through to the script as the `ares.args` array |
| `--timeout sec` | Wall-clock watchdog, default `120`. Guards CI against hung ROMs |
| `--help` | Show usage and exit |

| Exit code | Meaning |
| --- | --- |
| `0` | Script ran to completion (or called `ares.exit(0)`) |
| `1` | Uncaught JS exception - a plain `throw` is the assert mechanism |
| `2` | Usage error, ROM/script not found, or the `--timeout` watchdog fired |

## Execution model

* The emulator starts with **no ROM loaded and paused**.
* Every call is **synchronous**. There is no event loop: emulation only advances
  inside `wait*()` and `screenshot()`, on the same thread as the script.
* Time is **emulated time**, never wall-clock, so runs are reproducible: the same
  script on the same ROM always produces the same frames and the same hashes
  (`Deterministic Entropy` is forced on and there is no audio clock to drift).
* Between two JS statements the emulator is completely frozen - inputs latched
  with `hold()` take effect exactly at the following `wait*()`.

```js
ares.setRenderer("angrylion");
ares.loadRom(ares.args[0]);
ares.resume();

ares.wait(2.0);                       // two seconds of emulated time
ares.controller(1).hold("Start");
ares.waitFrames(5);
ares.controller(1).release("Start");

const shot = ares.screenshot();
if (shot.sha256 !== EXPECTED) throw new Error("mismatch: " + shot.sha256);
```

## `ares` - emulator control

| Function | Description |
| --- | --- |
| `loadRom(path)` | Load a ROM and cold-boot it. Throws on failure. Leaves the emulator paused |
| `closeRom()` | Unload the current ROM |
| `reset(hard?)` | `reset(true)` cold-boots (RDRAM refilled), `reset()` is a soft reset |
| `pause()` / `resume()` | Gate whether `wait*()` may advance emulation |
| `isPaused()` | → `bool` |
| `setRenderer(name)` | `"angrylion"` (CPU renderer) or `"none"` (no RDP; software VI scanout of RDRAM). Callable at any time - with a ROM loaded it hot-swaps at the next display-list boundary |
| `setHomebrew(enable?)` | Enable homebrew mode. Must be called **before** `loadRom` |
| `exit(code?)` | Quit immediately with the given exit code (default `0`) |
| `args` | Array of the script arguments passed on the command line (property, not a function) |

`setRenderer` is useful for speed: boot under `"none"` (RDP commands are no-ops)
and switch to `"angrylion"` only for the frames you actually assert on.

## `ares` - time

All durations are in **emulated** time. `wait()` uses the emulated CPU clock, so
a second is always a second of in-game time; `waitFrames()`/`waitVI()` count VI
ticks, whose rate is not constant (see the gotchas).

| Function | Description |
| --- | --- |
| `wait(seconds)` | Run for N emulated seconds, measured on the CPU clock |
| `waitFrames(n)` | Run for N VI ticks |
| `waitVI()` | Run until the next VI tick |
| `frameCount()` | → VI ticks since the ROM was loaded |

These throw if no ROM is loaded, if the emulator is paused, or if called from
inside a callback.

## `ares` - input

`ares.controller(port)` returns a controller handle for port `1`–`4`. All four
ports have a Gamepad connected. The methods are chainable.

| Function | Description |
| --- | --- |
| `controller(port)` | → controller object for port `1`–`4` |
| `.hold(button)` | Press and keep holding a button |
| `.release(button)` | Release a button |
| `.stick(x, y)` | Analog stick, both axes in `[-1, +1]` |
| `.clear()` | Release everything on this port, stick back to neutral |
| `.port` | The port number (property) |

Button names: `A` `B` `Z` `L` `R` `Start` `Up` `Down` `Left` `Right`
`C-Up` `C-Down` `C-Left` `C-Right`. An unknown name throws.

Inputs are latched, not pulsed - a press needs a wait between hold and release:

```js
const press = (btn) => { p1.hold(btn); ares.waitVI(); p1.release(btn); ares.waitVI(); };
```

## `ares` - capture

| Function | Description |
| --- | --- |
| `screenshot()` | Capture the frame currently on screen → **image object**. A pure read: it advances nothing and is allowed inside callbacks |
| `loadImage(path)` | Load a PNG → **image object** (same shape as a capture) |
| `depthBuffer(width, height, options?)` | Read the RDP's current depth image → **depth object**. A pure read, allowed inside callbacks |
| `startAudio(rate?)` | Begin recording. Default records at the AI output rate the game programmed; pass e.g. `48000` to force a rate |
| `stopAudio()` | Stop recording → **audio object** |
| `loadAudio(path)` | Load a 16-bit PCM WAV → **audio object** |

### Image object

| Member | Description |
| --- | --- |
| `width`, `height` | Pixel dimensions |
| `sha256` | Hash of the raw RGB pixels - the stable golden key (PNG bytes vary when `optipng` is installed) |
| `data` | `ArrayBuffer` of RGBA bytes; wrap with `new Uint8Array(img.data)` |
| `save(path)` | Write a PNG |
| `compare(other, tolerance?)` | Diff against an image object **or a PNG path** → **compare result** |
| `crop(x, y, width, height)` | Extract a subregion, new **image object**. |

`tolerance` (default `0`) is the maximum per-channel delta still counted as
equal. Alpha is ignored.

### Depth object

Hardware records *where* the depth image lives but never how large it is, so you
supply the buffer's dimensions. `options` selects a subsection and the value
format:

| Option | Description |
| --- | --- |
| `x`, `y` | Top-left of the region to read (default `0, 0`) |
| `width`, `height` | Size of the region (default: the rest of the buffer). Clamped to the buffer |
| `float` | `true` → normalised `0..1` floats; otherwise raw 18-bit integers |

| Member | Description |
| --- | --- |
| `width`, `height` | Size of the region actually returned |
| `address` | RDRAM address the values came from |
| `float` | Which format `data` holds |
| `data` | `ArrayBuffer` — wrap with `new Uint32Array(d.data)`, or `new Float32Array(d.data)` when `float` was set |

Values are the decompressed **18-bit linear Z** (`0..0x3ffff`), the same decode
the framebuffer viewer uses — not the raw 14-bit encoded halfword in RDRAM. The
float form is simply that value over `0x3ffff`.

```js
const d = ares.depthBuffer(320, 240);              // whole buffer, raw
const z = new Uint32Array(d.data);

const near = ares.depthBuffer(320, 240, {x: 100, y: 90, width: 32, height: 16, float: true});
const f = new Float32Array(near.data);             // 0 = near, 1 = far plane
```

If the ROM has not selected a depth image yet, the call throws.

### Audio object

| Member | Description |
| --- | --- |
| `samples` | Number of stereo frames |
| `frequency` | Sample rate in Hz |
| `data` | `ArrayBuffer` of interleaved stereo `s16`; wrap with `new Int16Array(rec.data)` |
| `save(path)` | Write a WAV |
| `compare(other, tolerance?)` | Diff against an audio object **or a WAV path** → **compare result** |
| `snr(reference)` | Signal-to-noise ratio in dB against a reference object or WAV path. `Infinity` when identical |
| `trim()` | → new object with leading/trailing silence removed (one zero frame kept per side) |
| `slice(start, end?)` | → new object with the `[start, end)` subsection, in seconds |

`slice` follows `Array.prototype.slice` conventions: `end` is optional and
exclusive, negative values count from the end, out-of-range values clamp.
`snr` requires matching sample rates; differing lengths are compared over the
overlapping prefix.

`trim()` is the natural companion to `snr`/`compare`, since when `startAudio()`
is called relative to the game emitting sound decides the lead-in length:

```js
if (rec.trim().snr(golden.trim()) < 40) throw new Error("audio degraded");
```

### Compare result

| Member | Images | Audio |
| --- | --- | --- |
| `match` | `true` when nothing exceeded the tolerance | same |
| `reason` | Present **instead of** the stats on a size mismatch | Present on frequency or length mismatch |
| `diffPixels` / `diffSamples` | Elements over the tolerance | same |
| `totalPixels` / `totalSamples` | Elements compared | same |
| `maxDelta`, `avgDelta` | Largest / mean deviation | same (in `s16` units) |
| `diff` | **image object** present only when pixels differed: the compared image darkened with every differing pixel in white | — |

Shape mismatches do not throw - they return `{match: false, reason: "..."}`, so a
snapshot test fails with a useful message instead of erroring.

## `ares` - RSP profiling (libdragon rspq)

Aggregated per-command RSP timing, built on the RSP command capture (the same
machinery as the desktop RSP viewer / flame chart). Works for libdragon ROMs
with their `.elf` next to the ROM (or in a `build/` directory beside it);
`rspq-libdragon.json` is picked up from next to the ELF or the `ares-test`
binary. Requires an ares build with `ARES_ENABLE_DEBUG_TOOLS`.

- `ares.rspProfileStart()` open a fresh aggregation window. Capture setup is
  lazy: the first profiling call auto-detects the ROM's ELF/config and prints
  what it found. Nothing resets per-frame, the window runs until the next
  `rspProfileStart()`.
- `ares.rspProfile()` returns the aggregated table for the current window:
  `{ totalCycles, commandCycles, overheadCycles, lostRows, rows: [...] }`.
  Each row: `{ name, overlay, overlayId, commandId, overhead, overheadType?,
  count, cycles, avg, bytesIn, bytesOut }`, sorted by cycles descending.
  Cycles are RSP cycles (62.5 MHz) from the emulated pipeline model, treat
  absolute values as approximate, deltas between two runs of the same workload
  as exact. Overhead rows attribute rspq kernel time: `loop` (dispatch),
  `overlaySwitch` (ucode save/load), `bufferFetch` (command-buffer DMA).
- `ares.waitRspCommand(name [, count [, timeoutFrames]])` /
  `ares.waitRspCommand(overlayId, commandId [, count [, timeoutFrames]])` —
  run emulation until `count` (default 1) more executions of the command have
  retired; throws after `timeoutFrames` (default 1800) VI frames. `name` is a
  command name from `rspq-libdragon.json` (e.g. `"Vert Load"`, `"Screen
  Size"`). While a profile window is open, rows up to and *including* the
  final match keep aggregating and the window then freezes — so bracketing
  with the same once-per-frame marker on both sides yields an exact
  whole-frame window at any frame rate:

```js
ares.waitFrames(30);                    // skip boot
ares.waitRspCommand("Screen Size");     // align to a frame boundary
ares.rspProfileStart();
ares.waitRspCommand("Screen Size", 60); // exactly 60 game frames
const t = ares.rspProfile();
for (const r of t.rows)
  console.log(r.name, r.count, Math.round(r.cycles), r.avg.toFixed(1));
```

`lostRows` is non-zero only if the capture ring (64k rows) overflowed between
API calls — bracket long windows with `waitRspCommand` (which drains
continuously) rather than a single huge `waitFrames`.

### Per-command instruction tracing

- `ares.rspTrace(name [, occurrences [, timeoutFrames]])` /
  `ares.rspTrace(overlayId, commandId [, occurrences [, timeoutFrames]])` —
  run emulation and record the RSP instruction trace while the given rspq
  command executes: tracing starts automatically at the command's dispatch and
  stops when its handler returns to the dispatch loop (the indirect successor
  of the emux `XTRACE` opcodes — no ROM changes needed). Returns
  `{ occurrences, truncated, text }`.

  Each line is `RSP  <pc>  [cycle]  | disassembly {register values}`; the
  cycle column is the offset since the command started (dual-issued pairs
  share a cycle number, stalls appear as gaps/`*` markers), and it restarts at
  0 for each traced occurrence. The text is capped at 16 MB (`truncated`
  set if hit). The recompiler keys its block cache on trace mode, so toggling
  is cheap and the traced timing is the same as an untraced run.

  Typical use: trace the same command in two ucode builds and diff the cycle
  columns to find where schedules diverge.

## `ares` - ROM log (ISViewer)

Text the ROM prints through the IS-Viewer channel - libdragon's `debugf()` and
libultra's `osSyncPrintf()` - is echoed to stdout live and captured for the
script.

| Function | Description |
| --- | --- |
| `log()` | → all ISViewer text accumulated so far |
| `clearLog()` | Discard the accumulated text |
| `waitLog(marker, maxSeconds?)` | Run until `marker` appears in the log. → `true` if found, `false` on timeout (default `10` emulated seconds) |
| `onLog(fn)` | Call `fn(line)` for each **completed** line the ROM prints (newline stripped); `onLog(null)` removes the handler |

```js
if (!ares.waitLog("TEST PASSED", 30)) throw new Error("self-test failed:\n" + ares.log());

const timings = [];
ares.onLog((line) => { if (line.startsWith("Avg:")) timings.push(line); });
```

`onLog` is a callback and follows the callback rules below: it fires from inside
the emulation loop, so it must not advance emulation. The ROM's text is echoed to
stdout and accumulated for `log()` regardless of whether a handler is installed.

## `ares` - callbacks

| Function | Description |
| --- | --- |
| `setTimeout(fn, seconds)` | Run `fn` once after N seconds of **in-game** time → timer id |
| `setInterval(fn, seconds)` | Run `fn` every N seconds of in-game time → timer id |
| `clearTimeout(id)` / `clearInterval(id)` | Cancel a timer → `true` if it was still pending. The two are interchangeable, as in JS |
| `onInterrupt(fn)` | Call `fn(source)` on every RCP interrupt; `source` is `"SP"` `"SI"` `"AI"` `"VI"` `"PI"` `"DP"` |
| `onInterrupt(null)` | Remove the handler |
| `onLog(fn)` / `onLog(null)` | Call `fn(line)` per completed ROM log line - see [ROM log](#ares--rom-log-isviewer) |

Argument order follows JS (callback first), but the delay is in **seconds** -
the unit used throughout this API - not milliseconds.

```js
ares.setTimeout(() => console.log("half a second of game time"), 0.5);
const id = ares.setInterval(() => samples.push(readState()), 0.1);
ares.wait(1.0);          // the interval ticks ~10 times during this call
ares.clearInterval(id);
```

Callbacks fire from inside the emulation loop - that is, *during* a `wait*()`
call - and are driven by the emulated CPU clock, so they are deterministic.
A repeating timer schedules the next tick from the deadline it just met, so the
period does not drift; missed ticks are skipped rather than queued into a burst.

Two rules follow from running inside the core:

* A callback **may** register or cancel timers and read state.
* A callback **may not** advance or tear down emulation: `wait*()`, `loadRom()`,
  `closeRom()`, `reset()`, `setRenderer()` and `exit()` all throw if called from
  one. `screenshot()` is fine - it only reads the last presented frame.
* An exception thrown by a callback **aborts the wait immediately** - emulation
  stops at the next tick boundary rather than finishing the remaining time - and
  is rethrown by the `wait()` that was running, with a normal stack trace. If the
  script catches it, emulation continues normally.

## Modules and shared state

A script that contains `import`/`export` is evaluated as an ES module;
everything else runs as a plain global script. Relative imports resolve against
the importing file, and modules are cached per path - so a mutable export is
shared between all importers.

```js
import { assert, assertSnapshot } from "../lib/assert.js";
```

`globalThis.FLAG = true` is visible to every module in the same run. Nothing is
shared *between* runs: each script is its own process.

`tests/lib/assert.js` ships `assert`, `assertEq`, `assertThrows` and
`assertSnapshot(media, goldenPath, tolerance?)`, which creates the golden file on
first run and compares against it afterwards.

## Console

`console.log(...)` and `console.error(...)` print to stdout; objects are
JSON-stringified.

## Test harness

```sh
tests/run-tests.sh [script.test.js ...]
```

With no arguments it runs `tests/*.test.js` (committed, ROM-free) plus
`tests/local/*.test.js` (git-ignored - put scripts with private ROM paths there).
A test passes when its script exits `0`. Set `ARES_TEST` to point at a different
runner binary.

## Docker

The [Dockerfile](./Dockerfile) builds `libdragon-ares-test`, combining the
libdragon toolchain with `ares-test` so one container can compile a ROM and then
test it:

```sh
podman build --build-context libdragon=../libdragon -t libdragon-ares-test .
podman run --rm -v "$PWD":/app -w /app libdragon-ares-test make
podman run --rm -v "$PWD":/app -w /app libdragon-ares-test ares-test test.js game.z64
```

angrylion is bit-exact, so `sha256` goldens recorded on a dev machine hold inside
the container and in CI.

## Notes & gotchas

* **Renderer output sizes differ.** angrylion renders at native `640x240`
  (NTSC); the `"none"` software VI scans out `640x480`. Goldens are therefore
  renderer-specific, and comparing across a renderer swap reports a size
  mismatch.
* **VI ticks are not a fixed rate.** While the VI is inactive (during boot, or a
  ROM that never initialises the display) ticks come roughly twice as fast as the
  nominal 60/50Hz. `wait()`, `waitLog()` and the timers all use the CPU clock and
  are unaffected, but `waitFrames(n)`/`frameCount()` count ticks, so they are not
  a reliable measure of elapsed time across the boot phase.
* **`screenshot()` returns the last presented frame.** It does not advance
  emulation, so call `waitVI()`/`waitFrames()` first if you want a newer one. A
  ROM that never enables the display presents nothing at all, and the call fails
  with a clear error instead of hanging.
* **The ISViewer channel needs the ROM to use it** (`debug_init_isviewer()` in
  libdragon). Some ROMs also require `setHomebrew(true)`.
