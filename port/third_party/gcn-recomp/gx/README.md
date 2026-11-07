# Graphics-only gcn-recomp reuse

Source: <https://github.com/ethannicholas/gcn-recomp> at commit
`3fad2f2deea7e563755819cf6f44da8743c61c55` (the supplied pinned checkout).
The license and copyright notice are preserved in [../LICENSE](../LICENSE).
The generated glad loader retains its own source/header license notices.

Vendored from `runtime/gx/`: `cmd.cpp`, `xf.cpp`, `texture.cpp`, `texture.h`,
`shadergen.cpp`, `shadergen.h`, `render_gl.cpp`, `render_gl.h`, `render.h`,
`gx.h`, `gl.h`, `gl_load.cpp`, `gl_msrtt.h`, and `glad/`.
`png.cpp` comes from upstream `runtime/png.cpp`.
Upstream `fifo.cpp`, CPU/recomp runtime, OS scheduler, audio and MMIO are not
part of this graphics target. Existing `../cp_fifo.*` remains a separate HAL
component.

The upstream GX parser, vertex decoding, transform/lighting/texgen, primitive
assembly, all texture formats, TEV shader generation and GL rendering are used
as a complete pipeline. Raw commands remain guest big-endian hardware commands;
CP ARRAYBASE and CALL_DL addresses resolve into the existing 24 MiB MEM1.

## Local adaptation

The four GX translation units that used `../runtime.h` now include the desktop
adapter's `upstream_runtime.h`; XF's platform include uses `upstream_platform.h`.
The shims provide RAM access, endian conversion, VI display height, PE callbacks,
logging, and debug hooks. They do not contain another CPU or hardware model.

`DIGIMON_GX_ADAPTER` adds synchronous batch dispatch in `xf.cpp`, plus lifecycle
reset/teardown helpers in XF, texture and GL code. The desktop adapter forces
upstream `GCN_XF_SYNC=1` and calls upstream `render_execute` on the GL owner thread.
No upstream frontend/transform worker is started. This makes texture-copy RAM
writeback complete before the following PE finish reaches the existing HAL.
The copy-writeback filter accepts every supported texture copy. Reset clears
pending commands, BP/CP/XF/TEV state, texture/TLUT/XFB caches, counters and EFB;
shutdown releases GL resources and the adapter's SDL resources.

`cmd.cpp` additionally validates the entire CALL_DL byte span against MEM1
before either parser (`process` or `skim`) dereferences it. It rejects invalid
addresses, end overruns and nesting beyond 64 lists with the raw/physical
address, decimal/hex length, parent list coordinates, root FIFO position,
nine command bytes and nearby parent bytes. Address validation uses the
upstream CALL_DL mask before the narrower RAM-pointer mask, so malformed
addresses cannot silently wrap into MEM1. Exceptions unwind the nesting stack;
the adapter retains the first stream error and rejects subsequent consumption
until reset. Failed display lists are never skipped or retried.
Regression coverage includes the observed `0xF8440359` length, a split header,
MEM1 edge/aliases, nested failure coordinates, self-recursion, sticky errors
and reset recovery. `/tmp/digimon-gx-bounds-fixture.log` records a passing run.
The earlier `%X` copy warning `format 10` means format `0x10`; its relationship
to the malformed list remains unestablished.

`platform/desktop/upstream_gx/adapter.cpp` owns SDL context creation and an RGBA8
output FBO. Hidden mode still uses a real SDL/OpenGL context, requires an
available desktop video driver, and captures that offscreen FBO. It supports
macOS system OpenGL and other desktop platforms through vendored glad.

The only diagnostic stubs are thread naming, stall accounting, guest symbol
names and heap profiling. Optional frame/DMA debug callbacks forward through
the C configuration. PE token/finish and VI height are real caller callbacks.
No weak graphics backend or software-renderer fallback is used. The renderer
inherits upstream rendering limitations and its unused VR features; flat GX
rendering is the tested path. Display copies are represented by the renderer's
XFB textures; this integration does not additionally encode those copies into
YUY2 guest RAM.

## Build and run

Existing SDL2, CMake, C/C++17 compiler and desktop OpenGL are required. No packages
are downloaded by CMake. The Makefile/headless build remains independent.

```sh
cmake -S <repo>/port \
  -B /tmp/digimon-upstream-gx-build -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/digimon-upstream-gx-build --target main_gx gx_fixture -j6
ctest --test-dir /tmp/digimon-upstream-gx-build --output-on-failure
/tmp/digimon-upstream-gx-build/main_gx \
  <repo>/port/extracted/main.dol \
  <repo>/port/disc.iso \
  10 /tmp/digimon-main-gx-game.png 60 --hidden
```

Arguments after disc: display-frame limit (0 for interactive/unlimited), final
PNG path, wall-clock seconds, optional `--hidden`. The timeout first requests a
normal exit/capture from the VI callback. If the guest stops servicing callbacks,
a second alarm forces exit 124 after two additional seconds without a capture.
`GC_CAPTURE_DIR` selects an existing directory for periodic PNGs;
`GC_CAPTURE_EVERY` selects their display-frame interval (default 60).
`GC_INPUT_SCRIPT` enables the existing HAL deterministic replay and disables
host pad writes so neutral input cannot overwrite scripted events.
`GC_RAM_DUMP` writes a complete 24 MiB RAM image on normal exit; the runner also
prints CPU registers, disc-read counters and FIFO/display totals. Visible mode
paces presentation at approximately 60 Hz; hidden mode runs without pacing.

`BUILD_UPSTREAM_AUDIO=ON` optionally links the separately owned AX adapter and
HAL audio path (default OFF). Its host sink queues stereo PCM through SDL;
`GC_AUDIO_MUTE=1` disables device playback and `GC_AUDIO_WAV=/tmp/audio.wav`
records the exact received PCM. Exit logs contain PCM/nonzero frame counts,
peak amplitude, ucode identity and sticky audio limitations. Enabling this
option requires the audio adapter sources to be present.

Keyboard: arrow keys = main stick, WASD = D-pad, J/K/U/I = A/B/X/Y, Return =
Start, Q/E = L/R, O = Z. The first SDL game controller is also supported.
The runner passes active-high raw button state to `hal_si_set_pad_raw`.

The C bridge is declared in `platform/desktop/upstream_gx/upstream_gx.h`.
After `hal_boot_prepare`, initialize with `ram=gc_ram`, `ram_size=0x1800000`,
output size and callbacks. Forward each CP-consumed byte range once through
`upstream_gx_consume`. The optional `GC_UPSTREAM_GX` HAL path bypasses its old
parser and calls this bridge, so display lists and PE events are not doubled.
PE callbacks call `hal_gx_signal_finish` and `hal_gx_signal_token`; VI height
comes from HAL timing registers. `consume`, `present`, capture, reset and
shutdown all run on the SDL GL owner thread. `present` pumps events and blits
new output to a visible window. `debug_frame` occurs before upstream batch
flush, so final-frame capture/termination belongs after `consume` or in the
runner's VI callback, rather than inside that debug callback.

## Verification

`gx_fixture` sends FIFO commands in seven-byte fragments, initializes a tiled
RGB565 four-color texture and indexed position array in borrowed MEM1, sets
XF/TEV state, draws a quad, executes EFB clear/display and texture copies, and
checks token/finish callbacks and synchronous RGB565 writeback to MEM1.
It asserts red/green/blue/yellow/clear pixel counts, repeats after reset, then
repeats after shutdown/reinitialization. Output is `gx_fixture.png` in the
CMake build directory.

On 2026-10-19, macOS arm64 Apple M4 / OpenGL 4.1 Metal passed the fixture and
actual generated-Gekko DOL boot. Ten display frames produced the Japanese
autosave notice in `/tmp/digimon-main-gx-game.png`. A deterministic 300-frame
run accepted A and displayed the memory-card menu. A 1,000-display-frame run
selected no-save and rendered the animated 3D intro, with checkpoints under
`/tmp/digimon-main-gx-nosave/` and final capture `final.png`. This confirms the
real guest command/RAM path; it does not by itself establish complete gameplay.

The optional audio build also completed an actual 1,000-display-frame intro run:
1,277,600 stereo PCM frames at 48 kHz, 239,830 nonzero frames, peak 25,024.
`/tmp/digimon-gx-audio-intro.wav` contains the captured PCM. Ucode `07f88145`
ran 1,596 command lists; sticky limitations `0x23` identify compressor,
polyphase and DPOP approximations, so this is diagnostic audio output.

## Original source SHA-256

These hashes describe the supplied pinned originals, before local adaptations.
Paths below are relative to upstream `runtime/`.

| Source | SHA-256 |
| --- | --- |
| `gx/cmd.cpp` | `a3a89230f6bc517a901ecc34b8202f7c039d7d430764e069bec51649ed553d8f` |
| `gx/xf.cpp` | `8f7af8854e2b7e77f59c815bba94f9cffd329da69d7a9ddbe1a75f6b9df5edb0` |
| `gx/texture.cpp` | `686b2291c4f8ed87b0982eac80f58e96abceac42e6a03fffe0860b986d23630c` |
| `gx/texture.h` | `37f76bba97b12a27ed10fbd3c4f687bfa68e9b02fa8a07e8d2c0fcc2696fdad5` |
| `gx/shadergen.cpp` | `8e69c4eab2eace0f706ee1e40911484e6eedd813d8286f9a48fe9d4b27a6624d` |
| `gx/shadergen.h` | `d026dacdff7195f483401bb70088a6f46e74664b64ef2751b26b030185c4933a` |
| `gx/render_gl.cpp` | `b6ab90df124ae01f311e858b6a8d5d4844435b65ce77ead1aa4ec611801b9f4d` |
| `gx/render_gl.h` | `3eede80add50d7ee39edd4a3f5bb4d20a9e81bde1150d71fb5f09764f839183d` |
| `gx/render.h` | `c971b570f9bb14d33565df665ce131fe3d43fe922d4dc951155b60ad3b713fb9` |
| `gx/gx.h` | `8df7e386e5d7ff60648ca3ac5d6286d2ff93046aafa9240840b8883dbc7a4160` |
| `gx/gl.h` | `bf29a45047a9b1d543ea3f30fe899124ef275f226b101c421fa686dae3d0db64` |
| `gx/gl_load.cpp` | `627086ab729878c8862982573848dbb6133c242411226076268fdf01e0b58a73` |
| `gx/gl_msrtt.h` | `a298d3d5dfd73ddf15a037c50871bfe3ab7944e0d72f2bf8a2c1f3604bc0183a` |
| `gx/glad/src/gl.c` | `421def813ec71345297c1a707a738cb2b01358ef8bf849f64ba45fad66277466` |
| `gx/glad/include/glad/gl.h` | `90befb1c66d3677837b9bd6b58d8c9bf99a39807fe427b12a3ed74b06e57cfd2` |
| `gx/glad/include/KHR/khrplatform.h` | `7b1e01aaa7ad8f6fc34b5c7bdf79ebf5189bb09e2c4d2e79fc5d350623d11e83` |
| `png.cpp` | `e66ab03ddc84ccdbf16ebeb9300781d361f45ee95724f5a19b95d250582954b3` |
