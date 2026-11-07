/* Standalone regression for the real runtime_glue.c IRQ dispatch boundary.
 * No generated code, game image, or shared build outputs are needed.
 * Run from any directory (substitute the repository's absolute root):
 * clang -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
 *   -fno-omit-frame-pointer port/tests/test_runtime_fp_context.c \
 *   port/recomp/runtime_glue.c -o /tmp/test_runtime_fp_context
 * /tmp/test_runtime_fp_context
 *
 * Fake guest blocks model handler FP clobber and the OSLoadContext rfi boundary;
 * the code under test is the separately linked, actual gc_dispatch/vector HLE.
 * These tests do not claim complete generated SDK or cooperative FPU semantics.
 */
#include "../hal/gc_abi.h"
#include <assert.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <process.h>
#else
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

uint8_t gc_ram[0x01800000];
uint64_t hal_poll_count(void) { return 0; }
uint64_t hal_frame_count(void) { return 0; }
uint16_t hal_dsp_csr(void) { return 0; }
uint16_t hal_mmio_read16(uint32_t phys) { (void)phys; return 0; }

#define CTX_A 0x803c0000u
#define CTX_B 0x803c0400u
#define HANDLER 0x802c3a40u
#define RETURN_PC 0x8001d884u
#define OTHER_PC 0x8001d89cu
#define LOAD_CONTEXT 0x802c00bcu
#define PA(a) ((a) & 0x1ffffffu)

typedef struct {
    uint64_t fpr[32], ps1[32];
    uint32_t gqr[8], fpscr;
} FpState;
static unsigned returns;
static uint32_t load_block = 0x802c0110u;

static uint32_t rd32(uint32_t a)
{
    a = PA(a);
    return ((uint32_t)gc_ram[a] << 24) | ((uint32_t)gc_ram[a+1] << 16)
         | ((uint32_t)gc_ram[a+2] << 8) | gc_ram[a+3];
}
static void wr32(uint32_t a, uint32_t v)
{
    a = PA(a);
    gc_ram[a] = (uint8_t)(v >> 24); gc_ram[a+1] = (uint8_t)(v >> 16);
    gc_ram[a+2] = (uint8_t)(v >> 8); gc_ram[a+3] = (uint8_t)v;
}
static FpState seed_fp(unsigned seed)
{
    FpState s;
    for (unsigned r = 0; r < 32; ++r) {
        /* Include distinct NaN payloads in both FPR lanes: preserve raw bits. */
        s.fpr[r] = ((uint64_t)(0x7ff80000u + seed + r) << 32)
                 | (0x3f800000u + seed * 37u + r);
        gc.fpr_bits[r] = s.fpr[r];
        s.ps1[r] = s.fpr[r] ^ 0x80000000f0000000ull;
        gc.fpr_ps1_bits[r] = s.ps1[r];
    }
    for (unsigned r = 0; r < 8; ++r) gc.gqr[r] = s.gqr[r] = seed * 100u + r;
    gc.fpscr = s.fpscr = 0x80000000u | seed;
    return s;
}
static void expect_fpr_fpscr(const FpState *s)
{
    assert(memcmp(gc.fpr_bits, s->fpr, sizeof(s->fpr)) == 0);
    assert(memcmp(gc.fpr_ps1_bits, s->ps1, sizeof(s->ps1)) == 0);
    assert(gc.fpscr == s->fpscr);
}
static void expect_fp(const FpState *s)
{
    expect_fpr_fpscr(s);
    assert(memcmp(gc.gqr, s->gqr, sizeof(s->gqr)) == 0);
}
static void fake_handler(void)
{
    assert(gc.r[3] == 4 || gc.r[3] == 8);
    (void)seed_fp(999);
    /* Handler deliberately parks this context; a later explicit load resumes it. */
}
static void returned_block(void) { ++returns; }
static void fake_load_context(void)
{
    uint32_t ctx = gc.r[3];
    wr32(0xD4u, ctx);
    for (unsigned r = 0; r < 32; ++r) gc.r[r] = rd32(ctx + 4u*r);
    for (unsigned r = 1; r < 8; ++r) gc.gqr[r] = rd32(ctx + 0x1A8u + 4u*(r-1));
    gc.cr = rd32(ctx + 0x80u);
    gc.lr = rd32(ctx + 0x84u);
    gc.ctr = rd32(ctx + 0x88u);
    gc.xer = rd32(ctx + 0x8Cu);
    gc.srr0 = rd32(ctx + 0x198u);
    gc.srr1 = rd32(ctx + 0x19Cu);
    gc.msr = gc.srr1;
    gc.pc = load_block;
    gc_dispatch(gc.srr0);
}
GcBlockFn gc_block_lookup(uint32_t ea)
{
    switch (ea) {
    case HANDLER: return fake_handler;
    case LOAD_CONTEXT: return fake_load_context;
    case RETURN_PC: case OTHER_PC: return returned_block;
    default: assert(!"unexpected fake block address"); return NULL;
    }
}
static void enter_irq(uint32_t ctx, uint32_t pc, uint32_t sp, uint32_t vector)
{
    wr32(0xC0u, PA(ctx)); wr32(0xD4u, ctx); wr32(0xE4u, ctx);
    wr32(0x3010u, HANDLER); wr32(0x3020u, HANDLER);
    gc.r[1] = sp; gc.srr0 = pc; gc.srr1 = 0xd032u;
    gc.msr = 0x5032u; gc.pc = vector;
    gc_dispatch(vector);
}
static void resume(uint32_t ctx)
{
    gc.r[3] = ctx;
    gc_dispatch(LOAD_CONTEXT);
}
static void basic(void)
{
    FpState s = seed_fp(1);
    wr32(CTX_A + 0x1C4u, 0x12345678u);
    wr32(CTX_A + 0x1C8u, 0xabcdef01u);
    enter_irq(CTX_A, RETURN_PC, 0x803e5338u, 0x80000500u);
    for (unsigned r = 1; r < 8; ++r)
        assert(rd32(CTX_A + 0x1A8u + 4u*(r-1)) == s.gqr[r]);
    assert(rd32(CTX_A + 0x1C4u) == 0x12345678u);
    assert(rd32(CTX_A + 0x1C8u) == 0xabcdef01u);
    assert(rd32(CTX_A + 0x198u) == RETURN_PC);
    assert(gc.fpr_bits[1] != s.fpr[1]);
    resume(CTX_A);
    expect_fp(&s);
    assert(returns == 1 && gc.pc == RETURN_PC && gc.r[1] == 0x803e5338u);
    /* A completed frame cannot overwrite new FP work on a later context load. */
    FpState fresh = seed_fp(2);
    resume(CTX_A);
    expect_fpr_fpscr(&fresh);
    assert(gc.gqr[0] == fresh.gqr[0]);
    for (unsigned r = 1; r < 8; ++r) assert(gc.gqr[r] == s.gqr[r]);
}
static void switched(void)
{
    FpState a = seed_fp(10);
    enter_irq(CTX_A, RETURN_PC, 0x803e5338u, 0x80000500u);
    FpState b = seed_fp(20);
    enter_irq(CTX_B, OTHER_PC, 0x803e1238u, 0x80000900u);
    /* Contexts need not return in global LIFO order. */
    resume(CTX_A); expect_fp(&a);
    resume(CTX_B); expect_fp(&b);
    assert(returns == 2);
}
static void nested(void)
{
    FpState a = seed_fp(30);
    enter_irq(CTX_A, RETURN_PC, 0x803e5338u, 0x80000500u);
    unsigned char outer[0x1C4];
    memcpy(outer, &gc_ram[PA(CTX_A)], sizeof(outer));
    FpState b = seed_fp(40);
    /* Even identical context/PC/SP is paired in context-local LIFO order. */
    enter_irq(CTX_A, RETURN_PC, 0x803e5338u, 0x80000900u);
    resume(CTX_A); expect_fp(&b);
    /* Guest nesting must also restore its outer GPR/control context storage. */
    memcpy(&gc_ram[PA(CTX_A)], outer, sizeof(outer));
    load_block = 0x802c010cu;
    resume(CTX_A); expect_fp(&a);
}
static void mismatch(void)
{
    FpState a = seed_fp(50);
    enter_irq(CTX_A, RETURN_PC, 0x803e5338u, 0x80000500u);
    FpState changed = seed_fp(60);
    /* Ordinary dispatch to the target does not restore. */
    gc.pc = 0x80010000u; gc_dispatch(RETURN_PC); expect_fp(&changed);
    wr32(CTX_A + 0x198u, OTHER_PC);
    resume(CTX_A); expect_fpr_fpscr(&changed);
    wr32(CTX_A + 0x198u, RETURN_PC);
    wr32(CTX_A + 4u, 0x803e5000u);
    resume(CTX_A); expect_fpr_fpscr(&changed);
    wr32(CTX_A + 4u, 0x803e5338u);
    resume(CTX_A); expect_fp(&a);
}
static void capacity(void)
{
    for (unsigned i = 0; i < 65; ++i)
        enter_irq(CTX_A, RETURN_PC, 0x803e5338u, 0x80000500u);
    assert(!"capacity must fail closed");
}
static void reuse_slots(void)
{
    for (unsigned i = 0; i < 100; ++i) {
        FpState a = seed_fp(i);
        enter_irq(CTX_A, RETURN_PC, 0x803e5338u, 0x80000500u);
        resume(CTX_A); expect_fp(&a);
    }
    assert(returns == 100);
}
static const struct {
    const char *name;
    void (*scenario)(void);
    int expected_signal;
} scenarios[] = {
    {"two-lane FP/FPSCR/GQR roundtrip; guest layout; no stale return cache", basic, 0},
    {"interleaved contexts; external and decrementer vectors", switched, 0},
    {"same-context nested IRQs with identical return identity", nested, 0},
    {"non-rfi/PC/SP mismatch does not consume pending frame", mismatch, 0},
    {"completed frames release capacity", reuse_slots, 0},
    {"outstanding capacity fails closed", capacity, SIGABRT},
};
static void run(unsigned index)
{
    const char *name = scenarios[index].name;
    int expected_signal = scenarios[index].expected_signal;
    assert(fflush(NULL) == 0);
#ifdef _WIN32
    char argument[16];
    snprintf(argument, sizeof(argument), "%u", index);
    intptr_t status = _spawnl(_P_WAIT, _pgmptr, _pgmptr, argument, NULL);
    assert(status != -1);
    /* Windows abort exits with the CRT's documented status 3. */
    assert(status == (expected_signal ? 3 : 0));
#else
    pid_t pid = fork(); assert(pid >= 0);
    if (!pid) {
        struct rlimit no_core = {0, 0};
        assert(setrlimit(RLIMIT_CORE, &no_core) == 0);
        scenarios[index].scenario();
        _exit(0);
    }
    int status; assert(waitpid(pid, &status, 0) == pid);
    if (expected_signal) assert(WIFSIGNALED(status) && WTERMSIG(status) == expected_signal);
    else assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
#endif
    printf("PASS %s\n", name);
}
int main(int argc, char **argv)
{
#ifdef _WIN32
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    if (argc == 2) {
        unsigned index = (unsigned)strtoul(argv[1], NULL, 10);
        assert(index < sizeof(scenarios) / sizeof(scenarios[0]));
        scenarios[index].scenario();
        return 0;
    }
    for (unsigned i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); ++i) run(i);
    return 0;
}
