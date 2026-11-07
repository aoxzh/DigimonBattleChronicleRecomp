/* Real VI + IRQ clock integration, with other hardware mocked.
 * cc -std=c11 -Wall -Wextra -Werror -Ihal tests/test_vi_timing.c \
 *    hal/vi.c hal/irq.c -o /tmp/test_vi_timing && /tmp/test_vi_timing
 * NTSC fixture comes from this DOL's 80368f4c timing record AND runtime
 * main_gx display=60 register dump: 00=0f06 02=0001 06=01ad 0c=0003
 * 0e=0018 10=0002 12=0019 30=1107 32=01ae 34=1001 36=0001 6c=0.
 */
#include "hal.h"
#include "vi.h"
#include "irq.h"
#include "mem.h"
#include "gc_abi.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

GcCtx gc;
uint32_t gc_spr_misc[1024];
uint8_t gc_ram[GC_MEM1_SIZE];
static uint64_t audio_cycles, callbacks;
static uint32_t expected_period;
static int ack_interrupts;

int gc_runtime_manifest_enabled(void) { return 0; }
uint32_t hal_ai_cycles_to_event(uint32_t hz) { (void)hz; return UINT32_MAX; }
uint32_t hal_dsp_cycles_to_event(uint32_t hz) { (void)hz; return UINT32_MAX; }
uint32_t hal_exi_cycles_to_event(void) { return UINT32_MAX; }
void hal_dsp_tick_cycles(uint32_t cycles, uint32_t hz) { (void)cycles; assert(hz == 486000000); }
void hal_ai_tick_cycles(uint32_t cycles, uint32_t hz) { audio_cycles += cycles; assert(hz == 486000000); }
void hal_exi_tick_cycles(uint32_t cycles) { (void)cycles; }
void hal_si_frame_tick(void) {}
void hal_ai_frame_tick(void) {}
void hal_dsp_frame_tick(void) {}
void gc_runtime_manifest_note_block(uint32_t pc) { (void)pc; }
void gc_runtime_manifest_note_event(const char *name) { (void)name; }
uint32_t hal_mem_read32(uint32_t p)
{
    return (uint32_t)gc_ram[p] << 24 | (uint32_t)gc_ram[p+1] << 16 |
           (uint32_t)gc_ram[p+2] << 8 | gc_ram[p+3];
}
static void put(unsigned reg, unsigned value) { hal_vi_write16(reg, (uint16_t)value); }
static unsigned status(unsigned i) { return (hal_vi_reg(0x30 + i * 4) >> 15) & 1; }
static void ack(unsigned i) { put(0x30 + 4 * i, hal_vi_reg(0x30 + 4 * i) & 0x7fff); }
static void compare(unsigned i, unsigned v, unsigned h, int mask)
{
    put(0x30 + i * 4, v | (mask ? 0x1000 : 0));
    put(0x32 + i * 4, h);
}
static void callback(void *user)
{
    (void)user;
    ++callbacks;
    assert(audio_cycles == hal_cpu_cycles());
    if (expected_period) {
        assert(hal_cpu_cycles() == callbacks * expected_period);
        assert(hal_timebase_ticks() == hal_cpu_cycles() / 12);
    }
    if (ack_interrupts) {
        assert(status(0) == (callbacks & 1));
        assert(status(1) == !(callbacks & 1));
        ack(0); ack(1);
    }
}
static void reset(void)
{
    memset(&gc, 0, sizeof(gc));
    hal_irq_init(); hal_vi_init();
    hal_decrementer_write(0x7fffffff);
    hal_set_frame_callback(callback, NULL);
    audio_cycles = callbacks = 0;
    expected_period = 0; ack_interrupts = 0;
}
static void ntsc(void)
{
    reset();
    put(0, 0x0f06); put(6, 429);
    put(0x0c, 3); put(0x0e, 24); put(0x10, 2); put(0x12, 25);
    put(0x6c, 0);
    compare(0, 263, 430, 1); compare(1, 1, 1, 1);
    put(2, 1);
}
static void test_actual_dol_scan(void)
{
    ntsc(); expected_period = 8108100;
    assert(hal_vi_read16(0x6c) == 0); /* CLOCK is not DTV */
    assert(hal_vi_read16(0x6e) == 3);
    assert(hal_vi_cycles_to_field() == expected_period);
    hal_clock_advance(35);
    assert(hal_vi_read16(0x2c) == 1 && hal_vi_read16(0x2e) == 1);
    hal_clock_advance(1);
    assert(hal_vi_read16(0x2e) == 2);
    hal_clock_advance(429 * 36 - 36);
    assert(hal_vi_read16(0x2c) == 1 && hal_vi_read16(0x2e) == 430);
    hal_clock_advance(429 * 36);
    assert(hal_vi_read16(0x2c) == 2 && hal_vi_read16(0x2e) == 1);
    hal_clock_advance(expected_period - 858 * 36 - 1);
    assert(!status(0) && !status(1) && callbacks == 0);
    hal_clock_advance(1);
    assert(status(0) && !status(1) && callbacks == 1);
    assert(hal_vi_read16(0x2c) == 263 && hal_vi_read16(0x2e) == 430);
    assert(hal_pi_read32(0) & PI_INT_VI);
    ack(0);
    assert(!(hal_pi_read32(0) & PI_INT_VI));
    hal_frame_tick();
    assert(!status(0) && status(1) && callbacks == 2);
    assert(hal_vi_read16(0x2c) == 1 && hal_vi_read16(0x2e) == 1);
    assert(hal_cpu_cycles() == 16216200);
    ack(1);
    /* Reads don't advance beam/time and a cleared compare doesn't re-fire at
     * its current coordinate until the raster reaches it again. */
    for (unsigned i = 0; i < 8; ++i) assert(hal_vi_read16(0x2e) == 1);
    hal_clock_advance(1); assert(!status(1));
}
static void test_compare_mask_and_exact_horizontal(void)
{
    ntsc();
    compare(0, 3, 17, 1); compare(1, 3, 18, 1);
    compare(2, 2, 13, 0); compare(3, 2, 13, 1);
    const uint32_t at = (858 + 12) * 36;
    hal_clock_advance(at - 1);
    assert(!status(2) && !status(3));
    assert(hal_vi_cycles_to_event() == 1);
    hal_clock_advance(1);
    assert(status(2) && status(3) && !status(0) && !status(1));
    ack(3); assert(!(hal_pi_read32(0) & PI_INT_VI));
    put(0x38, hal_vi_reg(0x38) | 0x1000); /* unmask pending => immediate PI */
    assert(hal_pi_read32(0) & PI_INT_VI);
    ack(2); assert(!(hal_pi_read32(0) & PI_INT_VI));
    hal_clock_advance((2 * 858 + 16) * 36 - at);
    assert(status(0) && !status(1)); ack(0);
    hal_clock_advance(35); assert(!status(1));
    hal_clock_advance(1); assert(status(1));
    assert(callbacks == 0); /* display compares need not be retraces */
}
static void test_partition_and_long_run(void)
{
    const uint32_t parts[] = {1, 257, 99991, UINT32_MAX};
    for (unsigned i = 0; i < sizeof(parts)/sizeof(parts[0]); ++i) {
        ntsc(); expected_period = 8108100; ack_interrupts = 1;
        uint32_t left = expected_period * 3 + 37;
        while (left) { uint32_t n = left < parts[i] ? left : parts[i]; hal_clock_advance(n); left -= n; }
        assert(callbacks == 3 && hal_vi_read16(0x2c) == 263 && hal_vi_read16(0x2e) == 431);
    }
    ntsc(); expected_period = 8108100; ack_interrupts = 1;
    /* 60000 fields are exactly 1001 seconds, no floating-point accumulation. */
    for (unsigned i = 0; i < 120; ++i) hal_clock_advance(expected_period * 500u);
    assert(callbacks == 60000 && hal_cpu_cycles() == 486000000ull * 1001);
    assert(hal_vi_read16(0x2c) == 1 && hal_vi_read16(0x2e) == 1);
}
static void test_modes_and_disable(void)
{
    ntsc();
    /* This DOL's next table entry is 240p/double strike, 526 half-lines. */
    put(0x0c, 4); put(0x0e, 24); put(0x10, 4); put(0x12, 24); put(2, 5);
    compare(0, 264, 1, 1);
    expected_period = 8123544; ack_interrupts = 1;
    hal_clock_advance(expected_period * 2); assert(callbacks == 2);
    /* 480p: doubled half-lines at 54 MHz gives the original field duration. */
    ntsc(); put(0, (480 << 4) | 12); put(0x0c, 6); put(0x0e, 48);
    put(0x10, 6); put(0x12, 48); put(0x6c, 1); put(2, 5);
    compare(0, 526, 1, 1);
    assert(hal_vi_read16(0x6c) == 1);
    expected_period = 8108100; ack_interrupts = 1;
    hal_clock_advance(expected_period * 2); assert(callbacks == 2);
    /* PAL timing is register-derived too (625 half-lines at HLW432). */
    reset(); put(0, (288 << 4) | 5); put(6, 432);
    put(0x0c, 2); put(0x0e, 32); put(0x10, 3); put(0x12, 31); put(2, 0x101);
    expected_period = 9720000;
    hal_clock_advance(expected_period * 2); assert(callbacks == 2);
    /* FMT and NIN do not independently retime an already programmed raster. */
    put(2, 5); assert(hal_vi_cycles_to_field() == expected_period);
    put(2, 0); assert(hal_vi_cycles_to_event() == UINT32_MAX);
    uint64_t before = hal_cpu_cycles(); hal_frame_tick(); assert(hal_cpu_cycles() == before);
    hal_clock_advance(1000); assert(callbacks == 2 && hal_vi_read16(0x2e) == 1);
    put(2, 1); expected_period = 0; hal_frame_tick(); assert(callbacks == 3);
    put(0x30, 0x9001); assert(hal_pi_read32(0) & PI_INT_VI);
    put(2, 3); assert(hal_vi_reg(2) == 1 && !hal_vi_reg(0x30));
    assert(!(hal_pi_read32(0) & PI_INT_VI));
    assert(hal_vi_read16(0x2c) == 1 && hal_vi_read16(0x2e) == 1);
    put(6, 0); assert(hal_vi_cycles_to_event() == UINT32_MAX);
}
static void test_idle_stops_at_compare(void)
{
    ntsc(); compare(2, 1, 5, 1); /* 144 cycles, inside first scanline */
    gc.pc = 0x802c7ae4; gc.r[13] = 0x803d6120; gc.msr = 0x8000;
    hal_scheduler_idle();
    assert(hal_cpu_cycles() == 144 && status(2) && callbacks == 0);
}
int main(void)
{
    test_actual_dol_scan(); test_compare_mask_and_exact_horizontal();
    test_partition_and_long_run(); test_modes_and_disable(); test_idle_stops_at_compare();
    puts("VI timing tests passed (DOL NTSC, 240p, 480p, PAL, compare/field/clock)");
    return 0;
}
