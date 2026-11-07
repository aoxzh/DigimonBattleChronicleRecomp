/* hal/mmio.h — MMIO 总派发（phys 0x0C000000-0x0CFFFFFF）+ 慢路径访问。
 * 分发表出处：Dolphin Source/Core/Core/HW/Memmap.cpp（各块 RegisterMMIO 基址，
 * 见 hal.h 文件头）。VI/AI/DSP/CP/PE 为 16 位寄存器（偶偏移），PI/DI/SI/EXI 为
 * 32 位寄存器（4 对齐）。访问器按 BE 拆装：32 位读写 = 高低两个 16 位寄存器。
 */
#ifndef HAL_MMIO_H
#define HAL_MMIO_H

#include <stdbool.h>
#include <stdint.h>

/* gc_abi.h 契约实现：宽窄访问派发到块 */
uint8_t  hal_mmio_read8 (uint32_t phys);
uint16_t hal_mmio_read16(uint32_t phys);
uint32_t hal_mmio_read32(uint32_t phys);
void     hal_mmio_write8 (uint32_t phys, uint8_t  v);
void     hal_mmio_write16(uint32_t phys, uint16_t v);
void     hal_mmio_write32(uint32_t phys, uint32_t v);

/* Synchronous locked-cache DMA, adapted from MIT gcn-recomp runtime/cpu.cpp
 * at 3fad2f2deea7e563755819cf6f44da8743c61c55 (see docs/analysis/lc-dma.md).
 * Pass raw SPR922/923 values. DMAL bit 1 triggers; bit 4 selects MEM1 -> LC.
 * Lines = ((dmau & 31) << 2) | ((dmal >> 2) & 3); zero means 128 x 32B.
 * MEM1 accepts physical/0x80000000/0xC0000000 aliases; LC accepts offsets or
 * 0xE0000000 addresses within its 16KB window. A complete span must fit both.
 * Returns true after copying; false (inactive/invalid) leaves both unchanged.
 * Caller owns SPR shadows: after an SPR923 write, clear its trigger (~2u).
 * Transfers finish immediately, so HID2 DMAQL reads should remain zero. */
bool hal_lc_dma(uint32_t dmau, uint32_t dmal);

/* 慢路径（phys >= 0x01800000）：LC + MMIO + 空洞（读 0 写忽略） */
uint8_t  hal_slow_read8 (uint32_t phys);
uint16_t hal_slow_read16(uint32_t phys);
uint32_t hal_slow_read32(uint32_t phys);
uint64_t hal_slow_read64(uint32_t phys);
void     hal_slow_write8 (uint32_t phys, uint8_t  v);
void     hal_slow_write16(uint32_t phys, uint16_t v);
void     hal_slow_write32(uint32_t phys, uint32_t v);
void     hal_slow_write64(uint32_t phys, uint64_t v);

/* 调试日志开关（0=静默；非 0 打印未知寄存器访问） */
extern int hal_mmio_trace;

#endif /* HAL_MMIO_H */
