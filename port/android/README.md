# Digimon Battle Chronicle — Android（骁龙 8 Gen 2 手机实测）

SDL2 + 静态重编译的 GameCube 游戏核心。Android 端现在使用原生 Material 3 启动器，
游戏画面通过 OpenGL ES 3.2 输出，音频走 AX 混音器，支持折叠屏、蓝牙手柄和触屏按键。

APK 内含从你自己的光盘翻译出的代码，**只装在自己的设备上，不要分发**。

## 构建

前提：`packaging/build.py` 和 `tools/lift_dol.py` 已经把 `main.dol` 生成到
`port/recomp/generated/`。

- JDK 17+（本机 JDK 25 可用）
- Android SDK：platform 36、build-tools、platform-tools
- NDK 29.0.14206865
- CMake 3.31.6
- Gradle 9.6+、AGP 9.5.1

`local.properties`：

```properties
sdk.dir=C\:\\Users\\<you>\\dev\\toolchains\\android-sdk
```

构建并安装：

```sh
cd port/android
gradle assembleRelease
adb install -r app/build/outputs/apk/release/app-release.apk
```

release 变体使用本地 debug 签名，native 代码使用 `-O2`。保留 `run-as` 权限用于读取实机日志和自动回放。

## 启动器

`MainActivity.java` 是原生 Material 3 界面（动态取色、edge-to-edge、折叠屏自适应），
不再打开桌面版 ImGui 启动器。启动器负责：

- 通过系统文件选择器选择 `.iso` / `.gcm`
- 或“扫描 Download 目录”（SAF 目录授权，不需要“所有文件访问”权限）
- 把镜像复制到应用专属目录，并记住位置；下次启动自动校验、直接显示“已记住”
- 如果应用目录中的副本被删掉，会用上次的 SAF 授权自动重新导入
- 画面、音频、触屏、存档设置，直接写应用目录的 `settings.ini`

选择镜像时如果系统选择器看不到 `.iso`，是因为 DocumentsUI 对未知 MIME 的
文件支持不好。可以在选择器右上角搜索文件名，或直接用“扫描 Download 目录”。

## 游戏端

`GameActivity.java` 只负责把启动器传来的绝对路径交给 SDL：

```
--disc /data/user/0/jp.digimonbc.port/files/disc.iso
```

`main_gx.c` 在 Android 上不再打开 ImGui 启动器；镜像无效时直接报错。
`adapter.cpp`、`overlay.cpp` 和桌面版共用，因此触屏按键、暂停菜单、手柄、
存档和音频逻辑不需要维护两份。

`overlay.cpp` 的触屏按键按屏幕密度缩放，并在折叠/展开时重新布局。触屏使用中，
游戏震动会交给手机振动器；连接手柄时自动隐藏（可在启动器改为常显/隐藏）。

## 帧率与音频

VI 根据游戏写入的扫描寄存器推进。这个日版游戏实际使用 NTSC 隔行模式：
每场 525 个半行、8,108,100 个 CPU 周期，即 `60000/1001 ≈ 59.94` 场/秒，
DI0/DI1 分别在奇偶场边界触发。`pace_guest()` 按 486 MHz guest 时钟限制速度；
Android 默认 `vsync=0`，避免把游戏速度绑定到手机的 60/120 Hz 显示模式。

手机性能不够时（例如骁龙 8 Gen 2 手机在 2× 内部分辨率下约 50 fps），guest 时钟会跑不满，
AX 音频按 guest 帧生产，实时播放的队列会被抽空，听感就是小爆音/断续。
`host_audio.c` 的自动缓冲会因此增长：

```text
[host-audio] underrun #111: buffer 110ms -> 185ms
```

Android 自动缓冲从 220 ms 起步，稳定播放后最低 200 ms，连续 underrun 时最高 350 ms。
队列持续空 20 ms 后暂停播放并重新填充，日志单独记录 `rebuffers`；这只能恢复短时
卡顿后的余量。持续掉速需要解决 CPU/GPU 吞吐，扩大缓冲无法补足缺少的音频。
以保持实际 59.94 场/秒为目标：

- **内部分辨率：1×**（NGC 原生 640×480）
- 音频延迟：Auto；如果仍有爆音，选 200 ms 或 350 ms
- 2×/3×/4× 只建议在确认能维持 60 fps 的设备上使用

## 自动实机回放

把桌面测试使用的输入 JSON 放入应用私有 `files/input-script.json`，启动时便会自动
推进菜单和对战，无需手动按键。事件可用 `channel: 0..3` 指定手柄，以
`{"frame":1,"channel":1,"connected":1}` 接入第二个手柄；`connected:0` 断开。
四手柄回放见 `analysis/rendered-boot/android-fourpad-input.json`。
测试结束删除输入文件即可恢复触屏和手柄输入。

```sh
adb -s DEVICE push ../analysis/rendered-boot/audio-paced-compact-input.json /data/local/tmp/digimon-input.json
adb -s DEVICE shell run-as jp.digimonbc.port cp /data/local/tmp/digimon-input.json files/input-script.json
```

连续画面检查可另放 `files/replay.properties`，先创建私有输出目录：

```properties
GC_DUMP_DIR=/data/user/0/jp.digimonbc.port/files/frames
GC_DUMP_EVERY=1
GCN_DUMP_RANGE=6998-7010
```

上述选项只在输入回放存在时加载。`GC_AUDIO_TRACE=1` 可记录有界音频队列时间序列。
PNG 写盘会影响当时的运行速度，音频/帧率验收应使用不带抓图选项的独立回放。
测试后删除这两个私有配置文件。

## 实机验证与剩余问题

2026-11-07 的实机原生连续帧已确认角色闪暗修复，并自动完成双人和四人对战。
根因是 PPC 标量/配对浮点指令共用寄存器时的表示不一致，导致游戏求逆后的法线矩阵
出现 Inf/极小值；不是角色调色板或正常受击效果。现在使用独立 PS0/PS1 数值通道，
中断也保存恢复两路状态。六帧角色区域亮度波动范围从 28.79/9.37 降到 0.47/0.52。

2026-11-08：四人对战掉速（34–47 场/秒）和随之而来的爆音/断续已修复。GCN_STALLS
计时显示 guest 线程约 45% 时间在等 GX 变换线程：游戏每帧做 8 次 64×64 纹理拷贝
（HUD 头像）并回写内存，旧实现每次都把批次切开、等上一整帧的顶点变换和 GPU 回读。
现按 upstream gcn-recomp 的做法，拷贝留在本帧批次里，渲染线程交回字节，由 guest
线程在两个显示帧后写入内存。骁龙 8 Gen 2 手机实测（4P 回放、含进战斗加载）：

| 设置 | 改前 | 改后 |
| --- | --- | --- |
| 1× 内部分辨率，4P 对战 | ~34 场/秒，14 次欠载，77 次重缓冲 | ~58.5 场/秒，0 / 0 |
| 4× 内部分辨率，4P 对战 | ~30–35 场/秒，14 次欠载，42 次重缓冲 | ~58–60 场/秒，0 / 0 |

4× 下画面、HUD 头像正常。注意：`files/input-script.json` 存在时游戏完全使用脚本
输入，触屏/手柄和游戏震动都会被忽略——测试后务必删除。触屏按键现在至少保持
60 ms，快速点按不会在两次采样之间丢失；日志里 `[touch] haptic ...` 与
`[rumble] ...` 行用于排查震动。详见 `analysis/android-cadence-2026-11-07.md`。

## 文件

```
app/src/main/java/jp/digimonbc/port/
    MainActivity.java       Material 3 启动器、镜像导入/记忆
    GameActivity.java       SDL 游戏 Activity
    DiscImage.java          与 settings_check_disc 一致的镜像校验
    SettingsStore.java      读写 settings.ini
    DigimonApp.java         动态取色
app/src/main/res/           主题、布局、图标
app/src/main/cpp/CMakeLists.txt
    libmain.so = main_gx + hal + recomp + gcn-recomp(GLES) + AX + ImGui + SDL2
```

日志：`files/logs/session.log`（上一次是 `session.prev.log`）。
存档：`files/MemoryCardA.raw`。
