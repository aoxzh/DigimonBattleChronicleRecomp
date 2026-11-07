/* hal/mmio.c — MMIO 派发 + MI（MemInterface，0xCC004000）影子。
 * MI 寄存器布局出处：Dolphin Source/Core/Core/HW/MemoryInterface.h
 *   MIMemStruct：regions[4]（0x00..0x0F）、prot_type=0x10、irq_mask=0x12、
 *   irq_flag=0x14（W1C）、unknown1=0x16、prot_addr=0x18、timers[10]=0x1C..、
 *   unknown2=0x44。
 * 块编号 = (phys >> 8) & 0xFF（GC MMIO 全部位于 0x0C00xx00 页）：
 *   0x00=CP 0x10=PE 0x20=VI 0x30=PI 0x40=MI 0x50=DSP 0x60=DI 0x64=SI
 *   0x68=EXI 0x6C=AI 0x80=WGPIPE
 */
#include "mmio.h"
#include "hal.h"
#include "irq.h"
#include "gc_abi.h"
#include "mem.h"

#include <stdio.h>
#include <string.h>

int hal_mmio_trace;

static bool is_mmio(uint32_t phys)
{
    return phys >= 0x0C000000u && phys < 0x0D000000u;
}

static bool is_wgpipe(uint32_t phys)
{
    return (phys & 0xFFFFF000u) == HW_WGPIPE;
}

/* ---- MI 影子（16 位表，偏移 0x00..0x46 偶数） ---- */
static uint16_t mi_r[0x48 / 2];

static uint16_t mi_read16(uint32_t reg)
{
    if (reg >= 0x48) return 0;
    return mi_r[reg / 2];
}

static void mi_write16(uint32_t reg, uint16_t v)
{
    if (reg >= 0x48) return;
    if (reg == 0x14) {          /* irq_flag：W1C，全清后撤 PI MEMORY 线 */
        mi_r[reg / 2] &= (uint16_t)~v;
        if (!(mi_r[reg / 2] & mi_r[0x12 / 2]))
            hal_pi_clear_interrupt(PI_INT_MEMORY);
        return;
    }
    mi_r[reg / 2] = v;
}

/* ---- 16 位寄存器块 ---- */
static uint16_t block16_read(uint32_t phys)
{
    uint32_t off = phys & 0xFFu;
    if (hal_mmio_trace) fprintf(stderr, "mmio r16 %06x\n", phys & 0xFFFFFFu);   /* 块内偏移（此前 0xFFFF 掩码致全部路由失败）*/
    switch ((phys >> 8) & 0xFFu) {
    case 0x00: return hal_cp_read16(off);
    case 0x10: return hal_pe_read16(off);
    case 0x20: return hal_vi_read16(off);
    case 0x40: return mi_read16(off);
    case 0x50: return hal_dsp_read16(off);
    case 0x6C: return hal_ai_read16(off);
    default:   return 0;
    }
}

static void block16_write(uint32_t phys, uint16_t v)
{
    uint32_t off = phys & 0xFFu;
    if (hal_mmio_trace) fprintf(stderr, "mmio w16 %06x =%04x\n", phys & 0xFFFFFFu, v);   /* 块内偏移（此前 0xFFFF 掩码致全部路由失败）*/
    switch ((phys >> 8) & 0xFFu) {
    case 0x00: hal_cp_write16(off, v);  return;
    case 0x10: hal_pe_write16(off, v);  return;
    case 0x20: hal_vi_write16(off, v);  return;
    case 0x40: mi_write16(off, v);      return;
    case 0x50: hal_dsp_write16(off, v); return;
    case 0x6C: hal_ai_write16(off, v);  return;
    default:   return;
    }
}

/* ---- 32 位寄存器块 ---- */
static uint32_t block32_read(uint32_t phys)
{
    uint32_t off = phys & 0xFFu;   /* 块内偏移（此前 0xFFFF 掩码致全部路由失败）*/
    if (hal_mmio_trace) fprintf(stderr, "mmio r32 %06x\n", phys & 0xFFFFFFu);
    switch ((phys >> 8) & 0xFFu) {
    case 0x30: return hal_pi_read32(off);
    case 0x60: return hal_di_read32(off);
    case 0x64: return hal_si_read32(off);
    case 0x68: return hal_exi_read32(off);
    default:   return 0;
    }
}

static void block32_write(uint32_t phys, uint32_t v)
{
    uint32_t off = phys & 0xFFu;   /* 块内偏移（此前 0xFFFF 掩码致全部路由失败）*/
    if (hal_mmio_trace) fprintf(stderr, "mmio w32 %06x =%08x\n", phys & 0xFFFFFFu, v);
    switch ((phys >> 8) & 0xFFu) {
    case 0x30: hal_pi_write32(off, v);  return;
    case 0x60: hal_di_write32(off, v);  return;
    case 0x64: hal_si_write32(off, v);  return;
    case 0x68: hal_exi_write32(off, v); return;
    default:   return;
    }
}

/* ---- gc_abi.h 契约实现 ---- */
uint16_t hal_mmio_read16(uint32_t phys)
{
    hal_clock_sync_devices();
    if (!is_mmio(phys)) return 0;
    gc_runtime_manifest_note_mmio(phys, 16u, 0);
    return block16_read(phys);
}

void hal_mmio_write16(uint32_t phys, uint16_t v)
{
    hal_clock_sync_devices();
    if (!is_mmio(phys)) return;
    gc_runtime_manifest_note_mmio(phys, 16u, 1);
    if (is_wgpipe(phys)) { hal_gxpipe_write16(v); return; }
    block16_write(phys, v);
}

uint32_t hal_mmio_read32(uint32_t phys)
{
    hal_clock_sync_devices();
    if (!is_mmio(phys)) return 0;
    gc_runtime_manifest_note_mmio(phys, 32u, 0);
    switch ((phys >> 8) & 0xFFu) {
    case 0x30: case 0x60: case 0x64: case 0x68:
        return block32_read(phys);
    default:
        /* 16 位块上的 32 位访问 = 高低两个 16 位寄存器（BE） */
        return ((uint32_t)block16_read(phys) << 16) | block16_read(phys + 2);
    }
}

void hal_mmio_write32(uint32_t phys, uint32_t v)
{
    hal_clock_sync_devices();
    if (!is_mmio(phys)) return;
    gc_runtime_manifest_note_mmio(phys, 32u, 1);
    /* WGPIPE 的 stw 不能拆成两个 16 位块写——否则 DrawDone 命令整条丢失 */
    if (is_wgpipe(phys)) { hal_gxpipe_write32(v); return; }
    switch ((phys >> 8) & 0xFFu) {
    case 0x30: case 0x60: case 0x64: case 0x68:
        block32_write(phys, v);
        return;
    default:
        block16_write(phys,     (uint16_t)(v >> 16));
        block16_write(phys + 2, (uint16_t)v);
        return;
    }
}

uint8_t hal_mmio_read8(uint32_t phys)
{
    hal_clock_sync_devices();
    if (!is_mmio(phys)) return 0;
    gc_runtime_manifest_note_mmio(phys, 8u, 0);
    uint16_t w = block16_read(phys & ~1u);
    return (phys & 1) ? (uint8_t)w : (uint8_t)(w >> 8);
}

void hal_mmio_write8(uint32_t phys, uint8_t v)
{
    hal_clock_sync_devices();
    if (!is_mmio(phys)) return;
    gc_runtime_manifest_note_mmio(phys, 8u, 1);
    /* 0xCC008000 = WGPIPE（GX FIFO 字节写口；GPFifo.cpp GATHER_PIPE） */
    if (is_wgpipe(phys)) {
        hal_gxpipe_write8(v);
        return;
    }
    uint16_t cur = block16_read(phys & ~1u);
    uint16_t nv = (phys & 1) ? (uint16_t)((cur & 0xFF00u) | v)
                             : (uint16_t)((cur & 0x00FFu) | ((uint16_t)v << 8));
    block16_write(phys & ~1u, nv);
}

/* ---- 慢路径（phys >= 0x01800000；含 MMIO、LC 锁定缓存与空洞） ----
 * 0x01800000-0x01803FFF = LC 区背板：SDK __LCEnable 把 0xE0000000 的 16KB
 * 锁进 D-cache（dcbz_l，见 lift.py e_dcbz_l 注释），LCLoad/LCStore 经普通
 * 读写访问该 EA——g_phys 路由到这里，单独缓冲避免落 MEM1。 */
static uint8_t lc_ram[0x4000];
static int lc_is(uint32_t p) { return p >= 0x01800000u && p < 0x01804000u; }
static int lc_span(uint32_t p, uint32_t bytes)
{
    return lc_is(p) && bytes <= 0x01804000u - p;
}

/* DMA decode/copy adapted from Ethan Nicholas's MIT gcn-recomp,
 * runtime/cpu.cpp:273-283, commit 3fad2f2deea7e563755819cf6f44da8743c61c55.
 * Copyright (c) 2026 Ethan Nicholas. Permission and warranty terms are in
 * ../third_party/gcn-recomp/LICENSE; provenance: ../docs/analysis/lc-dma.md.
 * Keep this on the same byte backing as ordinary guest LC loads/stores. */
bool hal_lc_dma(uint32_t dmau, uint32_t dmal)
{
    if (!(dmal & 2u)) return false;
    uint32_t lines = ((dmau & 0x1Fu) << 2) | ((dmal >> 2) & 3u);
    if (!lines) lines = 128u;
    uint32_t bytes = lines * 32u;
    uint32_t mem = dmau & ~0x1Fu;
    uint32_t cache = dmal & ~0x1Fu;

    /* MEM1 aliases only; never turn an MMIO/LC/unknown address into RAM.
     * Dolphin masks LC_ADDR into a larger cache backing. This HAL supports
     * only offsets and the SDK's E0000000 alias, with no 16KB wrapping. */
    uint32_t segment = mem & 0xE0000000u;
    if (segment != 0 && segment != 0x80000000u && segment != 0xC0000000u)
        return false;
    mem &= 0x1FFFFFFFu;
    if (cache >= 0xE0000000u && cache < 0xE0004000u)
        cache -= 0xE0000000u;
    if (mem >= GC_MEM1_SIZE || bytes > GC_MEM1_SIZE - mem ||
        cache >= sizeof(lc_ram) || bytes > sizeof(lc_ram) - cache) {
        if (hal_mmio_trace)
            fprintf(stderr, "hal: rejected LC DMA DMAU=%08x DMAL=%08x bytes=%u\n",
                    dmau, dmal, bytes);
        return false;
    }
    if (dmal & 0x10u) memcpy(lc_ram + cache, gc_ram + mem, bytes);
    else memcpy(gc_ram + mem, lc_ram + cache, bytes);
    return true;
}

uint8_t hal_slow_read8(uint32_t phys)
{
    if (lc_is(phys)) return lc_ram[phys - 0x01800000u];
    if (is_mmio(phys)) return hal_mmio_read8(phys);
    if (hal_mmio_trace) fprintf(stderr, "hal: unmapped read8  @%08x\n", phys);
    return 0;
}
uint16_t hal_slow_read16(uint32_t phys)
{
    if (lc_is(phys) && !lc_span(phys, 2u)) return 0;
    if (lc_is(phys)) return (uint16_t)((lc_ram[phys - 0x01800000u] << 8)
                                     | lc_ram[phys - 0x01800000u + 1]);
    if (is_mmio(phys)) return hal_mmio_read16(phys);
    if (hal_mmio_trace) fprintf(stderr, "hal: unmapped read16 @%08x\n", phys);
    return 0;
}
uint32_t hal_slow_read32(uint32_t phys)
{
    if (lc_is(phys) && !lc_span(phys, 4u)) return 0;
    if (lc_is(phys)) {
        uint32_t b = phys - 0x01800000u;
        return ((uint32_t)lc_ram[b] << 24) | ((uint32_t)lc_ram[b+1] << 16)
             | ((uint32_t)lc_ram[b+2] << 8) | lc_ram[b+3];
    }
    if (is_mmio(phys)) return hal_mmio_read32(phys);
    if (hal_mmio_trace) fprintf(stderr, "hal: unmapped read32 @%08x\n", phys);
    return 0;
}
uint64_t hal_slow_read64(uint32_t phys)
{
    if (lc_is(phys) && !lc_span(phys, 8u)) return 0;
    return ((uint64_t)hal_slow_read32(phys) << 32) | hal_slow_read32(phys + 4);
}
void hal_slow_write8(uint32_t phys, uint8_t v)
{
    if (lc_is(phys)) { lc_ram[phys - 0x01800000u] = v; return; }
    if (is_mmio(phys)) { hal_mmio_write8(phys, v); return; }
    if (hal_mmio_trace) fprintf(stderr, "hal: unmapped write8  @%08x =%02x\n", phys, v);
}
void hal_slow_write16(uint32_t phys, uint16_t v)
{
    if (lc_is(phys) && !lc_span(phys, 2u)) return;
    if (lc_is(phys)) {
        uint32_t b = phys - 0x01800000u;
        lc_ram[b] = (uint8_t)(v >> 8); lc_ram[b+1] = (uint8_t)v;
        return;
    }
    if (is_mmio(phys)) { hal_mmio_write16(phys, v); return; }
    if (hal_mmio_trace) fprintf(stderr, "hal: unmapped write16 @%08x =%04x\n", phys, v);
}
void hal_slow_write32(uint32_t phys, uint32_t v)
{
    if (lc_is(phys) && !lc_span(phys, 4u)) return;
    if (lc_is(phys)) {
        uint32_t b = phys - 0x01800000u;
        lc_ram[b] = (uint8_t)(v >> 24); lc_ram[b+1] = (uint8_t)(v >> 16);
        lc_ram[b+2] = (uint8_t)(v >> 8); lc_ram[b+3] = (uint8_t)v;
        return;
    }
    if (is_mmio(phys)) { hal_mmio_write32(phys, v); return; }
    if (hal_mmio_trace) fprintf(stderr, "hal: unmapped write32 @%08x =%08x\n", phys, v);
}
void hal_slow_write64(uint32_t phys, uint64_t v)
{
    if (lc_is(phys) && !lc_span(phys, 8u)) return;
    if (is_wgpipe(phys)) {
        hal_clock_sync_devices();
        hal_gxpipe_write64(v); /* stfd keeps the complete eight-byte FIFO write */
        return;
    }
    hal_slow_write32(phys, (uint32_t)(v >> 32));
    hal_slow_write32(phys + 4, (uint32_t)v);
}
