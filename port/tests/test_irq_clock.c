/* Standalone tests of the actual IRQ implementation with mocked devices.
 * cc -std=c11 -Wall -Wextra -Werror -Ihal tests/test_irq_clock.c hal/irq.c \
 *    -o /tmp/test_irq_clock && /tmp/test_irq_clock
 */
#define _POSIX_C_SOURCE 200809L
#include "irq.h"
#include "hal.h"
#include "gc_abi.h"
#include "mem.h"
#include "vi.h"
#include "ai.h"
#include "dsp.h"
#include "si.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define setenv(name, value, overwrite) ((void)(overwrite), _putenv_s((name), (value)))
#define unsetenv(name) _putenv_s((name), "")
#endif

#define CPU_HZ 486000000u
/* Mock VI period intentionally differs from the former fixed 60 Hz clock. */
#define FRAME_CYCLES 8108100u
#define EE 0x8000u
#define DEC_VECTOR 0x80000900u

GcCtx gc;
uint32_t gc_spr_misc[1024];
uint8_t gc_ram[GC_MEM1_SIZE];
static uint64_t dsp_cycles, dsp_calls, ai_cycles, ai_calls;
static uint64_t retraces, si_frames, ai_frames, dsp_frames, callbacks;
static uint64_t noted_blocks, noted_polls, noted_frames;
static uint32_t ai_deadline, dsp_deadline, exi_deadline;
static int manifest_enabled;
int gc_runtime_manifest_enabled(void) { return manifest_enabled; }
uint32_t hal_ai_cycles_to_event(uint32_t hz) { assert(hz == CPU_HZ); return ai_deadline; }
uint32_t hal_dsp_cycles_to_event(uint32_t hz) { assert(hz == CPU_HZ); return dsp_deadline; }
uint32_t hal_exi_cycles_to_event(void) { return exi_deadline; }
static void tick_deadline(uint32_t *deadline, uint32_t cycles)
{
    if (*deadline != UINT32_MAX)
        *deadline = cycles >= *deadline ? UINT32_MAX : *deadline - cycles;
}

void hal_dsp_tick_cycles(uint32_t cycles, uint32_t hz)
{
    assert(hz == CPU_HZ);
    tick_deadline(&dsp_deadline, cycles);
    dsp_cycles += cycles;
    ++dsp_calls;
    assert(hal_cpu_cycles() == dsp_cycles);
    assert(hal_timebase_ticks() == dsp_cycles / 12u);
}
/* Main integrates this independently owned AI API into the poll clock. */
void hal_ai_tick_cycles(uint32_t cycles, uint32_t hz)
{
    assert(hz == CPU_HZ);
    tick_deadline(&ai_deadline, cycles);
    ai_cycles += cycles;
    ++ai_calls;
    assert(hal_cpu_cycles() == ai_cycles);
}
void hal_exi_tick_cycles(uint32_t cycles) { tick_deadline(&exi_deadline, cycles); }
static uint32_t vi_phase;
uint32_t hal_vi_cycles_to_event(void) { return FRAME_CYCLES - vi_phase; }
void hal_vi_advance(uint32_t cycles)
{
    assert(cycles <= hal_vi_cycles_to_event());
    vi_phase += cycles;
    if (vi_phase == FRAME_CYCLES) {
        vi_phase = 0;
        ++retraces;
        hal_irq_frame_event();
    }
}
void hal_si_frame_tick(void) { ++si_frames; }
void hal_ai_frame_tick(void) { ++ai_frames; }
void hal_dsp_frame_tick(void) { ++dsp_frames; }
void gc_runtime_manifest_note_block(uint32_t pc) { (void)pc; ++noted_blocks; }
void gc_runtime_manifest_note_event(const char *name)
{
    if (!strcmp(name, "poll")) ++noted_polls;
    else { assert(!strcmp(name, "frame")); ++noted_frames; }
}
uint32_t hal_mem_read32(uint32_t address)
{
    assert(address <= GC_MEM1_SIZE - 4u);
    return (uint32_t)gc_ram[address] << 24 | (uint32_t)gc_ram[address+1] << 16 |
           (uint32_t)gc_ram[address+2] << 8 | gc_ram[address+3];
}
static void frame_callback(void *user)
{
    assert(user == &callbacks);
    ++callbacks;
    assert(hal_cpu_cycles() == callbacks * FRAME_CYCLES);
    assert(hal_timebase_ticks() == callbacks * FRAME_CYCLES / 12u);
}
static void handler(unsigned exc, uint32_t address)
{
    uint32_t pa = 0x3000u + exc * 4u;
    gc_ram[pa] = (uint8_t)(address >> 24);
    gc_ram[pa+1] = (uint8_t)(address >> 16);
    gc_ram[pa+2] = (uint8_t)(address >> 8);
    gc_ram[pa+3] = (uint8_t)address;
}
static void reset(void)
{
    memset(&gc, 0, sizeof(gc));
    memset(gc_ram + 0x3000, 0, 0x40);
    memset(gc_spr_misc, 0, sizeof(gc_spr_misc));
    dsp_cycles = dsp_calls = ai_cycles = ai_calls = 0;
    retraces = si_frames = ai_frames = dsp_frames = callbacks = 0;
    noted_blocks = noted_polls = noted_frames = 0;
    vi_phase = 0;
    ai_deadline = dsp_deadline = exi_deadline = UINT32_MAX;
    manifest_enabled = 1;
    hal_irq_init();
    assert(hal_cpu_cycles() == 0 && hal_timebase_ticks() == 0);
    assert(hal_poll_count() == 0 && hal_frame_count() == 0);
    assert(gc_spr_misc[22] == 0);
    hal_decrementer_write(0x7fffffffu);
    hal_set_frame_callback(frame_callback, &callbacks);
    gc.pc = 0x80004000u;
    gc.icount = UINT64_MAX; /* instruction accounting never drives timers */
}
static uint32_t poll(uint32_t cycles)
{
    hal_set_poll_cycles(cycles);
    return hal_irq_poll();
}
static void advance(uint64_t cycles)
{
    while (cycles) {
        uint32_t chunk = cycles > UINT32_MAX ? UINT32_MAX : (uint32_t)cycles;
        assert(poll(chunk) == 0);
        cycles -= chunk;
    }
}
static void assert_devices(void)
{
    hal_clock_sync_devices(); /* public observation commits sub-event debt */
    assert(dsp_cycles == hal_cpu_cycles());
    assert(dsp_calls >= retraces); /* every event is committed, not every block */
    assert(retraces == hal_frame_count() && si_frames == retraces);
    assert(ai_frames == retraces && dsp_frames == retraces);
    assert(callbacks == retraces && noted_frames == retraces);
    assert(noted_blocks == hal_poll_count() && noted_polls == hal_poll_count());
    assert(ai_cycles == hal_cpu_cycles());
    assert(ai_calls == dsp_calls);
}

static void test_fraction_and_reads(void)
{
    reset();
    hal_decrementer_write(3);
    for (unsigned i = 1; i <= 11; ++i) {
        assert(poll(1) == 0);
        assert(hal_cpu_cycles() == i && hal_timebase_ticks() == 0);
        assert(gc_spr_misc[22] == 3);
        assert(hal_timebase_ticks() == 0); /* reads cannot advance time */
    }
    hal_decrementer_write(2); /* must not reset the accumulated 11 cycles */
    assert(poll(1) == 0);
    assert(hal_timebase_ticks() == 1 && gc_spr_misc[22] == 1);
    assert(poll(11) == 0 && gc_spr_misc[22] == 1);
    assert(poll(1) == 0 && gc_spr_misc[22] == 0);
    assert(poll(11) == 0 && gc_spr_misc[22] == 0);
    handler(8, 0x80005000u);
    gc.msr = EE | 0x123u;
    assert(poll(1) == DEC_VECTOR);
    assert(hal_cpu_cycles() == 36 && hal_timebase_ticks() == 3);
    assert(gc_spr_misc[22] == UINT32_MAX);
    assert(gc.srr0 == 0x80004000u && gc.srr1 == (EE | 0x123u));
    assert(gc.msr == 0x123u && gc.pc == DEC_VECTOR);
    assert(gc.icount == UINT64_MAX);
    assert_devices();
}
static void test_poll_partition_and_boundaries(void)
{
    const uint64_t total = 2ull * FRAME_CYCLES + 17u;
    const uint32_t sizes[] = {1, 7, 256, 100003, UINT32_MAX};
    for (unsigned s = 0; s < sizeof(sizes)/sizeof(sizes[0]); ++s) {
        reset();
        uint64_t remaining = total;
        while (remaining) {
            uint32_t step = remaining < sizes[s] ? (uint32_t)remaining : sizes[s];
            assert(poll(step) == 0);
            remaining -= step;
        }
        assert(hal_cpu_cycles() == total);
        assert(hal_timebase_ticks() == total / 12u);
        assert(gc_spr_misc[22] == 0x7fffffffu - (uint32_t)(total / 12u));
        assert(hal_frame_count() == 2);
        assert_devices();
    }
    reset();
    assert(poll(FRAME_CYCLES - 1u) == 0 && retraces == 0);
    assert(poll(1) == 0 && retraces == 1);
    assert(poll(FRAME_CYCLES * 3u + 11u) == 0 && retraces == 4);
    assert(poll(FRAME_CYCLES - 12u) == 0 && retraces == 4);
    assert(poll(1) == 0 && retraces == 5);
    assert_devices();
    reset();
    assert(poll(UINT32_MAX) == 0);
    assert(hal_timebase_ticks() == (uint64_t)UINT32_MAX / 12u);
    assert(retraces == UINT32_MAX / FRAME_CYCLES);
    assert(poll(9) == 0); /* UINT32_MAX leaves 3 CPU cycles of phase */
    assert(hal_timebase_ticks() == ((uint64_t)UINT32_MAX + 9u) / 12u);
    assert_devices();
}
static void test_pending_ee_and_handler(void)
{
    reset();
    handler(8, 0x80005000u);
    hal_decrementer_write(0);
    assert(poll(12) == 0); /* EE disabled at underflow */
    assert(poll(120) == 0 && gc_spr_misc[22] == UINT32_MAX - 10u);
    hal_decrementer_write(100); /* reprogramming cannot discard old request */
    gc.msr = EE;
    assert(poll(1) == DEC_VECTOR && gc_spr_misc[22] == 100);
    gc.msr = EE;
    assert(poll(1) == 0); /* request was consumed */

    reset();
    gc.msr = EE;
    hal_decrementer_write(1);
    assert(poll(12) == 0 && gc_spr_misc[22] == 0);
    assert(poll(12) == 0 && gc_spr_misc[22] == UINT32_MAX);
    const uint32_t invalid[] = {0, 0x800030ffu, 0x81800000u};
    for (unsigned i = 0; i < sizeof(invalid)/sizeof(invalid[0]); ++i) {
        handler(8, invalid[i]);
        assert(poll(12) == 0);
    }
    handler(8, 0x80003100u);
    assert(poll(1) == DEC_VECTOR);
    assert(gc_spr_misc[22] == UINT32_MAX - 3u);
    gc.msr = EE;
    assert(poll(12) == 0); /* remaining negative is not a new edge */
    assert_devices();
}
static void test_explicit_write_edges(void)
{
    reset();
    handler(8, 0x80005000u);
    gc.msr = EE;
    hal_decrementer_write(0x80000000u);
    assert(gc_spr_misc[22] == 0x80000000u);
    assert(poll(1) == DEC_VECTOR); /* software edge needs no timer tick */
    gc.msr = EE;
    hal_decrementer_write(0xffffffffu); /* negative -> negative */
    assert(poll(1) == 0);
    hal_decrementer_write(10); /* negative -> positive */
    assert(poll(1) == 0);
    /* Also tolerate the coordinator's original shadow-store-then-hook API. */
    gc_spr_misc[22] = 0x80000000u;
    hal_decrementer_write(gc_spr_misc[22]);
    assert(poll(1) == DEC_VECTOR);
    gc.msr = EE;
    hal_decrementer_write(0);
    assert(poll(7) == 0); /* writes above did not change the CPU/12 phase */
    assert(poll(1) == DEC_VECTOR);
    gc.msr = EE;
    assert(poll(12) == 0); /* no artificial once-per-frame repeat */
    assert(hal_frame_count() == 0); /* three independent requests in one frame */
    assert_devices();
}
static void test_negative_wrap_and_timebase_high(void)
{
    reset();
    handler(8, 0x80005000u);
    hal_decrementer_write(0xffffffffu);
    assert(poll(1) == 0); /* software request remains EE-masked */
    gc.msr = EE;
    assert(poll(1) == DEC_VECTOR);
    /* DEC continues through 0x80000000 -> 0x7fffffff without an exception. */
    advance(0x80000000ull * 12u - 2u);
    assert(gc_spr_misc[22] == 0x7fffffffu);
    gc.msr = EE;
    assert(poll(1) == 0);
    advance(0x7ffffffeull * 12u);
    assert(gc_spr_misc[22] == 1);
    assert(poll(11) == 0 && gc_spr_misc[22] == 0);
    assert(poll(12) == DEC_VECTOR && gc_spr_misc[22] == UINT32_MAX);
    assert(hal_timebase_ticks() == 0x100000000ull);
    gc.msr = EE;
    assert(poll(12) == 0);
    assert(hal_timebase_ticks() == 0x100000001ull);
    assert(gc_spr_misc[22] == UINT32_MAX - 1u);
    assert_devices();
}
static void test_init_and_external_irq(void)
{
    reset();
    hal_decrementer_write(UINT32_MAX); /* pending software edge */
    assert(poll(FRAME_CYCLES + 11u) == 0);
    hal_pi_write32(4, PI_INT_VI);
    hal_pi_set_interrupt(PI_INT_VI);
    uint64_t old_callbacks = callbacks;
    hal_irq_init();
    dsp_cycles = dsp_calls = ai_cycles = ai_calls = 0;
    assert(hal_cpu_cycles() == 0 && hal_timebase_ticks() == 0);
    assert(hal_frame_count() == 0 && hal_poll_count() == 0);
    assert(hal_pi_read32(0) == 0x10000u && hal_pi_read32(4) == 0);
    assert(gc_spr_misc[22] == 0);
    gc.msr = EE;
    assert(poll(1) == 0); /* old pending was reset; no old fractional tick */
    assert(hal_timebase_ticks() == 0 && gc_spr_misc[22] == 0);
    hal_decrementer_write(0x7fffffffu);
    hal_irq_init();
    dsp_cycles = dsp_calls = ai_cycles = ai_calls = 0;
    hal_decrementer_write(0x7fffffffu);
    assert(hal_irq_poll() == 0 && hal_cpu_cycles() == 256); /* default restored */
    assert(poll(FRAME_CYCLES) == 0 && callbacks == old_callbacks); /* cb cleared */
    handler(4, 0x80005000u);
    hal_pi_write32(4, PI_INT_VI);
    hal_pi_set_interrupt(PI_INT_VI);
    gc.pc = 0x80006000u;
    gc.msr = EE | 3u;
    assert(poll(1) == GC_VEC_EXTERNAL);
    assert(gc.srr0 == 0x80006000u && gc.srr1 == (EE | 3u));
    assert(gc.msr == 3u && hal_irq_pending() == PI_INT_VI);
    hal_pi_clear_interrupt(PI_INT_VI);
    gc.msr = EE;
    assert(poll(0) == 0); /* zero poll budget clamps to one CPU cycle */
    assert(hal_cpu_cycles() == 256ull + FRAME_CYCLES + 2u);
}
static void test_explicit_executed_cycles(void)
{
    reset();
    assert(hal_irq_poll_cycles(0) == 0);
    assert(hal_cpu_cycles() == 0 && dsp_calls == 0 && ai_calls == 0);
    hal_clock_advance(3);
    assert(hal_cpu_cycles() == 3 && hal_timebase_ticks() == 0);
    handler(4, 0x80005000u);
    hal_pi_write32(4, PI_INT_VI);
    hal_pi_set_interrupt(PI_INT_VI);
    gc.msr = EE;
    assert(hal_irq_poll_cycles(0) == GC_VEC_EXTERNAL);
    assert(hal_cpu_cycles() == 3); /* diverted block executed no instructions */
    hal_pi_clear_interrupt(PI_INT_VI);
    gc.pc = gc.srr0; gc.msr = EE;
    assert(hal_irq_poll_cycles(0) == 0);
    assert(hal_cpu_cycles() == 3); /* resumed entry is not charged again */
    hal_clock_advance(9);
    assert(hal_cpu_cycles() == 12 && hal_timebase_ticks() == 1);
    /* Device time is batched to the next device event or MMIO access; committing
     * it here must deliver exactly the executed cycles, in one more call. */
    assert(dsp_cycles == 3 && ai_cycles == 3 && dsp_calls == 1 && ai_calls == 1);
    hal_clock_sync_devices();
    assert(dsp_cycles == 12 && ai_cycles == 12 && dsp_calls == 2 && ai_calls == 2);
    assert(noted_polls == 3 && hal_poll_count() == 3);
    reset();
    hal_clock_advance(FRAME_CYCLES - 1u);
    assert(retraces == 0);
    hal_clock_advance(1);
    assert(retraces == 1 && callbacks == 1 && hal_poll_count() == 0);
}

static void idle_setup(void)
{
    reset();
    gc.pc = 0x802c7ae4u; gc.r[13] = 0x803d6120u;
    memset(gc_ram + 0x3cfd58, 0, 4);
    gc.msr = EE;
}
static void test_scheduler_idle_deadlines(void)
{
    idle_setup();
    gc.r[0] = 123; gc.cr = 0x24681357u;
    hal_scheduler_idle();
    assert(hal_cpu_cycles() == FRAME_CYCLES && gc.r[0] == 123 && gc.cr == 0x24681357u);
    assert(dsp_cycles == FRAME_CYCLES && ai_cycles == FRAME_CYCLES);
    assert(dsp_calls == 1 && ai_calls == 1); /* formerly 31,672 idle iterations */
    idle_setup();
    ai_deadline = 30000; dsp_deadline = 22000; exi_deadline = 40000;
    hal_scheduler_idle(); assert(hal_cpu_cycles() == 22000 && dsp_deadline == UINT32_MAX);
    hal_scheduler_idle(); assert(hal_cpu_cycles() == 30000 && ai_deadline == UINT32_MAX);
    hal_scheduler_idle(); assert(hal_cpu_cycles() == 40000 && exi_deadline == UINT32_MAX);
    assert(dsp_calls == 3 && ai_calls == 3);
    idle_setup();
    exi_deadline = 1;
    hal_scheduler_idle(); assert(hal_cpu_cycles() == 1);
    reset(); manifest_enabled = 0;
    assert(hal_irq_poll_cycles(0) == 0);
    assert(hal_poll_count() == 1 && noted_polls == 0 && noted_blocks == 0);
    idle_setup(); gc.msr = 0; hal_scheduler_idle(); assert(hal_cpu_cycles() == 0);
    idle_setup(); gc.pc += 4; hal_scheduler_idle(); assert(hal_cpu_cycles() == 0);
    idle_setup(); gc.r[13] += 4; hal_scheduler_idle(); assert(hal_cpu_cycles() == 0);
    idle_setup(); gc_ram[0x3cfd5b] = 1; hal_scheduler_idle(); assert(hal_cpu_cycles() == 0);
    idle_setup(); handler(4, 0x80005000u);
    hal_pi_write32(4, PI_INT_VI); hal_pi_set_interrupt(PI_INT_VI);
    hal_scheduler_idle(); assert(hal_cpu_cycles() == 0);
    idle_setup(); handler(8, 0x80005000u); hal_decrementer_write(0x80000000u);
    hal_scheduler_idle(); assert(hal_cpu_cycles() == 0);
    idle_setup(); hal_clock_advance(FRAME_CYCLES - 5u);
    hal_scheduler_idle();
    assert(hal_cpu_cycles() == FRAME_CYCLES && callbacks == 1);
    idle_setup(); handler(8, 0x80005000u); hal_decrementer_write(1);
    hal_clock_advance(7); hal_scheduler_idle();
    assert(hal_cpu_cycles() == 24 && gc_spr_misc[22] == UINT32_MAX);
    assert(hal_irq_poll_cycles(0) == DEC_VECTOR && hal_cpu_cycles() == 24);
}

int main(void)
{
    /* Keep optional read-only diagnostic environment hooks out of this fixture. */
    unsetenv("GC_FP_TRACE");
    unsetenv("GC_AX_DEPOP_TRACE");
    unsetenv("GC_RAM_WATCH");
    unsetenv("GC_VI_TRACE");
    test_fraction_and_reads();
    test_poll_partition_and_boundaries();
    test_pending_ee_and_handler();
    test_explicit_write_edges();
    test_negative_wrap_and_timebase_high();
    test_init_and_external_irq();
    test_explicit_executed_cycles();
    test_scheduler_idle_deadlines();
    puts("IRQ clock tests passed");
    return 0;
}
