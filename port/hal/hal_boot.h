/* hal/hal_boot.h — 引导准备 + 帧驱动。
 * 初始寄存器出处：Dolphin Boot_BS2Emu.cpp EmulatedBS2_GC（NTSC）：
 *   r1 = 0x81566550（栈指针）、r2 = 0x81465CC0（SDA2）、r13 = 0x81465320（SDA）、
 *   msr = 0x0000F000（EE/PR/ME/IR…按 BS2 后默认：本模型只维护 EE 位）
 *   https://raw.githubusercontent.com/dolphin-emu/dolphin/master/Source/Core/Core/Boot/Boot_BS2Emu.cpp
 */
#ifndef HAL_HAL_BOOT_H
#define HAL_HAL_BOOT_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* 一站式引导：清 RAM → 解析/加载 DOL（先清 bss 再拷段）→ 写 BS2 内存状态
 * （含 disc header 0x100B、bi2→0x81700000 + 指针 0x800000F4）→ 复位全部硬件块
 * → 打开 disc.iso 给 DI → 置 GcCtx 初始寄存器（r1/r2/r13/msr/pc=entry）。
 * dol_path 与 iso_path 至少一个非 NULL：dol_path 为 NULL 时直接从盘内
 * （头部 0x420 指向的 DOL）读取；iso_path 为 NULL 时无盘运行。 */
bool hal_boot_prepare(const char *dol_path, const char *iso_path);

/* 读出盘内 main.dol（malloc 缓冲，调用者 free）；失败返回 NULL。 */
uint8_t *hal_boot_read_disc_dol(const char *iso_path, size_t *out_n);

/* 驱动 max_blocks 个"块边界"的时间量并服务帧事件（无 CPU 执行——用于
 * 生成代码缺位时的硬件侧推进与测试）。返回发生的帧事件次数。 */
uint32_t hal_run_frame(uint32_t max_blocks);

/* 引导后 CPU 入口（= DOL entry；自检对账用） */
uint32_t hal_boot_entry(void);

#endif /* HAL_HAL_BOOT_H */
