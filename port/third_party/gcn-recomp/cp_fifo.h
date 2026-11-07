/* MIT License; Copyright (c) 2026 Ethan Nicholas. See LICENSE.
 * CP FIFO subset adapted from gcn-recomp/runtime/gx/fifo.cpp; see README.md.
 */
#ifndef GCN_RECOMP_CP_FIFO_H
#define GCN_RECOMP_CP_FIFO_H

#include <stdbool.h>
#include <stdint.h>

enum {
    CP_FIFO_CR_READ = 1,
    CP_FIFO_CR_BPEN = 2,
    CP_FIFO_CR_OVFINT = 4,
    CP_FIFO_CR_UNFINT = 8,
    CP_FIFO_CR_LINK = 16,
    CP_FIFO_CR_BPINT = 32
};

/* Consume the entire span synchronously, preserving parser partial commands in
 * the caller if necessary. Callbacks must not reenter or mutate this context.
 * start_phys is a MEM1 offset; the caller owns RAM and PI/write-gather logic.
 */
typedef void (*CpFifoConsume)(uint32_t start_phys, uint32_t len, void *user);
typedef void (*CpFifoIrq)(bool asserted, void *user);

typedef struct CpFifo {
    uint16_t sr, cr, clear;
    uint32_t base, end, hiwm, lowm, distance, wptr, rptr, bp;
    uint32_t pi_wptr; /* Last burst snapshot, updated even when unlinked. */
    uint32_t mem1_size; /* Contiguous physical RAM [0, mem1_size). */
    bool settled;
    CpFifoConsume consume;
    CpFifoIrq irq;
    void *user;
} CpFifo;

/* Initializes zeroed CP registers and reports a deasserted IRQ. A NULL consume
 * callback prevents draining; a NULL IRQ callback is allowed. MEM1 is limited
 * to the GameCube's 24 MiB even if a larger size is supplied.
 */
void cp_fifo_init(CpFifo *cp, uint32_t mem1_size,
                  CpFifoConsume consume, CpFifoIrq irq, void *user);
/* Register byte offsets, decoded through off & 0xff like upstream. */
uint16_t cp_fifo_read16(const CpFifo *cp, uint32_t off);
void cp_fifo_write16(CpFifo *cp, uint32_t off, uint16_t value);
/* Call after committing a 32-byte PI burst and advancing/wrapping PI's pointer.
 * Only this hook follows PI while LINK is set. It never writes PI or RAM.
 */
void cp_fifo_note_burst(CpFifo *cp, uint32_t pi_wptr);

#endif
