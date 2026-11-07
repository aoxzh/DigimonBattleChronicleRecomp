/* hal/exi.h — Expansion Interface：IPL（RTC/SRAM）与槽 A 内存卡。
 * 出处：Dolphin Source/Core/Core/HW/EXI/EXI_Channel.{h,cpp}、EXI_DeviceIPL.cpp、
 *   EXI_DeviceMemoryCard.cpp
 *   https://raw.githubusercontent.com/dolphin-emu/dolphin/master/Source/Core/Core/HW/EXI/EXI_Channel.h
 *   每通道 stride 0x14：EXI_STATUS=0x00（EXIINTMASK0 EXIINT1 TCINTMASK2 TCINT3
 *   CLK4-6 CHIP_SELECT7-9 EXTINTMASK10 EXTINT11 EXT12 ROMDIS13）、
 *   DMA_ADDR=0x04、DMA_LEN=0x08、DMA_CTRL=0x0C（TSTART0 DMA1 RW2-3 TLEN4-5）、IMM=0x10。
 *   传输在写 DMA_CTRL.TSTART 时按字节流交给选中设备（ch0 dev0 = 槽 A 卡，
 *   ch0 dev1 = IPL；其他位置无设备），即时完成并置 TCINT。
 *   IPL：命令字 >> 6 为设备地址，0x800000 = RTC（GC_EPOCH=0x386D4380 起秒数），
 *   0x800004..0x800043 = SRAM（0x40 字节，校验与 flashID 校验均有效）。
 *   内存卡：EXI ID = 容量 Mbit（2 MiB 镜像 = 0x10 / 251 块），状态常带 UNLOCKED，
 *   命令 0x00/0x52/0x81/0x83/0x85/0x89/0xF1/0xF2/0xF4；擦除/编程在约 0.1 ms
 *   客体时间后完成并（若 0x81 打开）置 EXIINT。镜像为 Dolphin 兼容 raw。
 */
#ifndef HAL_EXI_H
#define HAL_EXI_H

#include <stdint.h>

void hal_exi_init(void);
/* 槽 A 内存卡镜像路径（hal_boot_prepare 之前设置；NULL = 槽空，保持无卡行为）。
 * 文件不存在时在内存中格式化一张 251 块卡，首次写回时创建文件。 */
void hal_exi_set_memcard(const char *path);
/* 由 hal_clock_advance 推进：完成挂起的卡命令，空闲约 1 秒后写回修改。 */
void hal_exi_tick_cycles(uint32_t cycles);
/* Next pending card completion/flush deadline; UINT32_MAX when none exists. */
uint32_t hal_exi_cycles_to_event(void);
/* 立即写回已修改的卡镜像（退出时调用）。返回 0 成功或无需写回。 */
int hal_exi_flush(void);

#endif /* HAL_EXI_H */
