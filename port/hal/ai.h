/* hal/ai.h — Audio Interface sample clock and register model.
 * Protocol reference: dolphin-emu/dolphin
 * 1822e3859f061c6f81076c94eb4ba7e6e0a3706c,
 * Source/Core/Core/HW/AudioInterface.{h,cpp} (reference only).
 * 32-bit sample counter at 0x08/0x0A; interrupt timing at 0x0C/0x0E.
 * AICR low half at 0x02: PSTAT bit0; AFR bit1 (1=48k,0=32k);
 * AIINTMSK bit2; AIINT bit3 (W1C); AIINTVLD bit4 (stored only, as in
 * the pinned Dolphin comparator); SREFRESH/SCRESET bit5 (self-clearing);
 * AIDFR bit6 (0=48k,1=32k). Reset control is 0x42.
 * Nominal sample rates; no DVD decoding, hardware divisor correction, or
 * AID DMA completion here. DSP HAL owns the latter.
 */
#ifndef HAL_AI_H
#define HAL_AI_H

#include <stdint.h>

typedef enum {
    HAL_AUDIO_MODE_NORMAL = 0,
    HAL_AUDIO_MODE_RELEASE_AFTER_FIRST_FRAME = 1,
} HalAudioMode;

void hal_ai_init(void);
/* Invoke before IRQ dispatch with elapsed emulated CPU cycles. The first call
 * with nonzero cpu_hz selects cycle clocking until init, even if cycles=0 or
 * PSTAT=0. Zero cpu_hz is ignored. Fixed-frequency fractional accumulation is
 * exact; frequency changes rescale phase with <1/cpu_hz sample truncation.
 * PSTAT transitions/reset discard partial samples; AFR changes preserve phase.
 * Polling cadence determines when a threshold IRQ is delivered to the CPU. */
void hal_ai_tick_cycles(uint32_t cycles, uint32_t cpu_hz);
/* CPU cycles to the next comparator latch, including fractional phase and
 * counter wrap; UINT32_MAX if stopped, already latched or farther away.
 * Public queries first commit pending shared-clock work, without advancing
 * CPU time; the query itself does not select a different clock source. */
uint32_t hal_ai_cycles_to_event(uint32_t cpu_hz);
/* Legacy nominal 60Hz clock until cycle clocking is selected; always applies
 * the release-after-first-frame diagnostic host policy. */
void hal_ai_frame_tick(void);
uint32_t hal_ai_samples(void); /* same wrapping 32-bit counter as MMIO */
uint32_t hal_ai_dma_sample_rate(void); /* AICR bit6: 0=48k, 1=32k */
uint16_t hal_ai_read16(uint32_t reg); /* observational big-endian halfwords */
void hal_ai_write16(uint32_t reg, uint16_t value);
HalAudioMode hal_ai_audio_mode(void);
const char *hal_ai_audio_mode_name(void);

#endif /* HAL_AI_H */
