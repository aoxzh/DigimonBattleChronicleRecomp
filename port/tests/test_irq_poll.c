/* IRQ hot/cold equivalence, probe ordering, and an isolated zero-cycle benchmark.
 * Run through test_irq_poll.py; device clocks are mocked, IRQ/TB/DEC are real. */
#define _POSIX_C_SOURCE 200809L
#include "irq.h"
#include "hal.h"
#include "gc_abi.h"
#include "mem.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define EE 0x8000u
#define DEC_VECTOR 0x80000900u
GcCtx gc;
uint32_t gc_spr_misc[1024];
uint8_t gc_ram[GC_MEM1_SIZE];
static uint64_t device_cycles, device_calls, manifest_queries, manifest_blocks;
static uint64_t total_polls, injected, local_polls, digest = 1469598103934665603ull;
static int mutate, emit_manifest;
extern uint64_t hal_irq_diag_ee_off, hal_irq_diag_injected, hal_irq_diag_pending;
extern uint64_t hal_irq_diag_notready, hal_irq_diag_line;
extern uint64_t hal_irq_diag_msr_hist[16], hal_irq_diag_msr_hi8[16], hal_irq_diag_src[16];

static int enabled(const char *name)
{
    const char *s = getenv(name);
    return s && *s && *s != '0';
}
static void put32(uint32_t p, uint32_t v)
{
    gc_ram[p] = (uint8_t)(v >> 24); gc_ram[p+1] = (uint8_t)(v >> 16);
    gc_ram[p+2] = (uint8_t)(v >> 8); gc_ram[p+3] = (uint8_t)v;
}
uint32_t hal_mem_read32(uint32_t p)
{
    assert(p <= GC_MEM1_SIZE - 4);
    return (uint32_t)gc_ram[p] << 24 | (uint32_t)gc_ram[p+1] << 16 |
           (uint32_t)gc_ram[p+2] << 8 | gc_ram[p+3];
}
int gc_runtime_manifest_enabled(void)
{ ++manifest_queries; return enabled("GC_RUNTIME_MANIFEST"); }
void gc_runtime_manifest_note_block(uint32_t pc)
{
    ++manifest_blocks;
    if (emit_manifest)
        fprintf(stderr, "[manifest-test] pc=%08x cycles=%llu\n", pc,
                (unsigned long long)hal_cpu_cycles());
}
void gc_runtime_manifest_note_event(const char *s) { (void)s; }
uint32_t hal_vi_cycles_to_event(void) { return UINT32_MAX; }
uint32_t hal_ai_cycles_to_event(uint32_t hz) { (void)hz; return UINT32_MAX; }
/* A test that arms `mutate` models a DSP event due within the next cycle, so the
 * batched clock commits device time (and runs the mutation) inside the poll. */
uint32_t hal_dsp_cycles_to_event(uint32_t hz) { (void)hz; return mutate ? 1u : UINT32_MAX; }
uint32_t hal_exi_cycles_to_event(void) { return UINT32_MAX; }
void hal_dsp_tick_cycles(uint32_t n, uint32_t hz)
{
    assert(hz == 486000000u && n);
    device_cycles += n; ++device_calls;
    assert(hal_cpu_cycles() == device_cycles);
    if (mutate == 1) {
        fprintf(stderr, "[tick-test] cycles=%llu\n", (unsigned long long)hal_cpu_cycles());
        gc.pc = 0x802c32f4u; put32(0x1000, 0x12345678u);
    } else if (mutate == 2) {
        put32(0xa08, 11); gc.pc = gc.lr;
    } else if (mutate == 3) {
        hal_pi_set_interrupt(PI_INT_DSP);
    }
    mutate = 0;
}
void hal_ai_tick_cycles(uint32_t n, uint32_t hz) { (void)n; (void)hz; }
void hal_exi_tick_cycles(uint32_t n) { (void)n; }
void hal_vi_advance(uint32_t n) { (void)n; }
void hal_si_frame_tick(void) {}
void hal_ai_frame_tick(void) {}
void hal_dsp_frame_tick(void) {}
static void reset(void)
{
    memset(&gc, 0, sizeof(gc)); memset(gc_ram + 0x3000, 0, 0x40);
    device_cycles = device_calls = local_polls = 0; mutate = 0;
    hal_irq_init(); hal_decrementer_write(0x7fffffffu);
    gc.pc = 0x80004000u; gc.msr = EE;
}
static void mix(uint64_t v) { digest = (digest ^ v) * 1099511628211ull; }
static uint32_t poll(uint32_t cycles)
{
    uint32_t v = hal_irq_poll_cycles(cycles);
    ++total_polls; ++local_polls; injected += v != 0;
    assert(hal_poll_count() == local_polls);
    /* Device time is batched until the next device event or MMIO access (none of
     * these stubs schedules one); commit it so every poll still checks that the
     * devices received exactly the executed cycles. */
    hal_clock_sync_devices();
    assert(hal_cpu_cycles() == device_cycles);
    mix(v); mix(hal_cpu_cycles()); mix(hal_timebase_ticks()); mix(gc_spr_misc[22]);
    mix(gc.pc); mix(gc.msr); mix(gc.srr0); mix(gc.srr1); mix(hal_irq_pending());
    return v;
}
static void resume_guest(void) { gc.pc = 0x80004000u; gc.msr = EE; }
static void functional(void)
{
    reset();
    uint64_t queries = manifest_queries;
    for (unsigned i = 0; i < 100; ++i) assert(poll(0) == 0);
    assert(device_calls == 0 && hal_cpu_cycles() == 0);
    if (!enabled("GC_RUNTIME_MANIFEST")) assert(manifest_queries == queries);
    gc.msr = 0; assert(poll(13) == 0); assert(hal_timebase_ticks() == 1);
    resume_guest();
    hal_pi_set_interrupt(PI_INT_DSP); assert(poll(0) == 0); /* masked */
    hal_pi_write32(4, PI_INT_DSP); assert(poll(0) == 0); /* no handler */
    put32(0x3010, 0x802bd894u); assert(poll(0) == 0); /* SDK template */
    put32(0x3010, 0x80005000u); assert(poll(0) == GC_VEC_EXTERNAL);
    assert(gc.srr0 == 0x80004000u && gc.srr1 == EE && !(gc.msr & EE));
    assert(poll(0) == 0); /* latched external while EE is clear */
    resume_guest(); hal_pi_write32(4, 0); assert(poll(0) == 0);
    hal_pi_write32(4, PI_INT_DSP); assert(poll(0) == GC_VEC_EXTERNAL);
    hal_pi_clear_interrupt(PI_INT_DSP); resume_guest();
    mutate = 3; assert(poll(11) == GC_VEC_EXTERNAL); /* clock creates IRQ */

    reset(); put32(0x3010, 0x80005000u); put32(0x3020, 0x80006000u);
    hal_pi_set_interrupt(PI_INT_DSP); hal_pi_write32(4, PI_INT_DSP);
    hal_decrementer_write(0); gc.msr = 0;
    assert(poll(12) == 0); assert(poll(0) == 0);
    resume_guest(); assert(poll(0) == DEC_VECTOR); /* DEC has priority */
    resume_guest(); assert(poll(0) == GC_VEC_EXTERNAL);
    hal_pi_clear_interrupt(PI_INT_DSP); resume_guest(); assert(poll(0) == 0);

    reset(); put32(0x3010, 0x80005000u);
    hal_pi_set_interrupt(PI_INT_DSP); hal_pi_write32(4, PI_INT_DSP);
    hal_decrementer_write(0);
    assert(poll(12) == GC_VEC_EXTERNAL); /* pending DEC handler not ready */
    hal_pi_clear_interrupt(PI_INT_DSP); resume_guest(); assert(poll(0) == 0);
    put32(0x3020, 0x80006000u); assert(poll(0) == DEC_VECTOR);
    resume_guest(); assert(poll(0) == 0);

    uint64_t lo = 0, hi = 0, sources = 0;
    for (unsigned i = 0; i < 16; ++i) {
        lo += hal_irq_diag_msr_hist[i]; hi += hal_irq_diag_msr_hi8[i];
        sources += hal_irq_diag_src[i];
    }
    if (enabled("GC_IRQ_DIAG")) {
        assert(lo == total_polls && hi == total_polls);
        assert(hal_irq_diag_injected == injected && hal_irq_diag_ee_off > 0);
        assert(hal_irq_diag_pending == sources && hal_irq_diag_line == sources);
        assert(hal_irq_diag_notready > 0);
    } else {
        assert(!lo && !hi && !sources && !hal_irq_diag_ee_off);
        assert(!hal_irq_diag_injected && !hal_irq_diag_pending);
        assert(!hal_irq_diag_notready && !hal_irq_diag_line);
    }
    assert(manifest_blocks == (enabled("GC_RUNTIME_MANIFEST") ? total_polls : 0));
    printf("guest-digest=%016llx polls=%llu injected=%llu\n",
           (unsigned long long)digest, (unsigned long long)total_polls,
           (unsigned long long)injected);
}
static void trace_order(void)
{
    reset(); emit_manifest = 1;
    assert(poll(0) == 0); /* initialize RAM watch before the mutation */
    gc.pc = 0x8001d874u; mutate = 1;
    assert(poll(12) == 0);
    assert(hal_cpu_cycles() == 12 && gc.pc == 0x802c32f4u);
}
static void ax_order(void)
{
    reset(); gc.pc = 0x802a5264u; gc.lr = 0x80004100u;
    gc.r[1] = 0x80008000u; gc.r[13] = 0x80007000u; gc.r[3] = 0x80002000u;
    put32(0xa08, 10); gc_ram[0x2000 + 2 * 41 + 1] = 1;
    mutate = 2;
    assert(poll(1) == 0); assert(poll(0) == 0);
    assert(hal_mem_read32(0xa08) == 11);
}
static void benchmark(unsigned count)
{
    reset();
    struct timespec a, b;
    assert(clock_gettime(CLOCK_MONOTONIC, &a) == 0);
    uint32_t result = 0;
    for (unsigned i = 0; i < count; ++i) result |= hal_irq_poll_cycles(0);
    assert(clock_gettime(CLOCK_MONOTONIC, &b) == 0);
    assert(!result && hal_poll_count() == count && !device_calls);
    double ns = (b.tv_sec - a.tv_sec) * 1e9 + b.tv_nsec - a.tv_nsec;
    printf("%.3f\n", ns / count);
}
int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "bench"))
        benchmark(argc > 2 ? (unsigned)strtoul(argv[2], NULL, 10) : 20000000u);
    else if (argc > 1 && !strcmp(argv[1], "trace")) trace_order();
    else if (argc > 1 && !strcmp(argv[1], "ax")) ax_order();
    else functional();
    return 0;
}
