/* MIT License; Copyright (c) 2026 Ethan Nicholas. See LICENSE.
 * Extracted/adapted from gcn-recomp/runtime/gx/fifo.cpp at
 * 3fad2f2deea7e563755819cf6f44da8743c61c55, lines 446-575 and 663-721.
 */
#include "cp_fifo.h"

#include <string.h>

#define CP_PTR_MASK UINT32_C(0x03ffffe0)
#define CP_MEM1_SIZE UINT32_C(0x01800000)

static void cp_update_irq(CpFifo *cp)
{
    bool bp_irq = (cp->sr & 0x10) && (cp->cr & CP_FIFO_CR_BPINT);
    bool ovf_irq = (cp->sr & 1) && (cp->cr & CP_FIFO_CR_OVFINT);
    bool unf_irq = (cp->sr & 2) && (cp->cr & CP_FIFO_CR_UNFINT);
    if (cp->irq) cp->irq(bp_irq || ovf_irq || unf_irq, cp->user);
}

/* The end register names the last 32-byte line, not the address past it. */
static uint32_t cp_end_excl(const CpFifo *cp)
{
    return cp->end ? (cp->end & CP_PTR_MASK) + 32u : 0;
}

static void cp_watermarks(CpFifo *cp)
{
    if (!(cp->cr & CP_FIFO_CR_LINK)) return;
    uint32_t base = cp->base & CP_PTR_MASK, end = cp_end_excl(cp);
    if (!end || end <= base) return;
    /* CP owns this pointer. Only cp_fifo_note_burst follows the PI snapshot. */
    uint32_t wptr = cp->wptr & CP_PTR_MASK, r = cp->rptr & CP_PTR_MASK;
    if (r < base || r >= end) r = base;
    if (wptr < base || wptr >= end) return;
    uint32_t d = wptr >= r ? wptr - r : (end - r) + (wptr - base);
    cp->distance = d;
    /* Upstream latches these conditions until CLEAR acknowledges them. */
    if (cp->hiwm && d > (cp->hiwm & CP_PTR_MASK)) cp->sr |= 1;
    if (d < (cp->lowm & CP_PTR_MASK)) cp->sr |= 2;
    cp_update_irq(cp);
}

static void cp_follow_pi(CpFifo *cp)
{
    if (cp->cr & CP_FIFO_CR_LINK) cp->wptr = cp->pi_wptr;
}

/* True when it got as far as checking the watermarks. */
static bool cp_drain(CpFifo *cp)
{
    cp->settled = false;
    if (!(cp->cr & CP_FIFO_CR_READ)) return false;
    uint32_t base = cp->base & CP_PTR_MASK, end = cp_end_excl(cp);
    uint32_t wptr = cp->wptr & CP_PTR_MASK;
    if (!end || end <= base) return false;
    /* Reject the entire ring before exposing even its first span to the caller.
     * In particular, do not alias out-of-MEM1 physical addresses back into RAM.
     */
    if (end > cp->mem1_size || end > CP_MEM1_SIZE || !cp->consume) return false;
    uint32_t r = cp->rptr & CP_PTR_MASK;
    if (r < base || r >= end) r = base;
    /* Another FIFO's write pointer must never drive this GP FIFO. */
    if (wptr < base || wptr >= end) return false;
    bool bp_on = (cp->cr & CP_FIFO_CR_BPEN) != 0;
    uint32_t bp = cp->bp & CP_PTR_MASK;
    /* At most a tail and head span; no register setup can cause an infinite
     * FIFO. As upstream, equal pointers mean empty (no full-ring inference).
     */
    for (unsigned guard = 0; r != wptr && guard < 2; guard++) {
        if (bp_on && r == bp) break;
        uint32_t stop = wptr > r ? wptr : end;
        if (bp_on && bp > r && bp < stop) stop = bp;
        if (stop > r) cp->consume(r, stop - r, cp->user);
        r = stop >= end ? base : stop;
    }
    bool hit = bp_on && r == bp;
    if (hit) cp->sr |= 0x10; else cp->sr &= (uint16_t)~0x10u;
    cp_update_irq(cp);
    cp->rptr = r;
    cp->distance = wptr >= r ? wptr - r : (end - r) + (wptr - base);
    cp_watermarks(cp);
    cp->settled = r == wptr;
    return true;
}

void cp_fifo_note_burst(CpFifo *cp, uint32_t pi_wptr)
{
    cp->pi_wptr = pi_wptr;
    /* The upstream settled fast path leaves an unlinked, drained CP alone. */
    if (!cp->settled || (cp->cr & CP_FIFO_CR_LINK)) {
        cp_follow_pi(cp);
        if (!cp_drain(cp)) cp_watermarks(cp);
    }
}

void cp_fifo_init(CpFifo *cp, uint32_t mem1_size,
                  CpFifoConsume consume, CpFifoIrq irq, void *user)
{
    memset(cp, 0, sizeof(*cp));
    cp->mem1_size = mem1_size < CP_MEM1_SIZE ? mem1_size : CP_MEM1_SIZE;
    cp->consume = consume;
    cp->irq = irq;
    cp->user = user;
    cp_update_irq(cp);
}

static uint16_t lo(uint32_t v) { return (uint16_t)v; }
static uint16_t hi(uint32_t v) { return (uint16_t)(v >> 16); }
static void set_lo(uint32_t *r, uint16_t v)
{
    *r = (*r & UINT32_C(0xffff0000)) | v;
}
static void set_hi(uint32_t *r, uint16_t v)
{
    *r = (*r & UINT32_C(0xffff)) | ((uint32_t)v << 16);
}

uint16_t cp_fifo_read16(const CpFifo *cp, uint32_t off)
{
    switch (off & 0xff) {
    case 0x00: {
        /* Do not report read-idle while bytes wait behind a breakpoint. */
        uint32_t base = cp->base & CP_PTR_MASK, end = cp_end_excl(cp);
        uint32_t r = cp->rptr & CP_PTR_MASK, w = cp->wptr & CP_PTR_MASK;
        uint32_t d = (end > base && r >= base && r < end && w >= base && w < end)
            ? (w >= r ? w - r : (end - r) + (w - base)) : 0;
        bool at_bp = (cp->cr & CP_FIFO_CR_BPEN) && r == (cp->bp & CP_PTR_MASK);
        bool read_idle = d == 0 || r == w;
        bool cmd_idle = d == 0 || at_bp || !(cp->cr & CP_FIFO_CR_READ);
        return (uint16_t)((cp->sr & 0x13) | (read_idle ? 4 : 0) | (cmd_idle ? 8 : 0));
    }
    case 0x02: return cp->cr;
    case 0x04: return cp->clear;
    case 0x20: return lo(cp->base); case 0x22: return hi(cp->base);
    case 0x24: return lo(cp->end); case 0x26: return hi(cp->end);
    case 0x28: return lo(cp->hiwm); case 0x2a: return hi(cp->hiwm);
    case 0x2c: return lo(cp->lowm); case 0x2e: return hi(cp->lowm);
    case 0x30: return lo(cp->distance); case 0x32: return hi(cp->distance);
    case 0x34: return lo(cp->wptr); case 0x36: return hi(cp->wptr);
    case 0x38: return lo(cp->rptr); case 0x3a: return hi(cp->rptr);
    case 0x3c: return lo(cp->bp); case 0x3e: return hi(cp->bp);
    default: return 0; /* Performance counters etc. are outside this subset. */
    }
}

void cp_fifo_write16(CpFifo *cp, uint32_t off, uint16_t v)
{
    cp->settled = false;
    switch (off & 0xff) {
    case 0x00: cp->sr = v; cp_update_irq(cp); break;
    case 0x02:
        cp->cr = v;
        if (!(v & CP_FIFO_CR_BPEN)) cp->sr &= (uint16_t)~0x10u;
        cp_update_irq(cp);
        cp_drain(cp);
        break;
    case 0x04:
        cp->clear = v;
        cp->sr &= (uint16_t)~(v & 3u);
        cp_watermarks(cp);
        cp_update_irq(cp); /* CLEAR must also deassert an unlinked IRQ. */
        break;
    case 0x20: set_lo(&cp->base, (uint16_t)(v & ~0x1fu)); break;
    case 0x22: set_hi(&cp->base, (uint16_t)(v & 0x3ffu)); break;
    case 0x24: set_lo(&cp->end, (uint16_t)(v & ~0x1fu)); break;
    case 0x26: set_hi(&cp->end, (uint16_t)(v & 0x3ffu)); break;
    case 0x28: set_lo(&cp->hiwm, v); break;
    case 0x2a: set_hi(&cp->hiwm, v); cp_watermarks(cp); break;
    case 0x2c: set_lo(&cp->lowm, v); break;
    case 0x2e: set_hi(&cp->lowm, v); cp_watermarks(cp); break;
    case 0x30: set_lo(&cp->distance, v); break;
    case 0x32: set_hi(&cp->distance, v); break;
    case 0x34: set_lo(&cp->wptr, v); break;
    case 0x36: set_hi(&cp->wptr, v); break;
    case 0x38: set_lo(&cp->rptr, v); break;
    case 0x3a: set_hi(&cp->rptr, v); break;
    case 0x3c: set_lo(&cp->bp, v); cp_drain(cp); break;
    case 0x3e: set_hi(&cp->bp, v); cp_drain(cp); break;
    default: break;
    }
}
