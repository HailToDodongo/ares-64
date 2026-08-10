# Ares 64

[![License: ISC](https://img.shields.io/badge/License-ISC-blue.svg)](https://github.com/higan-emu/ares/blob/master/LICENSE)

<img src="desktop-ui/resource/screenshot00.png" width="420px">

Original Repo: https://github.com/ares-emulator/ares

This is a special version of ares only containing the N64 core, with additional features geared towards developers.

If you plan on just playing games, please use the original repo.
If you want to improve accuracy or make changes to the N64 core itself, please also contribute upstream.

# Features

Additional features include (checkout the 'tools' menu):

- Framebuffer viewer that updates in realtime
  - Color / Coverage / Depth
  - Depth range can be tuned
  - Depth Histogram
  - Overdraw heatmap for depth/color
- RDP logging + single stepping
  - stepping updates the framebuffer in realtime
- RSP logging + single stepping (libdragon only)
  - stepping updates the framebuffer in realtime
  - supported commands can be decoded into readable arguments
  - metrics to measure RSP times
- TMEM viewer with tile preview
- Memory viewer and editor
- Audio visualizer
- rendering via ParallelRDP and angrylion (swappable mid-game)
- SD/HD/UHD for ParallelRDP changable mid-game
- UI that can be freely organized and is saved across starts

For single stepping, use the hotkey for frame-advance after pausing emulation.<br>
In the top-right you are also given the option what stepping mode is currently active.

Some features are only available with angrylion, you can select the renderer in the top-right.

# Build

Please make sure to also checkout all submodules via git.

## Requirements

- **CMake** 3.28 or later
- **C++23** compiler
- **SDL3** is bundled in `thirdparty/`, no system install needed

### CMake

```sh
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --target desktop-ui -j8
```

The binary will be at `build/desktop-ui/ares`.

For faster iterative builds, use:
```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DENABLE_IPO=OFF -DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=lld -DCMAKE_SHARED_LINKER_FLAGS=-fuse-ld=lld
```

# Running

Start ares while having the working directory in this repo.<br>
You can pass the ROM as the argument:
```sh
./build/desktop-ui/ares /path/to/game.z64
```

For any commercial games that will be enough to get all features except RSP commands working.<br>
For libdragon ROMs, you can make sure to keep the `.elf` file around.<br>
Either next to the ROM, or in a `build` directory next to it.<br>
This will enable fetching labels used to figure out the overlay setup.<br>

## Command-line options

Pass a N64 ROMs as a positional argument, plus any of the options below:

| Option | Description |
| --- | --- |
| `--help` | Display the available options (and the list of supported systems) and exit |
| `--version` | Display the version string and exit |
| `--system name` | Specify the system name to load the game with |
| `--setting name=value` | Override a single setting for this run (repeatable) |
| `--dump-all-settings` | Print every available setting path and exit |
| `--no-file-prompt` | Do not prompt to load optional additional ROMs (e.g. 64DD) |
| `--settings-file path` | Use an alternate `settings.bml` file |
| `--save-state slot` | Load a save state slot on startup (`1`-`9`) |
| `--dump-log spec` | Dump the RSP/RDP command log to stdout, then quit (see below) |
| `--terminal` | Windows only: open a new terminal window for stdio |

### `--dump-log`

Dumps the RSP and/or RDP command log to stdout then quit. Useful for profiling and diffing command streams programmatically. 
The viewer windows do not need to be open.

```
--dump-log <targets>:<after-frames>[:<frame-count>]
```

- `targets`: `rsp`, `rdp`, `rsp+rdp` (or `all`)
- `after-frames`: number of presented frames to skip before dumping
- `frame-count`: number of consecutive frames to dump (default: `1`)

Output is tab-separated, one command per row, with a `=== frame N (R RSP + D RDP commands) ===`
header per frame. When both logs are requested they are merged into a single,
causally-ordered stream (by capture order), so you can see which RSP command
spawned which RDP commands. Every row starts with a `Src` column (`RSP` or `RDP`):

```
Src   #   us   Ovl   Command   Data   Hex
```

RDP rows leave the `us`/`Ovl` columns empty and fill `Hex` with the raw command word;
RSP rows leave `Hex` empty.<br>
As with the viewers, RSP command/overlay names require a
libdragon `.elf` alongside the ROM.

```sh
# After 120 frames, dump 3 frames of both logs, then exit
./build/desktop-ui/ares --dump-log rsp+rdp:120:3 /path/to/game.z64 > log.tsv
```

## Headless testing (`ares-test`)

A third build flavor next to the normal and play-mode builds: a JavaScript-scripted
CLI runner for automated testing that needs **no window, no GPU, and no audio
device** — it works in bare CI containers. Rendering uses the angrylion CPU renderer
(or `none`, which skips RDP rendering entirely and scans out whatever the game wrote
to RDRAM); paraLLEl-RDP and its ~40 Vulkan translation units are not compiled at all
(`ARES_ENABLE_VULKAN=OFF`). Scripts run on an embedded QuickJS-NG runtime
(`thirdparty/quickjs-ng`).

```sh
cmake --preset linux-headless
cmake --build build_headless --target ares-test -j10
./build_headless/test-runner/ares-test my-test.js [script-args...] [--timeout sec]
```

The emulator boots with no ROM and **paused**; the script drives it through the
global `ares` object. All calls are synchronous — emulation advances inline inside
`wait*()`/`screenshot()` and always in *emulated* time, so runs are fully
deterministic (`Deterministic Entropy` is forced on and there is no audio clock):

```js
ares.setRenderer("angrylion");        // or "none"; callable at any time — with a
                                      // ROM loaded it hot-swaps at the next frame
                                      // boundary (e.g. boot under "none" for speed,
                                      // enable angrylion before the frames you assert on)
ares.loadRom(ares.args[0]);           // throws on failure
ares.resume();                        // pause() / resume() / isPaused()

ares.wait(2.0);                       // run for 2 emulated seconds
ares.waitVI();                        // run until the next VI scanout
ares.waitFrames(30);                  // ares.frameCount() reads the counter

const p1 = ares.controller(1);        // ports 1..4
p1.hold("A").release("B");            // A B Z L R Start Up Down Left Right C-Up ...
p1.stick(0.5, -1.0);                  // analog, [-1, +1]; p1.clear() resets all

const shot = ares.screenshot();       // advances to the next frame, captures in memory
// -> {width, height, sha256, data, save(path), compare(other, tolerance?)}
//    data:   ArrayBuffer of RGBA bytes (wrap with new Uint8Array(shot.data))
//    sha256: hash of the raw RGB pixels (stable golden key — PNG bytes vary
//            when optipng is installed)
shot.save("out.png");

ares.startAudio();                    // records at the AI output rate the game
const rec = ares.stopAudio();         // programmed; startAudio(48000) forces a rate
// -> {samples, frequency, data, save(path), compare(other, tolerance?)}
//    data: ArrayBuffer of interleaved stereo s16 (new Int16Array(rec.data))
rec.save("out.wav");

// snapshot testing: load goldens back and diff them
const golden = ares.loadImage("golden.png");   // same object shape as screenshot()
const cmp = shot.compare(golden);              // or compare("golden.png") directly
// -> {match, reason?, diffPixels, totalPixels, maxDelta, avgDelta}
//    tolerance (default 0) = max per-channel delta still counted as equal
if (!cmp.match) throw new Error("snapshot mismatch: " + JSON.stringify(cmp));
ares.loadAudio("golden.wav");                  // audio equivalent; audio.compare()
                                               // -> {match, reason?, diffSamples, ...}
const db = rec.snr("golden.wav");              // quality metric instead of bit-exact:
if (db < 40) throw new Error("audio too noisy: " + db + " dB");
// SNR in dB of `rec` against the reference (argument: object or WAV path);
// Infinity when identical. Frequencies must match; differing lengths are
// compared over the overlapping prefix.
const aligned = rec.trim();           // new audio object with leading/trailing
// silence removed (a single zero frame survives on each side) — align recordings
// before compare()/snr(), since the lead-in depends on when startAudio() ran
const jingle = rec.slice(0.5, 1.5);   // [start, end) subsection in seconds;
// end optional (defaults to the recording's end), negatives count from the end
// (Array.prototype.slice conventions); out-of-range clamps

// ISViewer text (libdragon debugf / libultra osSyncPrintf) is echoed to stdout:
if (!ares.waitLog("TEST PASSED", 10)) // run until marker appears (emulated seconds)
  throw new Error("ROM self-test failed: " + ares.log());
ares.clearLog();

ares.closeRom();                      // reset(hard), setHomebrew(bool), exit(code)
```

Exit codes: `0` = script completed, `1` = uncaught JS exception (plain `throw` is the
assert mechanism), `2` = usage error or `--timeout` watchdog (default 120s wall-clock,
the only wall-clock element — it guards CI against hung ROMs and runaway scripts).

Scripts can share common logic via **ES modules**: a script containing `import`/
`export` is automatically evaluated as a module, and relative imports resolve
against the importing file (`import { assertSnapshot } from "../lib/assert.js"`).
Modules are cached per path, so shared state in a module is shared between
importers. `tests/lib/assert.js` ships assertion helpers including
`assertSnapshot(media, goldenPath, tolerance?)`, which creates the golden on first
run and compares against it afterwards. Scripts without `import`/`export` are
evaluated as plain (non-strict) global scripts, unchanged.

`tests/run-tests.sh` runs `tests/*.test.js` (committed, ROM-free — e.g. the scripting
smoke test) plus `tests/local/*.test.js` (machine-local, gitignored — put scripts
with private ROM paths there). The `Headless Test Runner` GitHub workflow builds
`ares-test` and runs the committed scripts on every PR without any X11/GL/audio
packages installed.

The `RDP: none` renderer is also selectable in the normal desktop UI, and a regular
desktop build can be configured with `-DARES_ENABLE_VULKAN=OFF` (angrylion/none only).

### Docker image (`libdragon-ares-test`)

The [Dockerfile](./Dockerfile) builds a combined N64 homebrew CI image: the libdragon
toolchain + library (built from a local libdragon checkout, passed as an extra build
context) and `ares-test`, so one container can compile a ROM and run it headless:

```sh
podman build --build-context libdragon=../libdragon -t libdragon-ares-test .

podman run --rm -v "$PWD":/app -w /app libdragon-ares-test make
podman run --rm -v "$PWD":/app -w /app libdragon-ares-test ares-test test.js game.z64
```

## Adapting overlays

Some data that can't be fetched form the `.elf` will be loaded from a JSON file.<br>
To change this data checkout [rspq-libdragon.json](./desktop-ui/ui/rsp-overlays/rspq-libdragon.json).
After making changes, just rebuild ares.

For per-ROM changes, place this JSON next to the `.elf` file.

# Internal changes

If you are familiar with the original code of ares, some larger changes where made in this version.

All cores except N64 are gone.<br>
For the window itself, rendering, audio as well as input, SDL3 is now used.<br>
This means no more drivers and driver settings.<br>
For anything UI, imgui is used, so native UI toolkits are no longer needed.

The `hiro` code is completely deleted, `ruby` is only a few small wrappers around SDL3.<br>
In general, large portions of now unused code have been removed.

Rendering is now supported via ParallelRDP or angrylion, which can be changed while the game is running.

Keep in mind this project is still very much WIP, so many parts are still left-over and not removed yet (mostly options in UIs).

## Why so many changes?

Imgui is the perfect tool to make debugging UIs like this, at which point it makes little sense to draw the bit of menu UI with something else.<br>
Especially since it cuts out any platform specific code.<br>
SDL3 is the same story, by using the GPU API, rendering is abstracted from both OS and graphics API.<br>
Only vulkan remains as a requirement due to ParallelRDP.

The general code stripping is both due to reducing complexity, build size and compilation times.<br>

With other projects of mine ([Pyrite64](https://github.com/HailToDodongo/pyrite64)), i may also use this as a base for future "PC versions" of games.<br>
Where ROMs can be bundled with ares, and potentially include extended features / rendering.<br>
In which case a simpler version without driver settings becomes easier to manage.

The exception to this is the N64 core, besides hooks to make certain features work.<br>
Emulation itself is not touched, and should only be changed upstream.

## Updating from upstream

Due to the deletions, merge conflicts **will** happen.<br>
However they are easy to solve, for that reason a script is included to help with that.<br>
This will auto accept deleted files on our end, as well as completely changed files that only share the name still.<br>

Anything left are true conflicts independent of any refactors.<br>
Those can happen if changes near the added hooks where made.<br>
Always prefer the upstream code as close as possible, and adapt hooks and changes around it.

Before running the script, make sure you are on the `imgui` branch.<br>
The upstream remote must be setup and be called `origin`, this repo here must be a different remote name.

After that just run: `./scripts/pull-upstream.sh`.<br>
On either success or conflict, it will give further logging.

## Modified Repos

This project uses a modified and directly embedded version of angrylion.
The specific repo it is based on is: <https://github.com/Thar0/angrylion-rdp-plus>
Nothing of the rendering was changed, only new things added to capture metrics.