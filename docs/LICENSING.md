# Licensing and provenance

This repository combines this project's own code with several third-party components. Each keeps its own license; the root [LICENSE](../LICENSE) (MIT) covers only this project's own code.

| Component | Where | License | Notes |
|---|---|---|---|
| This project (recompiler, HAL, launchers, build scripts, tests) | `port/hal`, `port/tools`, `port/platform`, `port/android`, `port/packaging`, `port/tests`, `port/recomp/runtime_glue.c` | MIT, [LICENSE](../LICENSE) | |
| gcn-recomp (GX renderer, AX mixer, CP FIFO) | `port/third_party/gcn-recomp` | MIT, [LICENSE](../port/third_party/gcn-recomp/LICENSE) | Pinned upstream commit and local changes are described in its README and `audio/PIN.json`. Later fixes ported from upstream (for example the deferred EFB copy write-back) are marked in the source. |
| Dear ImGui | `port/third_party/imgui` | MIT, [LICENSE.txt](../port/third_party/imgui/LICENSE.txt) | |
| SDL2 | `port/third_party/SDL2` | zlib, [LICENSE.txt](../port/third_party/SDL2/LICENSE.txt) | Vendored for the Android build; desktop builds may use the system SDL2. |
| stb_image | `port/third_party/stb` | MIT or public domain (your choice), notice at the end of `stb_image.h` | Upstream commit in `COMMIT`. |
| glad loaders | `port/third_party/gcn-recomp/gx/glad*` | Generated code: (WTFPL or CC0-1.0) and Apache-2.0, per the SPDX header | |

Reference-only material (Dolphin's GPL sources, consulted for hardware behaviour) was read, not copied; source comments say where a behaviour was checked against it.

## Game content

*Digimon Battle Chronicle* (デジモンバトルクロニクル) and all Digimon names, characters, artwork, audio and code belong to their respective rights holders and are not licensed by anything in this repository. The repository contains no game data and no generated game code. Users supply their own legally obtained disc image, and the programs the build scripts produce contain code translated from that disc: they are for the user's own use and must not be redistributed.

The documentation screenshots and the launcher background image (`port/platform/desktop/upstream_gx/launcher_bg.jpg`, `port/android/app/src/main/res/drawable-nodpi/launcher_bg.webp`) are included for illustration; their rights remain with their owners and the MIT license does not apply to them. Replace the background image if you redistribute builds and are unsure of its rights.
