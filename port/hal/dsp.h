/* hal/dsp.h — DSP 接口（邮箱/控制桩，含 ARAM DMA 寄存器）。
 * 出处：Dolphin Source/Core/Core/HW/DSP.{h,cpp}
 *   https://raw.githubusercontent.com/dolphin-emu/dolphin/master/Source/Core/Core/HW/DSP.cpp
 *   偏移（块内）：MAIL_TO_DSP_HI=0x00 LO=0x02、MAIL_FROM_DSP_HI=0x04 LO=0x06、
 *   DSP_CONTROL=0x0A（16 位：DSPReset0 DSPAssertInt1 DSPHalt2 AID3/4 ARAM5/6
 *   DSP7/8 DMAState9 DSPInitCode10 DSPInit11）；
 *   AR_INFO=0x12、AR_MODE=0x16、AR_REFRESH=0x1A、AR_DMA_MMADDR=0x20/0x22、
 *   AR_DMA_ARADDR=0x24/0x26、AR_DMA_CNT=0x28/0x2A（写 CNT_L 触发 DMA；
 *   CNT bit31 = 方向：1=ARAM→MRAM，0=MRAM→ARAM）。
 *   DSP_CONTROL_MASK=0x0C07；ARAM_SIZE=0x01000000（16MB）。
 * 启动 spin 点语义：SDK __OSInitDSP 写 DSPReset=1 后轮询等 0——写路径里
 * 立即清零该位即可让等待循环退出；DSPInit/DSPInitCode 位按写值透传。
 */
#ifndef HAL_DSP_H
#define HAL_DSP_H

#include <stdint.h>

void hal_dsp_init(void);
uint16_t hal_dsp_csr(void);          /* 观测：DSPCR 影子 */
void hal_dsp_frame_tick(void);
/* Optional CPU poll hook. Call before checking pending IRQs; cycles use cpu_hz.
 * First call selects cycle timing and disables frame-based DMA delivery.
 * Each call completes at most one DMA block, leaving an IRQ dispatch opportunity.
 * No-op in builds without GC_UPSTREAM_AUDIO. */
void hal_dsp_tick_cycles(uint32_t cycles, uint32_t cpu_hz);
/* Remaining CPU cycles to the current DMA block's completion, rounded up;
 * UINT32_MAX if inactive/no upstream cycle device. Includes sub-sample phase.
 * The scheduler uses this bound before advancing an idle guest. */
uint32_t hal_dsp_cycles_to_event(uint32_t cpu_hz);
void hal_dsp_sync_arm(void);       /* legacy sync stub; no-op with GC_UPSTREAM_AUDIO */
/* GC_UPSTREAM_AUDIO selects the pinned partial AX adapter, exact upload hash
 * gating, queued mail IRQs and borrowed-RAM PCM DMA. Default stub is preserved.
 * Cycle-clock DMA delivery uses AI's selected sample rate; only callers without
 * a CPU clock use the legacy nominal 60Hz frame fallback. */

#endif /* HAL_DSP_H */
