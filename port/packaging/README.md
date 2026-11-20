# Digimon Battle Chronicle self-build kit

This kit contains **no game data or code**. It builds the game for your PC from **your own disc image**; the resulting program contains code translated from that disc, so keep it to yourself and do not share it.

It carries the project's recompiler, GameCube hardware layer, renderer, audio mixer, launcher and build scripts — the same sources as the repository, with no generated code.

## Requirements

- An image of the Japanese GameCube disc **G2DJB2, revision 0** or the USA disc **GD6EB2, revision 0**: `.iso` / `.gcm`, or Dolphin `.rvz` / `.gcz` / `.ciso` (imported to ISO first; `.wia` must be converted to ISO in Dolphin). RVZ import needs Python's `zstandard` (`python -m pip install zstandard`).
- About 2 GB of free disk space; a first build takes roughly 5–15 minutes.
- A GPU with OpenGL 3.3.

## Windows

**Double-click `Build-Windows.bat`** and pick your disc image in the dialog (or drop the image onto the .bat). If [MSYS2](https://www.msys2.org) is missing, the script offers to install it with winget, then installs clang, CMake, Ninja, SDL2 and Python itself.

When it finishes:

- the game is in the `DigimonBattleGC` folder (the script opens it);
- `DigimonBattleGC-<version>-windows-amd64.zip` is written beside it for your own backup. It contains code translated from your disc: **do not share it**;
- if a step fails, `build-report-<time>.txt` appears in this folder with the failed step, tool versions and the full build output (no game data). Attach it to an issue.

Command line:

```powershell
powershell -ExecutionPolicy Bypass -File build-windows.ps1 "D:\Games\Digimon Battle Chronicle (Japan).iso"
```

`-Portable` keeps settings, saves and logs in a `user` folder next to the program; `-NoZip` skips the backup zip. The script sets up clang, CMake, Ninja, SDL2 and Python through MSYS2 CLANG64. Then run `DigimonBattleGC\DigimonBattleGC.exe`; the exe is statically linked and runs from anywhere.

A compressed image is imported once to `cache/<image name>.iso` (in the user data folder, or `user/cache` with `-Portable`); the builder reads `main.dol` from that ISO, and the launcher can keep using the original `.rvz` / `.gcz` / `.ciso`.

## Linux (x86-64)

```sh
# Debian/Ubuntu
sudo apt install python3 cmake ninja-build clang pkg-config libsdl2-dev libgl-dev
# Fedora
sudo dnf install python3 cmake ninja-build clang pkgconf-pkg-config SDL2-devel mesa-libGL-devel

python3 build.py "/path/to/Digimon Battle Chronicle (Japan).iso"
# with no path it looks in ~/Downloads, ~/Games and similar folders, or opens a file dialog
./DigimonBattleGC/DigimonBattleGC
```

## What the builder does

1. Checks the disc: ID `G2DJB2` (Japan) or `GD6EB2` (USA), revision 0, and the `main.dol` fingerprint the port was tested with. A mismatch stops the build instead of producing a broken port.
2. Translates the PowerPC code of `main.dol` to C with `src/tools/lift_dol.py`: about 166,000 basic blocks, byte-identical to the output the project was tested with.
3. Compiles it with CMake and clang into the `DigimonBattleGC` folder (program, `PLAYING.md` and third-party licences).

## Playing

Start the program and pick the disc image in the launcher (or drag it onto the window); set resolution, fullscreen and controllers, then Play. Controls, hotkeys and save locations are in `PLAYING.md`. Saves are standard GameCube memory card images (`MemoryCardA.raw`) and work with Dolphin in both directions.

## Licenses

The renderer and AX mixing come from gcn-recomp (MIT, `src/third_party/gcn-recomp/LICENSE`); the launcher UI uses Dear ImGui (MIT); the OpenGL loader is generated glad code; SDL2 comes from your system or MSYS2 (zlib licence).

---

# 数码宝贝大乱斗 GC 自建包

这个包**不含任何游戏内容**：只有本移植项目自己的源码（硬件模拟层、PowerPC→C 翻译器、渲染/音频/启动器）和构建脚本。游戏程序要用**你自己的光盘镜像**在你的电脑上生成，生成的程序含有从光盘翻译出的代码，**仅供自己使用，不要分享**。

## 需要

- 日版光盘镜像《デジモンバトルクロニクル》`G2DJB2`，或美版 *Digimon Rumble Arena 2* `GD6EB2`，版本 0，`.iso`/`.gcm`/`.rvz`/`.gcz`/`.ciso`。RVZ 导入需要 Python 的 `zstandard`（`python -m pip install zstandard`）；WIA 请先在 Dolphin 里右键 → 转换文件 → ISO。
- 约 2 GB 空闲磁盘，首次构建约 5–15 分钟。
- 支持 OpenGL 3.3 的显卡。

## Windows

**双击 `Build-Windows.bat`**，在弹出的对话框里选你的光盘镜像即可（也可以把镜像直接拖到 `Build-Windows.bat` 上）。没装 [MSYS2](https://www.msys2.org) 时脚本会询问并用 winget 自动安装，之后自动装好 clang、CMake、Ninja、SDL2 和 Python。

完成后：

- 游戏在 `DigimonBattleGC` 文件夹（脚本会自动打开它）；
- 同时生成 `DigimonBattleGC-<版本>-windows-amd64.zip`，方便自己备份/换电脑。它含有从你光盘翻译出的代码，**不要分享**；
- 如果某一步失败，本文件夹会出现 `build-report-<时间>.txt`，记录失败步骤、工具版本和完整构建输出（不含任何游戏数据），求助时附上它即可。

命令行用法：

```powershell
powershell -ExecutionPolicy Bypass -File build-windows.ps1 "D:\Games\Digimon Battle Chronicle (Japan).iso"
```

加 `-Portable` 会把设置、存档、日志放在程序旁边的 `user` 文件夹；`-NoZip` 跳过打包 zip。生成的 exe 是静态链接的，复制到别处也能运行，不需要 MSYS2。

压缩输入会先导入到 `cache/<镜像名>.iso`（非 portable：用户数据目录；portable：程序旁 `user/cache`），构建器从同一份 ISO 读取 `main.dol`；启动器仍可选择原始 `.rvz/.gcz/.ciso`，不用手动再转换一次。

## Linux (x86-64)

```sh
# Debian/Ubuntu
sudo apt install python3 cmake ninja-build clang pkg-config libsdl2-dev libgl-dev
# Fedora
sudo dnf install python3 cmake ninja-build clang pkgconf-pkg-config SDL2-devel mesa-libGL-devel

python3 build.py "/path/to/Digimon Battle Chronicle (Japan).iso"
# 不写路径时会在 ~/Downloads、~/Games 等目录里找，或弹出文件选择框
./DigimonBattleGC/DigimonBattleGC
```

## 构建做了什么

1. 检查光盘：ID `G2DJB2`（日版）或 `GD6EB2`（美版）、版本 0、`main.dol` 指纹与本移植测试用的一致（不一致会拒绝，避免改版/坏档导致的奇怪问题）。
2. 用 `src/tools/lift_dol.py` 把 `main.dol` 的 PowerPC 代码翻译成 C（约 16.6 万个代码块，结果与项目测试用的逐字节一致）。
3. 用 CMake + clang 编译，生成 `DigimonBattleGC` 文件夹（程序、玩法说明 `PLAYING.md`、第三方许可证）。

## 玩

启动后在启动器里选择光盘镜像（也可拖进窗口），设置分辨率、全屏、手柄等，点 Play。按键、热键、存档位置见 `PLAYING.md`。存档是标准的 GameCube 记忆卡镜像 `MemoryCardA.raw`，可与 Dolphin 互通。

## 许可

渲染器与 AX 混音来自 gcn-recomp（MIT，`src/third_party/gcn-recomp/LICENSE`），启动器界面用 Dear ImGui（MIT），OpenGL 加载器为 glad 生成代码；SDL2 由你的系统/MSYS2 提供（zlib 许可）。
