# 数码宝贝大乱斗 GC 自建包 / Digimon Battle Chronicle self-build kit

这个包**不含任何游戏内容**：只有本移植项目自己的源码（硬件模拟层、PowerPC→C 翻译器、渲染/音频/启动器）和构建脚本。游戏程序要用**你自己的光盘镜像**在你的电脑上生成，生成的程序含有从光盘翻译出的代码，**仅供自己使用，不要分享**。

This kit contains no game data or code. It builds the game for your PC from **your own disc image**; the resulting program contains code translated from that disc, so keep it to yourself.

## 需要 / Requirements

- 日版光盘镜像《デジモンバトルクロニクル》`G2DJB2`，版本 0，`.iso`/`.gcm`/`.rvz`/`.gcz`/`.ciso`。RVZ 导入需要 Python 的 `zstandard`（`python -m pip install zstandard`）；WIA 请先在 Dolphin 里右键 → 转换文件 → ISO。
- 约 2 GB 空闲磁盘，首次构建约 5–15 分钟。
- 支持 OpenGL 3.3 的显卡。

## Windows

**双击 `Build-Windows.bat`**，在弹出的对话框里选你的光盘镜像即可（也可以把镜像直接拖到 `Build-Windows.bat` 上）。没装 MSYS2 时脚本会询问并用 winget 自动安装。

完成后：

- 游戏在 `DigimonBattleGC` 文件夹（脚本会自动打开它）；
- 同时生成 `DigimonBattleGC-<版本>-windows-amd64.zip`，方便自己备份/换电脑。它含有从你光盘翻译出的代码，**不要分享**；
- 如果某一步失败，本文件夹会出现 `build-report-<时间>.txt`，记录失败步骤、工具版本和完整构建输出（不含任何游戏数据），求助时附上它即可。

命令行用法：

```powershell
powershell -ExecutionPolicy Bypass -File build-windows.ps1 "D:\Games\Digimon Battle Chronicle (Japan).iso"
```

加 `-Portable` 会把设置、存档、日志放在程序旁边的 `user` 文件夹；`-NoZip` 跳过打包 zip。脚本会自动安装 clang、cmake、ninja、SDL2、python（MSYS2 CLANG64）。完成后运行 `DigimonBattleGC\DigimonBattleGC.exe`。生成的 exe 是静态链接的，复制到别处也能运行，不需要 MSYS2。

压缩输入会先导入到 `cache/<镜像名>.iso`（非 portable：用户数据目录；portable：程序旁 `user/cache`），
构建器从同一份 ISO 读取 `main.dol`；启动器仍选择原始 `.rvz/.gcz/.ciso` 即可，不用手动再转换一次。

## Linux (x86-64)

```sh
# Debian/Ubuntu
sudo apt install python3 cmake ninja-build clang pkg-config libsdl2-dev libgl-dev
# Fedora
sudo dnf install python3 cmake ninja-build clang pkgconf-pkg-config SDL2-devel mesa-libGL-devel

python3 build.py "/path/to/Digimon Battle Chronicle (Japan).iso"
# 不写路径时会在 ~/Downloads、~/Games 等目录查找，或弹出文件选择框；
# 同样会生成 zip 和失败时的 build-report-*.txt
./DigimonBattleGC/DigimonBattleGC
```

## 构建做了什么 / What the builder does

1. 检查光盘：ID `G2DJB2`、版本 0、`main.dol` 指纹与本移植测试用的一致（不一致会拒绝，避免改版/坏档导致的奇怪问题）。
2. 用 `src/tools/lift_dol.py` 把 `main.dol` 的 PowerPC 代码翻译成 C（约 16.6 万个代码块，结果与项目测试用的逐字节一致）。
3. 用 CMake + clang 编译，生成 `DigimonBattleGC` 文件夹（程序、玩法说明 `PLAYING.md`、第三方许可证）。

## 玩 / Playing

启动后在启动器里选择光盘镜像（也可拖进窗口），设置分辨率、全屏、手柄等，点 Play。按键、热键、存档位置见 `PLAYING.md`。存档是标准的 GameCube 记忆卡镜像 `MemoryCardA.raw`，可与 Dolphin 互通。

## 许可 / Licenses

渲染器与 AX 混音来自 gcn-recomp（MIT，`src/third_party/gcn-recomp/LICENSE`），启动器界面用 Dear ImGui（MIT），OpenGL 加载器为 glad 生成代码；SDL2 由你的系统/MSYS2 提供（zlib 许可）。
