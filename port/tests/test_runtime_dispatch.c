/* Real runtime_glue.c dispatch regressions and an optional lookup microbenchmark.
 * Link as a separate translation unit; test_runtime_dispatch.py drives builds.
 * The fake lookup preserves the generated table's exact-first alias semantics.
 */
#include "../hal/gc_abi.h"
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#endif

uint8_t gc_ram[0x01800000];
uint64_t hal_poll_count(void) { return 0; }
uint64_t hal_frame_count(void) { return 0; }
uint16_t hal_dsp_csr(void) { return 0; }
uint16_t hal_mmio_read16(uint32_t p) { (void)p; return 0; }
extern uint32_t gc_prev;
extern void gc_trace_dump(const char *path);
/* Test-only wrapper appended by the Python driver; no production API added. */
extern GcBlockFn test_dispatch_lookup(uint32_t ea);

#define A 0x80100100u
#define B 0x80110110u /* Same slot as A in the 16384-entry ((ea>>2)^(ea>>14)) cache, different full key. */
#define PHYS 0x00110000u
#define VIRT 0x80110000u
#define UNCACHED 0xc0110000u
#define HANDLER_A 0x802c3a40u
#define HANDLER_B 0x802c3b60u
#define RETURN_PC 0x80120004u
#define CONTEXT 0x003c0000u
static uint64_t lookups, calls;
static unsigned which, exception;
static uint32_t last_lookup;
static void block_a(void) { ++calls; which = 1; }
static void block_b(void) { ++calls; which = 2; }
static void physical(void) { ++calls; which = 3; }
static void virtual_alias(void) { ++calls; which = 4; }
static void zero_block(void) { ++calls; which = 5; }
static void no_op(void) { ++calls; }
static void handler_a(void)
{
    ++calls; which = 6;
    assert(gc.r[3] == exception && gc.r[4] == (CONTEXT | 0x80000000u));
    gc.fpr_bits[7] = 0; gc.fpr_ps1_bits[7] = 0; gc.fpscr = 0;
}
static void handler_b(void) { handler_a(); which = 7; }

typedef struct { uint32_t ea; GcBlockFn fn; } Entry;
static Entry *bench_table;
static uint32_t bench_count;
static GcBlockFn search(uint32_t ea)
{
    uint32_t lo = 0, hi = bench_count;
    while (lo < hi) {
        uint32_t mid = lo + ((hi - lo) >> 1);
        if (bench_table[mid].ea < ea) lo = mid + 1;
        else if (bench_table[mid].ea > ea) hi = mid;
        else return bench_table[mid].fn;
    }
    return NULL;
}
static GcBlockFn exact(uint32_t ea)
{
    if (bench_table) return search(ea);
    switch (ea) {
    case A: return block_a;
    case B: return block_b;
    case PHYS: return physical;
    case VIRT: return virtual_alias;
    case 0: return zero_block;
    case HANDLER_A: return handler_a;
    case HANDLER_B: return handler_b;
    case RETURN_PC: return no_op;
    case 0x8028dd70u: case 0x8028e298u: return no_op;
    default: return NULL;
    }
}
GcBlockFn gc_block_lookup(uint32_t ea)
{
    ++lookups; last_lookup = ea;
    GcBlockFn fn = exact(ea);
    if (!fn) fn = exact((ea & 0x1fffffffu) | 0x80000000u);
    return fn;
}
static void wr32(uint32_t p, uint32_t v)
{
    gc_ram[p] = (uint8_t)(v >> 24); gc_ram[p+1] = (uint8_t)(v >> 16);
    gc_ram[p+2] = (uint8_t)(v >> 8); gc_ram[p+3] = (uint8_t)v;
}
static uint32_t rd32(uint32_t p)
{
    return ((uint32_t)gc_ram[p] << 24) | ((uint32_t)gc_ram[p+1] << 16)
         | ((uint32_t)gc_ram[p+2] << 8) | gc_ram[p+3];
}
static void semantics(void)
{
    gc.pc = 0x80003100u; gc.lr = 0x12345678u;
    gc_dispatch(A);
    assert(lookups == 1 && last_lookup == A && which == 1);
    for (unsigned i = 0; i < 20; ++i) gc_dispatch(A);
    assert(lookups == 1 && calls == 21 && gc_prev == A && gc.pc == A);
    gc_dispatch(B); assert(lookups == 2 && which == 2);
    gc_dispatch(B); assert(lookups == 2 && which == 2);
    gc_dispatch(A); assert(lookups == 3 && which == 1);
    puts("PASS cold/hit dispatch, full-key collision replacement, call/PC bookkeeping");

    const uint32_t aliases[] = { PHYS, VIRT, UNCACHED, 0 };
    const unsigned expected[] = { 3, 4, 4, 5 };
    for (unsigned i = 0; i < 4; ++i) {
        uint64_t before = lookups;
        gc_dispatch(aliases[i]);
        assert(lookups == before + 1 && last_lookup == aliases[i]);
        assert(which == expected[i] && gc.pc == aliases[i]);
        gc_dispatch(aliases[i]);
        assert(lookups == before + 1 && which == expected[i]);
    }
    puts("PASS exact physical key wins, virtual/uncached aliases, valid address zero");

    uint64_t misses_before = lookups;
    assert(test_dispatch_lookup(0xdeadbeefu) == NULL);
    assert(test_dispatch_lookup(0xdeadbeefu) == NULL);
    assert(lookups == misses_before + 2); /* A null result is never cached. */
    puts("PASS repeated null lookups remain misses");

    wr32(0xc0u, CONTEXT); wr32(0xd4u, CONTEXT | 0x80000000u);
    const uint32_t handlers[] = { HANDLER_A, HANDLER_A, HANDLER_B, HANDLER_B };
    uint64_t first_hit_lookups = 0;
    for (unsigned i = 0; i < 4; ++i) {
        uint32_t vector = i == 3 ? 0x80000900u : 0x80000500u;
        exception = i == 3 ? 8 : 4;
        wr32(0x3010u, handlers[i]); wr32(0x3020u, handlers[i]);
        gc.pc = 0x80004000u; gc.srr0 = RETURN_PC; gc.srr1 = 0xd032u;
        gc.r[1] = 0x803e0000u; gc.r[8] = 100 + i;
        uint64_t fp0 = 0x7ff80000ab000000ull + i;
        uint64_t fp1 = 0xfff80000cd000000ull + i;
        gc.fpr_bits[7] = fp0; gc.fpr_ps1_bits[7] = fp1; gc.fpscr = 50 + i;
        gc_dispatch(vector);
        assert(gc.pc == handlers[i] && which == (i < 2 ? 6u : 7u));
        assert(rd32(CONTEXT + 32) == 100 + i && gc.fpr_bits[7] == 0);
        gc.pc = 0x802c0110u; gc_dispatch(RETURN_PC);
        assert(gc.fpr_bits[7] == fp0 && gc.fpr_ps1_bits[7] == fp1);
        assert(gc.fpscr == 50 + i);
        if (i == 0) first_hit_lookups = lookups;
        if (i == 1) assert(lookups == first_hit_lookups);
    }
    puts("PASS IRQ remap rereads mutable handlers; cached handler and rfi retain FP shadow");

    /* Hook side effects must run on hits too. Python checks four enter/exit logs. */
    for (unsigned i = 0; i < 4; ++i) {
        gc.r[3] = 64 + i; gc_dispatch(0x8028dd70u);
        gc.r[3] = 0x80300000u + i * 64; gc_dispatch(0x8028e298u);
    }
    gc_trace_dump("dispatch_trace.txt");
    FILE *f = fopen("dispatch_trace.txt", "r"); assert(f);
    unsigned n = 0, pc, lr;
    while (fscanf(f, "%x %x", &pc, &lr) == 2) ++n;
    assert(fclose(f) == 0 && n == calls);
    printf("PASS hook and trace run for every call (%" PRIu64 " calls, %" PRIu64 " lookups)\n", calls, lookups);
}
static uint64_t now_ns(void)
{
#ifdef _WIN32
    LARGE_INTEGER counter, frequency;
    assert(QueryPerformanceCounter(&counter) && QueryPerformanceFrequency(&frequency));
    return (uint64_t)((double)counter.QuadPart * 1e9 / (double)frequency.QuadPart);
#else
    struct timespec t; assert(clock_gettime(CLOCK_MONOTONIC, &t) == 0);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
#endif
}
static int benchmark_safe_pc(uint32_t ea)
{
    /* Random no-op calls cannot model malloc entry/exit pairing or an rfi.
     * Keep those stateful hook addresses in the table but out of the trace. */
    return ea != 0x8028f120u && ea != 0x8028dd70u && ea != 0x8028e298u
        && ea != 0x80281d2cu && ea != 0x80281d74u
        && ea != 0x802c010cu && ea != 0x802c0110u;
}
static void benchmark(const char *path, unsigned hot, unsigned iterations)
{
    FILE *f = fopen(path, "r"); assert(f);
    unsigned count; assert(fscanf(f, "%u", &count) == 1 && count > hot);
    bench_count = count; bench_table = malloc(sizeof(*bench_table) * count); assert(bench_table);
    for (unsigned i = 0; i < count; ++i) {
        assert(fscanf(f, "%x", &bench_table[i].ea) == 1);
        bench_table[i].fn = no_op;
    }
    assert(fclose(f) == 0);
    uint32_t *trace = malloc(sizeof(*trace) * 65536u); assert(trace);
    uint32_t random = 0x13579bdu;
    for (unsigned i = 0; i < 65536; ++i) {
        random = random * 1664525u + 1013904223u;
        unsigned slot = (random >> 8) % hot;
        unsigned index = (unsigned)((uint64_t)slot * count / hot);
        while (!benchmark_safe_pc(bench_table[index].ea)) index = (index + 1u) % count;
        trace[i] = bench_table[index].ea;
    }
    for (unsigned i = 0; i < 65536; ++i) gc_dispatch(trace[i]);
    uint64_t before = lookups, start = now_ns();
    for (unsigned i = 0; i < iterations; ++i) gc_dispatch(trace[i & 65535u]);
    uint64_t elapsed = now_ns() - start;
    printf("BENCH hot=%u iterations=%u ns_per_dispatch=%.3f lookups=%" PRIu64 " hit_pct=%.3f\n",
           hot, iterations, (double)elapsed / iterations, lookups - before,
           100.0 * (1.0 - (double)(lookups - before) / iterations));
    assert(calls == (uint64_t)iterations + 65536u);
    free(trace); free(bench_table);
}
int main(int argc, char **argv)
{
#ifdef _WIN32
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    if (argc == 5 && strcmp(argv[1], "bench") == 0) {
        benchmark(argv[2], (unsigned)strtoul(argv[3], NULL, 10),
                  (unsigned)strtoul(argv[4], NULL, 10));
    } else if (argc == 3 && strcmp(argv[1], "miss") == 0) {
        gc.pc = 0x80003100u; gc_dispatch(A); gc_dispatch(A);
        gc_dispatch((uint32_t)strtoul(argv[2], NULL, 16));
        assert(!"missing block must abort");
    } else semantics();
    return 0;
}
