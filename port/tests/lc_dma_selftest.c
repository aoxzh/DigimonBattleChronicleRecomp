/* Standalone LC DMA regression tests. No game assets or generated code.
 * Build instructions and pinned upstream provenance: ../docs/analysis/lc-dma.md.
 */
#include "../hal/mmio.h"
#include "../hal/mem.h"
#include "../hal/hal.h"
#include "../hal/gc_abi.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

uint8_t gc_ram[GC_MEM1_SIZE];
static uint8_t ram_before[GC_MEM1_SIZE], lc_before[0x4000];
static unsigned other_calls, wg8, wg16, wg32, wg64;
#define LC_PHYS 0x01800000u

/* Link only the real mmio.c. Unrelated hardware is deliberately inert, and
 * counters prove that malformed DMA does not turn into MMIO/FIFO accesses. */
#define STUB_R16(name) uint16_t name(uint32_t r) { (void)r; ++other_calls; return 0; }
#define STUB_R32(name) uint32_t name(uint32_t r) { (void)r; ++other_calls; return 0; }
#define STUB_W16(name) void name(uint32_t r, uint16_t v) { (void)r; (void)v; ++other_calls; }
#define STUB_W32(name) void name(uint32_t r, uint32_t v) { (void)r; (void)v; ++other_calls; }
STUB_R16(hal_cp_read16) STUB_W16(hal_cp_write16)
STUB_R16(hal_pe_read16) STUB_W16(hal_pe_write16)
STUB_R16(hal_vi_read16) STUB_W16(hal_vi_write16)
STUB_R16(hal_dsp_read16) STUB_W16(hal_dsp_write16)
STUB_R16(hal_ai_read16) STUB_W16(hal_ai_write16)
STUB_R32(hal_pi_read32) STUB_W32(hal_pi_write32)
STUB_R32(hal_di_read32) STUB_W32(hal_di_write32)
STUB_R32(hal_si_read32) STUB_W32(hal_si_write32)
STUB_R32(hal_exi_read32) STUB_W32(hal_exi_write32)
void hal_pi_clear_interrupt(uint32_t bits) { (void)bits; ++other_calls; }
void gc_runtime_manifest_note_mmio(uint32_t p, unsigned w, int wr)
{ (void)p; (void)w; (void)wr; }
void hal_gxpipe_write8(uint8_t v) { (void)v; ++wg8; }
void hal_gxpipe_write16(uint16_t v) { (void)v; ++wg16; }
void hal_gxpipe_write32(uint32_t v) { (void)v; ++wg32; }
void hal_gxpipe_write64(uint64_t v) { (void)v; ++wg64; }

static uint8_t pattern(unsigned i) { return (uint8_t)((i * 73u + 19u) ^ (i >> 3)); }
static void fill_ram(uint32_t start, unsigned n)
{
    for (unsigned i = 0; i < n; ++i) gc_ram[start + i] = pattern(i);
}
static void check_lc(uint32_t start, const uint8_t *p, unsigned n)
{
    for (unsigned i = 0; i < n; ++i) assert(hal_slow_read8(LC_PHYS + start + i) == p[i]);
}
static void snapshot(void)
{
    memcpy(ram_before, gc_ram, sizeof(gc_ram));
    for (unsigned i = 0; i < sizeof(lc_before); ++i) lc_before[i] = hal_slow_read8(LC_PHYS + i);
}
static void unchanged(void)
{
    assert(memcmp(ram_before, gc_ram, sizeof(gc_ram)) == 0);
    check_lc(0, lc_before, sizeof(lc_before));
    assert(other_calls == 0 && wg8 == 0 && wg16 == 0 && wg32 == 0 && wg64 == 0);
}
static void rejected(uint32_t dmau, uint32_t dmal)
{
    snapshot();
    assert(!hal_lc_dma(dmau, dmal));
    unchanged();
}

static void directions_and_fields(void)
{
    memset(gc_ram, 0xA5, sizeof(gc_ram));
    for (unsigned i = 0; i < sizeof(lc_before); ++i) hal_slow_write8(LC_PHYS + i, 0x5A);
    fill_ram(0x1000, 224);
    /* Fixed raw register vectors, not an encoder mirroring the implementation.
     * DMAU low5=1 + DMAL bits2..3=3 => 7 lines (224B).
     * DMAL ...9f includes DMA_F, trigger, length, load; cache address is ...80. */
    assert(hal_lc_dma(0x80001001u, 0xE000009Fu));
    check_lc(0x80, gc_ram + 0x1000, 224);
    assert(hal_slow_read8(LC_PHYS + 0x7F) == 0x5A);
    assert(hal_slow_read8(LC_PHYS + 0x160) == 0x5A);
    assert(hal_slow_read32(LC_PHYS + 0x80) ==
           ((uint32_t)pattern(0) << 24 | (uint32_t)pattern(1) << 16 |
            (uint32_t)pattern(2) << 8 | pattern(3)));
    hal_slow_write32(LC_PHYS + 0x80, 0x01234567u);
    /* Clear DMA_LD stores back; uncached alias targets distinct destination. */
    assert(hal_lc_dma(0xC0002001u, 0xE000008Eu));
    assert(gc_ram[0x2000] == 0x01 && gc_ram[0x2001] == 0x23);
    assert(gc_ram[0x2002] == 0x45 && gc_ram[0x2003] == 0x67);
    assert(memcmp(gc_ram + 0x2004, gc_ram + 0x1004, 220) == 0);
    assert(gc_ram[0x1FFF] == 0xA5 && gc_ram[0x20E0] == 0xA5);
    assert(gc_ram[0x1000] == pattern(0));
    /* Offset form, 1 line, flag bit 0 must not change the cache address. */
    assert(hal_lc_dma(0x00001000u, 0x00000017u));
    check_lc(0, gc_ram + 0x1000, 32);
    assert(hal_lc_dma(0x00003000u, 0x00000006u));
    assert(memcmp(gc_ram + 0x3000, gc_ram + 0x1000, 32) == 0);
    /* No trigger, even when DMA_F and DMA_LD are set, must not copy. */
    rejected(0x80001001u, 0xE000009Du);
    puts("PASS raw vectors: directions, split count, 32B address fields, byte order, aliases, trigger");
}

static void zero_and_length_limits(void)
{
    fill_ram(0x4000, 4096);
    /* Zero 7-bit count encodes 128 lines, not an empty transfer. */
    assert(hal_lc_dma(0x80004000u, 0xE0001012u));
    check_lc(0x1000, gc_ram + 0x4000, 4096);
    assert(hal_lc_dma(0x80006000u, 0xE0001002u));
    assert(memcmp(gc_ram + 0x4000, gc_ram + 0x6000, 4096) == 0);
    /* High5=31 / low2=3 is 127 lines, 4064 bytes. */
    hal_slow_write8(LC_PHYS + 0x2FE0, 0xD7);
    assert(hal_lc_dma(0x8000401Fu, 0xE000201Eu));
    check_lc(0x2000, gc_ram + 0x4000, 4064);
    assert(hal_slow_read8(LC_PHYS + 0x2FE0) == 0xD7);
    puts("PASS zero encoding = 4096B and maximum nonzero count = 4064B");
}

static void guarded_ends(void)
{
    fill_ram(GC_MEM1_SIZE - 4096, 4096);
    assert(hal_lc_dma(0x817FF000u, 0xE0003012u));
    check_lc(0x3000, gc_ram + GC_MEM1_SIZE - 4096, 4096);
    assert(hal_lc_dma(0xC17FFFE0u, 0xE0003FF6u));
    check_lc(0x3FE0, gc_ram + GC_MEM1_SIZE - 32, 32);
    hal_slow_write32(LC_PHYS + 0x3FFC, 0xFEDCBA98u);
    assert(hal_lc_dma(0x017FFFE0u, 0xE0003FE6u));
    assert(gc_ram[GC_MEM1_SIZE - 4] == 0xFE && gc_ram[GC_MEM1_SIZE - 1] == 0x98);
    assert(hal_lc_dma(0x817FF000u, 0xE0003002u));

    rejected(0x817FFFE0u, 0xE000000Au); /* 2 lines cross RAM end */
    rejected(0x817FFFE0u, 0xE000001Au); /* same, load */
    rejected(0x80001000u, 0xE0003FEAu); /* 2 lines cross LC end */
    rejected(0x80001000u, 0xE0003FFAu);
    rejected(0x81800000u, 0xE0000016u); /* RAM first invalid address */
    rejected(0x80001000u, 0xE0004006u); /* LC first invalid address */
    rejected(0x80001000u, 0x00004016u); /* offset first invalid */
    rejected(0x80001000u, 0x01800016u); /* HAL backing is not guest LC EA */
    rejected(0xCC008000u, 0xE0000016u); /* MMIO must not become RAM */
    rejected(0xE0001000u, 0xE0000006u); /* LC is not MEM1 */
    rejected(0x20001000u, 0xE0000016u); /* unsupported MEM1 alias */
    rejected(0x80001000u, 0xC0000016u); /* unsupported LC alias */
    rejected(0xFFFFFFE0u, 0xFFFFFFF6u); /* no integer wrap */
    rejected(0x817FF020u, 0xE0003012u); /* zero encoding still bounds checked */
    rejected(0x80001000u, 0xE0003022u); /* zero count crosses LC end */
    puts("PASS exact ends and atomic OOB rejection in both directions");
}

static void slow_access_bounds(void)
{
    hal_slow_write64(LC_PHYS + 0x3FF8, UINT64_C(0x0123456789ABCDEF));
    assert(hal_slow_read64(LC_PHYS + 0x3FF8) == UINT64_C(0x0123456789ABCDEF));
    assert(hal_slow_read32(LC_PHYS + 0x3FFC) == 0x89ABCDEFu);
    assert(hal_slow_read16(LC_PHYS + 0x3FFE) == 0xCDEFu);
    snapshot();
    hal_slow_write16(LC_PHYS + 0x3FFF, 0);
    hal_slow_write32(LC_PHYS + 0x3FFE, 0);
    hal_slow_write64(LC_PHYS + 0x3FF9, 0);
    assert(hal_slow_read16(LC_PHYS + 0x3FFF) == 0);
    assert(hal_slow_read32(LC_PHYS + 0x3FFD) == 0);
    assert(hal_slow_read64(LC_PHYS + 0x3FF9) == 0);
    assert(hal_slow_read8(LC_PHYS + 0x4000) == 0);
    unchanged();
    puts("PASS ordinary LC wide accesses cannot cross backing end");
}

static void wg_aliases_preserved(void)
{
    hal_mmio_write8(0x0C008FFFu, 1);
    hal_mmio_write16(0x0C008FFEu, 2);
    hal_mmio_write32(0x0C008FFCu, 3);
    hal_slow_write64(0x0C008FF8u, 4);
    assert(wg8 == 1 && wg16 == 1 && wg32 == 1 && wg64 == 1);
    assert(other_calls == 0);
    puts("PASS pre-existing full WGPIPE page alias");
}

int main(void)
{
    directions_and_fields();
    zero_and_length_limits();
    guarded_ends();
    slow_access_bounds();
    wg_aliases_preserved();
    puts("lc_dma_selftest: all checks passed");
    return 0;
}
