# Working in this repository

Notes for human contributors and coding agents alike.

## What this is

A static recompilation of the Japanese GameCube game *Digimon Battle Chronicle* (G2DJB2, revision 0): `port/tools/lift_dol.py` translates the disc's `main.dol` from PowerPC to C, and `port/hal/` plus `port/third_party/gcn-recomp` provide the GameCube hardware, renderer and audio. Desktop builds use CMake (`port/CMakeLists.txt`); Android uses Gradle (`port/android`).

## Reproducing a build for a user

A user can ask an agent: *"Build and start Digimon Battle Chronicle Recomp on this machine for my <Windows PC / Linux PC / Android phone>. My disc image is at <path>."* Then:

1. Check the host: Python 3.8+, CMake 3.20+, Ninja, clang/clang++, SDL2 development files (Windows: MSYS2 CLANG64 — `port/packaging/build-windows.ps1` installs these). Android additionally needs the SDK (platform 36), NDK 29, CMake 3.31 and Gradle 9.6+.
2. Run `python3 port/packaging/build.py <disc>` (on Windows: `port/packaging/Build-Windows.bat` or `build-windows.ps1 <disc>`). It verifies the disc, lifts `main.dol` into `port/recomp/`, checks the expected block and instruction counts, and builds `port/packaging/DigimonBattleGC/`. If it fails, read the `build-report-*.txt` it writes and fix the cause rather than bypassing a check.
3. For Android, after step 2: `cd port/android`, write `local.properties` with `sdk.dir`, `gradle assembleRelease`, install the APK, then pick the disc in the app's launcher.
4. Finish by actually starting the game with picture, sound and input — a passing unit test is not a working game. Ask the user only for missing paths or genuine choices.

The standalone tests (`make -C port selftest`, `make -C port test-host-audio`, and the Python tests under `port/tests/`) need no game.

## Private inputs and outputs

Never commit or upload: disc images, `main.dol`, anything under `port/recomp/` produced by the lifter (`generated/`, `blocks.c`, `block_decls.h`, `gen_prefix.h`, `recomp_entry.c`, `coverage.json`), built programs or APKs, memory cards, RAM dumps, logs with local paths, or captured game audio/video. `.gitignore` is a safeguard, not a privacy boundary: review what you stage.

Some source comments refer to analysis notes from the original development that are not part of this repository; treat the code and tests as the evidence.

## Changes

Keep edits scoped, preserve license headers and third-party notices (see [docs/LICENSING.md](docs/LICENSING.md)), and prefer porting a fix from upstream gcn-recomp or other mature GameCube projects over inventing a new mechanism; cite the source when you do.
