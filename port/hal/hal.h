/* hal/hal.h — HAL 内部共享定义（块间可见，非对提升器契约）。
 * 硬件块基址出处：Dolphin Source/Core/Core/HW/Memmap.cpp RegisterMMIO 调用点
 *   CP=0x0C000000 PE=0x0C001000 VI=0x0C002000 PI=0x0C003000 MI=0x0C004000
 *   DSP(含 ARAM DMA)=0x0C005000 DI=0x0C006000 SI=0x0C006400 EXI=0x0C006800
 *   AI=0x0C006C00  WGPIPE(GX FIFO 写口)=0x0C008000
 * https://raw.githubusercontent.com/dolphin-emu/dolphin/master/Source/Core/Core/HW/Memmap.cpp
 */
#ifndef HAL_HAL_H
#define HAL_HAL_H

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

/* ---- MMIO 基址（phys = ea & 0x1FFFFFFF） ---- */
#define HW_CP_BASE   0x0C000000u
#define HW_PE_BASE   0x0C001000u
#define HW_VI_BASE   0x0C002000u
#define HW_PI_BASE   0x0C003000u
#define HW_MI_BASE   0x0C004000u
#define HW_DSP_BASE  0x0C005000u
#define HW_DI_BASE   0x0C006000u
#define HW_SI_BASE   0x0C006400u
#define HW_EXI_BASE  0x0C006800u
#define HW_AI_BASE   0x0C006C00u
#define HW_WGPIPE    0x0C008000u
#define HW_MMIO_TOP  0x0D000000u

/* ---- PI 中断线（Dolphin ProcessorInterface.h INT_CAUSE_*） ---- */
#define PI_INT_PI         0x0001u  /* GP runtime error */
#define PI_INT_RSW        0x0002u  /* Reset Switch */
#define PI_INT_DI         0x0004u  /* DVD */
#define PI_INT_SI         0x0008u
#define PI_INT_EXI        0x0010u
#define PI_INT_AI         0x0020u
#define PI_INT_DSP        0x0040u
#define PI_INT_MEMORY     0x0080u
#define PI_INT_VI         0x0100u
#define PI_INT_PE_TOKEN   0x0200u
#define PI_INT_PE_FINISH  0x0400u
#define PI_INT_CP         0x0800u
#define PI_INT_DEBUG      0x1000u
#define PI_INT_HSP        0x2000u

/* ---- 异常向量 ---- */
#define GC_VEC_EXTERNAL 0x80000500u   /* PPC 外部输入中断向量=0x500（0x300 是 DSI）*/

/* PI 汇聚线 API（irq.c 实现，各硬件块调用） */
void hal_pi_set_interrupt(uint32_t bits);
void hal_pi_clear_interrupt(uint32_t bits);
/* PI GX FIFO 环形缓冲（PI_FIFO_BASE/END/WPTR，gx.c 与 mmio.c 共用） */
extern uint32_t hal_pi_fifo_base, hal_pi_fifo_end, hal_pi_fifo_wptr;
void hal_pi_fifo_reset(void);
/* 帧事件（irq.c：frames++ + VI retrace + AI 推进 + host 回调） */
void hal_irq_frame_event(void);

/* 各块 MMIO 派发（mmio.c 调用；reg 为块内偏移，BE 语义已由访问器拆好） */
uint16_t hal_cp_read16 (uint32_t reg);
void     hal_cp_write16(uint32_t reg, uint16_t v);
uint16_t hal_pe_read16 (uint32_t reg);
void     hal_pe_write16(uint32_t reg, uint16_t v);
uint16_t hal_vi_read16 (uint32_t reg);
void     hal_vi_write16(uint32_t reg, uint16_t v);
uint32_t hal_pi_read32 (uint32_t reg);
void     hal_pi_write32(uint32_t reg, uint32_t v);
uint16_t hal_mi_read16 (uint32_t reg);
void     hal_mi_write16(uint32_t reg, uint16_t v);
uint16_t hal_dsp_read16(uint32_t reg);
void     hal_dsp_write16(uint32_t reg, uint16_t v);
uint32_t hal_di_read32 (uint32_t reg);
void     hal_di_write32(uint32_t reg, uint32_t v);
uint32_t hal_si_read32 (uint32_t reg);
void     hal_si_write32(uint32_t reg, uint32_t v);
uint32_t hal_exi_read32(uint32_t reg);
void     hal_exi_write32(uint32_t reg, uint32_t v);
uint16_t hal_ai_read16 (uint32_t reg);
void     hal_ai_write16(uint32_t reg, uint16_t v);
/* WGPIPE 32 字节 burst（gx.c：入 PI FIFO 环 + GX 解析器） */
void hal_gx_burst32(const uint8_t *bytes32);

/* 复位 */
void hal_irq_init(void);
void hal_vi_init(void);
void hal_gx_init(void);
void hal_si_init(void);
void hal_exi_init(void);
void hal_dsp_init(void);
void hal_ar_init(void);
void hal_ai_init(void);
void hal_di_init(void);

#endif /* HAL_HAL_H */
