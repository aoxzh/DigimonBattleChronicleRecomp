# Desktop gameplay

The desktop port executes the locally supplied GameCube DOL/ISO with the generated PowerPC CPU and pinned gcn-recomp GX/AX components. Current evidence covers title/menu input, character selection, movement/attacks and two complete time battles with score results and return to selection. A paced full-match run also returns to selection; 60 HUD seconds span 60.0006 wall seconds in the measured battle interval. The final SDL playback run completes a full battle and return to selection over about 368 seconds with zero playing-state zero-availability observations, clears, enqueue errors or device stalls. Those measurements used a fixed 350 ms playback buffer; playback now uses an adaptive 80-350 ms buffer (see below). WAV/screenshot-heavy diagnostics have separate recorded continuity failures. Detailed results are tracked in `port/analysis/2026-10-19-reuse-runtime.md`. Polyphase voices (`src_type` 0, the only remaining limitation flag this game raised besides DPOP) now use a 4-tap polyphase interpolator with generated coefficients instead of linear interpolation: same loudness, about 8% more high-frequency energy in menu music, not bit-exact to the DSP coefficient ROM. DPOP stays flagged as partial.

From the repository root:

```sh
cmake -S port -B /tmp/digimon-upstream-gx-build -DCMAKE_BUILD_TYPE=Release -DBUILD_UPSTREAM_AUDIO=ON
cmake --build /tmp/digimon-upstream-gx-build --target main_gx -j6
/tmp/digimon-upstream-gx-build/main_gx port/extracted/main.dol port/disc.iso
```

### Self-build kit (for other people)

`python3 port/packaging/make_kit.py` writes `port/dist/DigimonBattleGC-builder-<version>.zip` plus `SHA256SUMS`: this project's source, the lifter and its versioned inputs, and the build scripts, with nothing from the disc (the packager refuses lifter output, disc/DOL files, images, logs and card images). Recipients run `build-windows.ps1 <disc.iso>` (Windows, MSYS2) or `python3 build.py <disc.iso>` (Linux), which checks the disc (G2DJB2 rev 0 and the main.dol fingerprint), lifts it, verifies the coverage counts and builds a `DigimonBattleGC` folder. Only share the kit; the program it builds contains code translated from the disc. See `port/packaging/README.md`.

### Windows

Build with MSYS2's CLANG64 toolchain (the generated code needs clang/GCC builtins, so MSVC is not supported). Install [MSYS2](https://www.msys2.org/) (`winget install MSYS2.MSYS2`), then in the **MSYS2 CLANG64** shell:

```sh
pacman -S --needed mingw-w64-clang-x86_64-{clang,cmake,ninja,SDL2,pkgconf}
cd /c/path/to/digimonbattlengc/port
cmake -S . -B build/windows -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_UPSTREAM_AUDIO=ON
cmake --build build/windows --target main_gx
```

SDL and the C/C++ runtime are linked statically, so `build\windows\main_gx.exe` is self-contained; copy it anywhere and double-click it. On Windows it is a GUI program (no console window).

## Launcher and settings

Run `main_gx` with no arguments to open the launcher: choose the disc image (Browse, drag and drop, or auto-detected `disc.iso`), then set window size, fullscreen, internal resolution (1x-4x of 640x480), 4:3 or stretched aspect, smooth scaling, VSync, sound/volume, and controller/keyboard mapping. Only the disc image is needed; `main.dol` is read from it and the disc is checked against the recompiled build (Digimon Battle Chronicle, `G2DJB2` rev 0). Compressed Dolphin images (`.rvz`, `.wia`, `.gcz`, ...) must be converted to ISO first.

| Command | Effect |
|---|---|
| `main_gx` | launcher, or straight into the game if "Start the game directly" was ticked |
| `main_gx disc.iso` | start with that disc, no launcher |
| `--setup` | always show the launcher |
| `--fullscreen` / `--windowed`, `--scale N`, `--window WxH` | override settings for this run |
| `--console` | attach to the calling console instead of writing a log file (Windows) |
| `--no-settings` | ignore `settings.ini` (fixed 640x480, original behaviour) |

Data folder: `%APPDATA%\DigimonBattleGC` on Windows (SDL pref path elsewhere), or a `user` folder beside the executable when an empty `portable.txt` is placed there. It holds `settings.ini` (`KEY=VALUE`, written by the launcher), `screenshots\`, and on Windows `logs\session-*.log` (newest 8). Put a [`gamecontrollerdb.txt`](https://github.com/mdqinc/SDL_GameControllerDB) in the data folder or beside the executable to add controllers SDL does not recognise.

| Hotkey | Action |
|---|---|
| F11 or Alt+Enter | fullscreen (Esc leaves fullscreen) |
| F8 | pause / resume (also pauses on focus loss unless disabled) |
| F9 | FPS in the title bar |
| F12 | screenshot (PNG at internal resolution) |

## Controls

Up to four SDL game controllers (Xbox, PlayStation, Switch Pro, ...) take players 1-4 in connection order and can be plugged in or out while playing. The keyboard also drives one player (player 1 by default). Sticks use a radial deadzone; analog triggers report pressure and click L/R at ~85%. Rumble follows the game's motor commands. All buttons are remappable in the launcher.

| GameCube | Controller (default) | Keyboard (default) |
|---|---|---|
| Main stick / C-stick | left / right stick | arrow keys / T F G H |
| A / B / X / Y | A / B / X / Y | J / K / U / I |
| Z | RB / R1 | O |
| L / R | LT / RT (analog) | Q / E |
| START | Start | Return |
| D-pad | D-pad | W A S D |

## Saves

Player runs put a 251-block memory card in slot A: `MemoryCardA.raw` in the data folder (a plain 2 MiB raw image, the same layout Dolphin uses). The first launch creates it already formatted; the game then creates its `DRA2` save (1 block) automatically after the autosave notice and loads it on later boots. Verified so far: creation, reload on the next boot, and a full scripted versus battle with the card inserted (which, with nothing unlocked, did not write the card). Autosaves of story/unlock progress use the same card commands but have not been exercised by a scripted run yet. Changes are written to disk about a second after the game finishes writing, and again on exit, via a temporary file so an interrupted write never replaces the previous card. Untick "Memory card in slot A" in the launcher to play without saving; at the autosave prompts then press A, choose the continue-without-saving option with DOWN, and press A.

Back up the card by copying `MemoryCardA.raw`. Other raw cards of 59-2043 blocks (for example Dolphin `MemoryCardA.JAP.raw`) can be copied in as `MemoryCardA.raw`. The emulated console reports itself as NTSC-J, so its font encoding is Shift-JIS like a Japanese GameCube; a card whose header records the other encoding (older cards from this port, overseas Dolphin settings) has just that header field and checksum rewritten on load. Command-line and hidden runs have an empty slot unless `GC_MEMCARD=<path>` names a card image.

Let the introduction finish, press START at the title, and use the displayed menus.

## Command-line runs

The original positional form still works and is what the verification runs below use (`main_gx main.dol disc.iso [frames] [capture.png] [seconds] [--hidden]`). Hidden runs ignore `settings.ini`. On Windows, redirect stderr (`Start-Process ... -RedirectStandardError log.txt -Wait`) or pass `--console` to see output. Final framebuffer capture defaults to `/tmp/digimon-main-gx.png` (`%TEMP%` on Windows) in this form; player runs capture nothing on exit.

A bounded hidden verification run remains explicit:

```sh
GC_AUDIO_MUTE=1 GC_INPUT_SCRIPT=port/analysis/rendered-boot/battle-controls-input.json \
  /tmp/digimon-upstream-gx-build/main_gx port/extracted/main.dol port/disc.iso \
  12000 /tmp/digimon-battle.png 140 --hidden
```

The optional positional arguments are display limit, capture path, wall seconds and `--hidden`; zero disables each limit. Input scripts use virtual retrace frames, not display submission counts, and own the controller while selected. Keep scripts at or below the 256-event bound. `GC_AUDIO_MUTE=1` suppresses device playback; `GC_AUDIO_WAV=/tmp/battle.wav` records the generated PCM. Visible runs pace the shared guest CPU clock against monotonic wall time. Hidden runs are unpaced unless `GC_REALTIME=1` is supplied.

`GC_AUDIO_TRACE=1` enables bounded audio timing observations (up to 127 periodic lines plus shutdown). Queue sizes and cleared-byte totals are SDL estimates; counters also report rejected enqueues. The deterministic host-sink regression is included in `make -C port test` and can also run with `make -C port test-host-audio CC=clang`; `make -C port test-host-audio-sanitize CC=clang` adds ASan/UBSan. These checks need no audio device.

A paced hidden measurement can use `GC_REALTIME=1` and an explicit wall limit. Exit status 3 with `timeout=1` means that wall limit ended the run; the final image and diagnostics are still captured. Aggregate clock agreement alone does not establish uninterrupted sound. The first 65-second actual SDL test found three audio queue clears and is retained as a failed synchronization measurement in `port/analysis/rendered-boot/paced-audio-before-fix.log`. Real-time playback prebuffers to a queue target before starting the device and applies backpressure above it instead of clearing samples. The target was a fixed 350 ms to cover a measured 309 ms graphics/loading gap; it is now adaptive by default (launcher "Audio latency" = Auto, `audio_latency=0`): it starts at 120 ms; an isolated dropout (typically one loading moment) adds 20 ms, a second within 15 s grows it x1.5+20 ms (max 350 ms), and after 15 s without a dropout it shrinks 10 ms every 5 s down to 80 ms. Measured on the development PC, play stalls the guest 50-90 ms in most seconds and 150-600 ms at loads (the shader cache below removes most of the cold-start ones). A fixed target can be chosen in the launcher or forced with `GC_AUDIO_BUFFER_MS`; the SDL device buffer is 512 frames (16 ms). Player runs also keep a shader cache (`shader_cache.bin` in the data folder; `GC_SHADER_CACHE=<path>` for hidden runs), so shader combinations seen before are built at startup instead of stalling play the first time a scene needs them. A device that pauses, fails an enqueue, or stops draining for 500 ms is logged and reopened after two seconds (headset unplugged, output switched); `GC_AUDIO_STRICT=1` restores the explicit fatal diagnostic for measurement runs. Host stalls longer than one second (window drag, pause, debugger) rebase the pacing clock instead of fast-forwarding to catch up. Unpaced diagnostic playback retains its older half-second queue-clear recovery. Initialization time is excluded from the gameplay pacing epoch.

Local game assets and RAM snapshots are not part of the upstream vendored code or the published source. See `port/third_party/gcn-recomp` for the fixed upstream revision, MIT notices and adaptations.
