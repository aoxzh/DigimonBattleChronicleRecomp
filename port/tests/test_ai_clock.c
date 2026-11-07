/* Standalone tests of actual ai.c with only PI/frame hooks mocked.
 * From port/:
 * cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
 *    -fno-omit-frame-pointer -Ihal tests/test_ai_clock.c hal/ai.c \
 *    -o /tmp/test_ai_clock && /tmp/test_ai_clock
 * Also run with -DGC_UPSTREAM_AUDIO to verify identical register semantics.
 */
#define _POSIX_C_SOURCE 200809L
#include "ai.h"
#include "hal.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define setenv(name, value, overwrite) ((void)(overwrite), _putenv_s((name), (value)))
#define unsetenv(name) _putenv_s((name), "")
#endif

#define PSTAT 0x01u
#define AFR 0x02u
#define MASK 0x04u
#define STATUS 0x08u
#define VALID 0x10u
#define RESET 0x20u
#define DMA32 0x40u
#define CPU_HZ 486000000u

static uint32_t pi;
static uint64_t frame;
void hal_pi_set_interrupt(uint32_t bits) { pi |= bits; }
void hal_pi_clear_interrupt(uint32_t bits) { pi &= ~bits; }
uint64_t hal_frame_count(void) { return frame; }

static void reset(void)
{
    assert(unsetenv("GC_AUDIO_MODE") == 0);
    pi = PI_INT_AI | PI_INT_DSP;
    frame = 0;
    hal_ai_init();
    assert(pi == PI_INT_DSP);
    assert(hal_ai_read16(2) == 0x42);
    assert(hal_ai_samples() == 0);
}

static uint32_t read32(uint32_t reg)
{
    return ((uint32_t)hal_ai_read16(reg) << 16) | hal_ai_read16(reg + 2);
}

static void write32(uint32_t reg, uint32_t value)
{
    hal_ai_write16(reg, (uint16_t)(value >> 16));
    hal_ai_write16(reg + 2, (uint16_t)value);
    assert(read32(reg) == value);
}

static void test_stopped_reads_and_registers(void)
{
    reset();
    for (unsigned i = 0; i < 10000; ++i) {
        assert(read32(8) == 0);
        assert(hal_ai_samples() == 0);
    }
    hal_ai_tick_cycles(CPU_HZ, CPU_HZ);
    hal_ai_frame_tick();
    assert(read32(8) == 0);
    write32(8, 0x12345678);
    write32(12, 0x89ABCDEF);
    for (unsigned i = 0; i < 100; ++i) {
        assert(read32(8) == 0x12345678);
        assert(read32(12) == 0x89ABCDEF);
        assert(hal_ai_samples() == read32(8));
    }
    hal_ai_write16(4, 0xABCD);
    assert(hal_ai_read16(4) == 0xABCD);
    hal_ai_write16(0, 0xFFFF);
    assert(hal_ai_read16(0) == 0);
    hal_ai_write16(0x10, 0xFFFF);
    assert(hal_ai_read16(0x10) == 0);
    hal_ai_write16(2, 0xFF80);
    assert(hal_ai_read16(2) == 0);
}

static void test_rates_and_dma_polarity(void)
{
    for (unsigned stream48 = 0; stream48 < 2; ++stream48) {
        for (unsigned dma32 = 0; dma32 < 2; ++dma32) {
            reset();
            hal_ai_write16(2, PSTAT | (stream48 ? AFR : 0) |
                                     (dma32 ? DMA32 : 0));
            assert(hal_ai_dma_sample_rate() == (dma32 ? 32000u : 48000u));
            /* Stereo 16-bit AID bitrate: rate * 4 bytes per sample frame. */
            assert(hal_ai_dma_sample_rate() * 4u ==
                   (dma32 ? 128000u : 192000u));
            hal_ai_tick_cycles(CPU_HZ, CPU_HZ);
            assert(read32(8) == (stream48 ? 48000u : 32000u));
            hal_ai_frame_tick();
            assert(hal_ai_samples() == (stream48 ? 48000u : 32000u));
        }
    }
}

static void test_fractions_and_clock_selection(void)
{
    for (unsigned stream48 = 0; stream48 < 2; ++stream48) {
        reset();
        hal_ai_write16(2, PSTAT | (stream48 ? AFR : 0));
        uint32_t rate = stream48 ? 48000u : 32000u;
        uint64_t cycles = 0;
        for (unsigned i = 0; i < 100000; ++i) {
            uint32_t step = i % 997u + 1u;
            cycles += step;
            hal_ai_tick_cycles(step, CPU_HZ);
            assert(hal_ai_samples() == cycles * rate / CPU_HZ);
            if (!(i % 1000)) {
                uint32_t before = read32(8);
                hal_ai_tick_cycles(0, CPU_HZ);
                hal_ai_frame_tick();
                assert(read32(8) == before);
            }
        }
    }
    reset();
    hal_ai_write16(2, PSTAT | AFR);
    hal_ai_tick_cycles(10124, CPU_HZ);
    assert(read32(8) == 0);
    hal_ai_tick_cycles(1, CPU_HZ);
    assert(read32(8) == 1);
    hal_ai_tick_cycles(5062, CPU_HZ);
    hal_ai_write16(2, PSTAT); /* carry partial sample through AFR change */
    hal_ai_tick_cycles(7595, CPU_HZ);
    assert(read32(8) == 2);

    reset();
    hal_ai_tick_cycles(0, CPU_HZ); /* select cycle clock while stopped */
    hal_ai_write16(2, PSTAT | AFR);
    hal_ai_frame_tick();
    assert(read32(8) == 0);
    reset();
    hal_ai_write16(2, PSTAT);
    hal_ai_tick_cycles(UINT32_MAX, 0); /* invalid hz leaves fallback intact */
    for (unsigned i = 0; i < 60; ++i)
        hal_ai_frame_tick();
    assert(read32(8) == 32000);
    hal_ai_tick_cycles(0, CPU_HZ);
    hal_ai_frame_tick();
    assert(read32(8) == 32000);

    reset();
    hal_ai_write16(2, PSTAT | AFR);
    hal_ai_tick_cycles(1, 96000); /* half sample */
    hal_ai_tick_cycles(0, 192000); /* same phase with a new denominator */
    hal_ai_tick_cycles(2, 192000);
    assert(read32(8) == 1);
    /* Exercise the largest public arguments without integer overflow. */
    reset();
    hal_ai_write16(2, PSTAT | AFR);
    hal_ai_tick_cycles(UINT32_MAX, UINT32_MAX);
    assert(read32(8) == 48000);
}

static void test_reset_and_pause(void)
{
    reset();
    hal_ai_write16(2, PSTAT | AFR);
    hal_ai_tick_cycles(10124, CPU_HZ);
    hal_ai_write16(2, AFR); /* stop discards partial sample */
    hal_ai_tick_cycles(CPU_HZ, CPU_HZ);
    hal_ai_write16(2, PSTAT | AFR);
    hal_ai_tick_cycles(1, CPU_HZ);
    assert(read32(8) == 0);
    write32(12, 0x10203040);
    write32(8, 0xABCDEF12);
    hal_ai_tick_cycles(10123, CPU_HZ);
    hal_ai_write16(2, PSTAT | AFR | DMA32 | RESET);
    assert(hal_ai_read16(2) == (PSTAT | AFR | DMA32));
    assert(read32(8) == 0);
    assert(read32(12) == 0x10203040);
    hal_ai_tick_cycles(1, CPU_HZ);
    assert(read32(8) == 0);
    hal_ai_tick_cycles(10124, CPU_HZ);
    assert(read32(8) == 1);
    hal_ai_write16(2, AFR | RESET);
    hal_ai_tick_cycles(CPU_HZ, CPU_HZ);
    assert(read32(8) == 0);
}

static void test_interrupts_and_wrap(void)
{
    reset();
    write32(12, 3);
    hal_ai_write16(2, PSTAT | AFR);
    hal_ai_tick_cycles(2, 48000);
    assert(!(hal_ai_read16(2) & STATUS));
    hal_ai_tick_cycles(1, 48000);
    assert(hal_ai_read16(2) & STATUS);
    assert(!(pi & PI_INT_AI)); /* latch even while masked */
    hal_ai_write16(2, PSTAT | AFR | MASK);
    assert(pi & PI_INT_AI);
    hal_ai_write16(2, PSTAT | AFR);
    assert(!(pi & PI_INT_AI));
    assert(hal_ai_read16(2) & STATUS);
    hal_ai_write16(2, AFR | MASK); /* stop does not acknowledge IRQ */
    assert(pi & PI_INT_AI);
    hal_ai_write16(2, AFR | MASK | RESET); /* reset also does not ack */
    assert(pi & PI_INT_AI);
    assert(read32(8) == 0);
    hal_ai_write16(2, PSTAT | AFR | MASK | STATUS);
    assert(!(pi & PI_INT_AI));
    hal_ai_tick_cycles(3, 48000);
    assert(pi & PI_INT_AI);
    hal_ai_write16(2, PSTAT | AFR | MASK | STATUS);
    hal_ai_tick_cycles(1, 48000);
    assert(!(pi & PI_INT_AI)); /* passed threshold must not retrigger */
    write32(12, read32(8)); /* writing equality is not a new sample match */
    hal_ai_tick_cycles(0, 48000);
    assert(!(pi & PI_INT_AI));
    hal_ai_tick_cycles(1, 48000);
    assert(!(pi & PI_INT_AI));

    reset();
    write32(8, 0xFFFEFFFF);
    write32(12, 0xFFFF0002);
    hal_ai_write16(2, PSTAT | AFR | MASK | VALID);
    hal_ai_tick_cycles(2, 48000);
    assert(read32(8) == 0xFFFF0001);
    assert(!(pi & PI_INT_AI));
    hal_ai_tick_cycles(1, 48000);
    assert(pi & PI_INT_AI); /* upstream stores VALID without gating match */
    hal_ai_write16(2, PSTAT | AFR | MASK | STATUS);
    write32(8, 0xFFFFFFFE);
    write32(12, 1);
    hal_ai_tick_cycles(2, 48000);
    assert(read32(8) == 0);
    assert(!(pi & PI_INT_AI));
    hal_ai_tick_cycles(1, 48000);
    assert(read32(8) == 1);
    assert(pi & PI_INT_AI);
    hal_ai_write16(2, PSTAT | AFR | MASK | STATUS);
    write32(8, UINT32_MAX);
    write32(12, 0); /* zero is a valid match at wrap */
    hal_ai_tick_cycles(1, 48000);
    assert(read32(8) == 0);
    assert(pi & PI_INT_AI);

    reset();
    write32(12, 100);
    hal_ai_write16(2, PSTAT | AFR | MASK);
    hal_ai_tick_cycles(120, 48000); /* a coarse tick can cross the threshold */
    assert(pi & PI_INT_AI);
    reset();
    hal_ai_write16(2, PSTAT | AFR | MASK);
    hal_ai_tick_cycles(UINT32_MAX, 1); /* includes multiple counter wraps */
    assert(read32(8) == (uint32_t)((uint64_t)UINT32_MAX * 48000u));
    assert(pi & PI_INT_AI);
}

static void test_release_after_frame(void)
{
    reset();
    assert(setenv("GC_AUDIO_MODE", "release-after-first-frame", 1) == 0);
    hal_ai_init();
    assert(hal_ai_audio_mode() == HAL_AUDIO_MODE_RELEASE_AFTER_FIRST_FRAME);
    assert(!strcmp(hal_ai_audio_mode_name(), "release-after-first-frame"));
    write32(12, 1);
    hal_ai_write16(2, PSTAT | AFR | MASK | DMA32);
    hal_ai_tick_cycles(480, 48000);
    hal_ai_frame_tick(); /* before first frame: still running */
    assert(hal_ai_read16(2) & PSTAT);
    frame = 1;
    hal_ai_frame_tick();
    assert(!(hal_ai_read16(2) & (PSTAT | STATUS)));
    assert(!(pi & PI_INT_AI));
    hal_ai_tick_cycles(CPU_HZ, CPU_HZ);
    assert(read32(8) == 480);
    assert(hal_ai_dma_sample_rate() == 32000);
    hal_ai_write16(2, PSTAT | AFR); /* policy is one-shot */
    frame = 2;
    hal_ai_frame_tick();
    assert(hal_ai_read16(2) & PSTAT);
    hal_ai_tick_cycles(1, 48000);
    assert(read32(8) == 481);
    reset();
    assert(hal_ai_audio_mode() == HAL_AUDIO_MODE_NORMAL);
    assert(!strcmp(hal_ai_audio_mode_name(), "normal"));
}

int main(void)
{
    test_stopped_reads_and_registers();
    test_rates_and_dma_polarity();
    test_fractions_and_clock_selection();
    test_reset_and_pause();
    test_interrupts_and_wrap();
    test_release_after_frame();
    puts("AI clock tests passed");
    return 0;
}
