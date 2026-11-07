/* hal/ar.h — ARAM 缓冲与 DMA 引擎（16MB）。
 * GC 的 ARAM 只能经 DSP 块的 ARAM DMA 寄存器访问（无 CPU 直映射）。
 * ARAM 镜像语义：传出数据每 64MB 镜像（Dolphin DSP.cpp Do_ARAM_DMA 实测注释）。 */
#ifndef HAL_AR_H
#define HAL_AR_H

#include <stdint.h>
#include <stddef.h>

#define GC_ARAM_SIZE_ 0x01000000u
extern uint8_t gc_aram[GC_ARAM_SIZE_];

void hal_ar_init(void);
/* 方向 dir：0=MRAM→ARAM，1=ARAM→MRAM（CNT bit31） */
void hal_ar_dma(uint32_t mram_addr, uint32_t aram_addr, uint32_t len, int dir);
uint8_t  hal_ar_read8(uint32_t addr);
void     hal_ar_write8(uint32_t addr, uint8_t v);

#endif /* HAL_AR_H */
