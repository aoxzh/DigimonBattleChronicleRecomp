/* hal/ai.c — cycle-driven Audio Interface register model.
 * Protocol reference (GPL reference only; no implementation transplanted):
 * dolphin-emu/dolphin 1822e3859f061c6f81076c94eb4ba7e6e0a3706c,
 * Source/Core/Core/HW/AudioInterface.{h,cpp}: AICR, Init, RegisterMMIO,
 * IncreaseSampleCount, UpdateInterrupts. Nominal rates are used here, not
 * Dolphin's slightly faster GameCube hardware divisors. Like that reference,
 * AIINTVLD is stored but does not inhibit the interrupt timing comparator.
 */
#include "ai.h"
#include "hal.h"
#include "irq.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define AI_PSTAT    0x01u
#define AI_AFR      0x02u
#define AI_INTMASK  0x04u
#define AI_INT      0x08u
#define AI_INTVLD   0x10u
#define AI_SREFRESH 0x20u
#define AI_AIDFR    0x40u
#define AI_WRITABLE (AI_PSTAT | AI_AFR | AI_INTMASK | AI_INTVLD | AI_AIDFR)

static uint16_t aicr_lo, ai_vol;
static uint32_t ai_counter, ai_irpt_timing;
/* Fractional samples, in units of 1/clock_hz. Products fit uint64_t even
 * for the full uint32_t cycle/hz domain. */
static uint64_t sample_fraction;
static uint32_t clock_hz;
static int cycle_clock_active;
static HalAudioMode audio_mode;
static int audio_release_applied;

static HalAudioMode read_audio_mode(void)
{
    const char *mode = getenv("GC_AUDIO_MODE");
    if (mode && strcmp(mode, "release-after-first-frame") == 0)
        return HAL_AUDIO_MODE_RELEASE_AFTER_FIRST_FRAME;
    return HAL_AUDIO_MODE_NORMAL;
}

HalAudioMode hal_ai_audio_mode(void)
{
    hal_clock_sync_devices();
    return audio_mode;
}
const char *hal_ai_audio_mode_name(void)
{
    hal_clock_sync_devices();
    return audio_mode == HAL_AUDIO_MODE_RELEASE_AFTER_FIRST_FRAME
        ? "release-after-first-frame" : "normal";
}

static void update_interrupt(void)
{
    if ((aicr_lo & (AI_INT | AI_INTMASK)) == (AI_INT | AI_INTMASK))
        hal_pi_set_interrupt(PI_INT_AI);
    else
        hal_pi_clear_interrupt(PI_INT_AI);
}

static void increase_sample_count(uint64_t amount)
{
    if (!amount)
        return;
    /* Match the samples visited in (old, new], including uint32_t wrap.
     * Equality at the old counter needs a complete revolution to match again. */
    uint32_t distance = ai_irpt_timing - ai_counter;
    if (amount >= (UINT64_C(1) << 32) || (distance && amount >= distance))
        aicr_lo |= AI_INT;
    ai_counter += (uint32_t)amount;
    update_interrupt();
}

void hal_ai_init(void)
{
    hal_clock_sync_devices();
    hal_clock_device_changed();
    audio_mode = read_audio_mode();
    aicr_lo = AI_AFR | AI_AIDFR; /* reset 0x42: stream48k, DMA32k, stopped */
    ai_vol = 0;
    ai_irpt_timing = ai_counter = 0;
    sample_fraction = 0;
    clock_hz = 0;
    cycle_clock_active = 0;
    audio_release_applied = 0;
    update_interrupt();
}

static void tick_clock(uint32_t cycles, uint32_t hz)
{
    if (!(aicr_lo & AI_PSTAT))
        return;
    /* Preserve fractional sample phase on a clock-source change. A fraction
     * not representable at the new denominator is rounded down by <1/hz
     * sample. With a fixed CPU frequency all accumulation is exact. */
    if (clock_hz && clock_hz != hz)
        sample_fraction = sample_fraction * hz / clock_hz;
    clock_hz = hz;
    uint32_t rate = (aicr_lo & AI_AFR) ? 48000u : 32000u;
    uint64_t total = sample_fraction + (uint64_t)cycles * rate;
    /* Most translated blocks cannot finish a sample. Preserve the exact
     * numerator without paying for quotient/remainder until one is due. */
    if (total < hz) {
        sample_fraction = total;
        return;
    }
    sample_fraction = total % hz;
    increase_sample_count(total / hz);
}

uint32_t hal_ai_cycles_to_event(uint32_t cpu_hz)
{
    hal_clock_sync_devices();
    if (!cpu_hz || !(aicr_lo & AI_PSTAT) || (aicr_lo & AI_INT))
        return UINT32_MAX;
    uint64_t samples = (uint32_t)(ai_irpt_timing - ai_counter);
    if (!samples) samples = UINT64_C(1) << 32;
    uint64_t phase = sample_fraction;
    if (clock_hz && clock_hz != cpu_hz)
        phase = phase * cpu_hz / clock_hz;
    const uint32_t rate = (aicr_lo & AI_AFR) ? 48000u : 32000u;
    const uint64_t needed = samples * cpu_hz - phase;
    const uint64_t cycles = needed / rate + (needed % rate != 0);
    return cycles > UINT32_MAX ? UINT32_MAX : (uint32_t)cycles;
}

void hal_ai_tick_cycles(uint32_t cycles, uint32_t cpu_hz)
{
    hal_clock_sync_devices();
    hal_clock_device_changed();
    if (!cpu_hz)
        return; /* no clock supplied; do not disable the legacy fallback */
    cycle_clock_active = 1;
    tick_clock(cycles, cpu_hz);
}

void hal_ai_frame_tick(void)
{
    hal_clock_sync_devices();
    hal_clock_device_changed();
    /* Legacy callers without a cycle clock retain nominal 60Hz progression.
     * This path never advances the counter after a valid cycle-clock call. */
    if (!cycle_clock_active)
        tick_clock(1, 60);

    /* Diagnostic host policy remains a frame event even with a cycle clock. */
    if (audio_mode == HAL_AUDIO_MODE_RELEASE_AFTER_FIRST_FRAME &&
        !audio_release_applied && hal_frame_count() >= 1) {
        audio_release_applied = 1;
        /* Diagnostic release also discards the pending latch, keeping the
         * stored cause and PI line consistent after the forced release. */
        aicr_lo &= (uint16_t)~(AI_PSTAT | AI_INT);
        sample_fraction = 0;
        update_interrupt();
        fprintf(stderr, "[audio] release-after-first-frame at frame %llu\n",
                (unsigned long long)hal_frame_count());
    }
}

uint32_t hal_ai_samples(void)
{
    hal_clock_sync_devices();
    return ai_counter;
}
uint32_t hal_ai_dma_sample_rate(void)
{
    hal_clock_sync_devices();
    return (aicr_lo & AI_AIDFR) ? 32000u : 48000u;
}

uint16_t hal_ai_read16(uint32_t reg)
{
    hal_clock_sync_devices();
    switch (reg) {
    case 0x00: return 0; /* reserved control high half */
    case 0x02: return aicr_lo;
    case 0x04: return ai_vol;
    case 0x08: return (uint16_t)(ai_counter >> 16);
    case 0x0A: return (uint16_t)ai_counter;
    case 0x0C: return (uint16_t)(ai_irpt_timing >> 16);
    case 0x0E: return (uint16_t)ai_irpt_timing;
    default:   return 0;
    }
}

void hal_ai_write16(uint32_t reg, uint16_t v)
{
    hal_clock_sync_devices();
    hal_clock_device_changed();
    switch (reg) {
    case 0x00: break; /* reserved */
    case 0x02:
        if (((aicr_lo ^ v) & AI_PSTAT) || (v & AI_SREFRESH))
            sample_fraction = 0;
        if (v & AI_SREFRESH)
            ai_counter = 0;
        /* AIINT is a latch: writing zero preserves it, writing one clears it.
         * SREFRESH is a command and always reads back zero. */
        aicr_lo = (uint16_t)((v & AI_WRITABLE) |
                            (aicr_lo & AI_INT & (uint16_t)~v));
        update_interrupt();
        break;
    case 0x04: ai_vol = v; break;
    /* Dolphin permits counter writes; both big-endian halves are supported.
     * Writing counter/timing does not itself constitute a comparator match. */
    case 0x08:
        ai_counter = (ai_counter & 0xFFFFu) | ((uint32_t)v << 16);
        sample_fraction = 0;
        break;
    case 0x0A:
        ai_counter = (ai_counter & 0xFFFF0000u) | v;
        sample_fraction = 0;
        break;
    case 0x0C:
        ai_irpt_timing = (ai_irpt_timing & 0xFFFFu) | ((uint32_t)v << 16);
        break;
    case 0x0E:
        ai_irpt_timing = (ai_irpt_timing & 0xFFFF0000u) | v;
        break;
    default: break;
    }
}
