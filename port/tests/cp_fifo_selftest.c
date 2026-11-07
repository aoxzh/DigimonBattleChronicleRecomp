/* Standalone tests for the MIT gcn-recomp CP FIFO subset. No HAL dependency. */
#include "../third_party/gcn-recomp/cp_fifo.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define MEM1_SIZE UINT32_C(0x01800000)

typedef struct Capture {
    uint32_t starts[8], lengths[8], bytes;
    unsigned count, irq_calls;
    bool irq;
} Capture;

static void consume(uint32_t start, uint32_t len, void *user)
{
    Capture *c = user;
    assert(c->count < 8);
    assert(len > 0 && start < MEM1_SIZE && len <= MEM1_SIZE - start);
    c->starts[c->count] = start;
    c->lengths[c->count++] = len;
    c->bytes += len;
}

static void irq(bool asserted, void *user)
{
    Capture *c = user;
    c->irq = asserted;
    c->irq_calls++;
}

static void init(CpFifo *cp, Capture *c)
{
    memset(c, 0, sizeof(*c));
    cp_fifo_init(cp, MEM1_SIZE, consume, irq, c);
    assert(!c->irq && c->irq_calls == 1);
    assert(cp_fifo_read16(cp, 0) == 12);
}

static void reg32(CpFifo *cp, uint32_t off, uint32_t v)
{
    cp_fifo_write16(cp, off, (uint16_t)v);
    cp_fifo_write16(cp, off + 2, (uint16_t)(v >> 16));
}

static void ring(CpFifo *cp, uint32_t base, uint32_t end,
                 uint32_t rptr, uint32_t wptr)
{
    reg32(cp, 0x20, base);
    reg32(cp, 0x24, end);
    reg32(cp, 0x38, rptr);
    reg32(cp, 0x34, wptr);
}

static void independent_fifos(void)
{
    CpFifo cp;
    Capture c;
    init(&cp, &c);
    ring(&cp, 0x1000, 0x10e0, 0x1000, 0x1000);
    cp_fifo_write16(&cp, 2, CP_FIFO_CR_LINK);
    cp_fifo_note_burst(&cp, 0x1020);
    assert(cp.wptr == 0x1020 && cp.rptr == 0x1000 && c.count == 0);
    /* Exact GXSetGPFifo regression: program the GP's other FIFO while LINK
     * remains set, then clear LINK and enable READ. PI still points at 0x1020.
     */
    ring(&cp, 0x2000, 0x20e0, 0x2000, 0x2060);
    cp_fifo_write16(&cp, 2, CP_FIFO_CR_READ);
    assert(cp.wptr == 0x2060 && cp.pi_wptr == 0x1020);
    assert(c.count == 1 && c.starts[0] == 0x2000 && c.lengths[0] == 0x60);
    assert(cp.rptr == 0x2060 && cp.distance == 0 && cp.settled);
    cp_fifo_note_burst(&cp, 0x1040);
    assert(cp.pi_wptr == 0x1040 && cp.wptr == 0x2060 && c.count == 1);
    puts("PASS independent CPU/GP FIFOs and LINK-clear register regression");
}

static void backlog_and_wrap(void)
{
    CpFifo cp;
    Capture c;
    init(&cp, &c);
    ring(&cp, 0x1000, 0x10ff, 0x10c0, 0x10c0);
    assert(cp.end == 0x10e0);
    cp_fifo_write16(&cp, 2, CP_FIFO_CR_LINK);
    cp_fifo_note_burst(&cp, 0x04001040); /* PI wrap bit is not a RAM address. */
    assert(c.count == 0 && cp.distance == 0x80);
    assert((cp_fifo_read16(&cp, 0) & 12) == 8);
    cp_fifo_write16(&cp, 2, CP_FIFO_CR_READ | CP_FIFO_CR_LINK);
    assert(c.count == 2 && c.bytes == 0x80);
    assert(c.starts[0] == 0x10c0 && c.lengths[0] == 0x40);
    assert(c.starts[1] == 0x1000 && c.lengths[1] == 0x40);
    assert(cp.rptr == 0x1040 && cp.distance == 0);
    assert((cp_fifo_read16(&cp, 0) & 12) == 12);
    /* A single queued final line wraps after that line, not before it. */
    init(&cp, &c);
    ring(&cp, 0x1000, 0x10e0, 0x10e0, 0x1000);
    cp_fifo_write16(&cp, 2, CP_FIFO_CR_READ);
    assert(c.count == 1 && c.starts[0] == 0x10e0 && c.lengths[0] == 32);
    assert(cp.rptr == 0x1000);
    puts("PASS READ-disabled backlog and last-line-inclusive two-span wrap");
}

static void partial_registers(void)
{
    CpFifo cp;
    Capture c;
    init(&cp, &c);
    const uint32_t offsets[] = {0x20, 0x24, 0x28, 0x2c, 0x30, 0x34, 0x38, 0x3c};
    for (unsigned i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
        uint32_t off = offsets[i];
        bool bounds = off == 0x20 || off == 0x24;
        reg32(&cp, off, 0x01234560);
        assert(cp_fifo_read16(&cp, off + 2) == 0x0123);
        cp_fifo_write16(&cp, off, 0xabe7);
        assert(cp_fifo_read16(&cp, off + 2) == 0x0123);
        assert(cp_fifo_read16(&cp, off) == (bounds ? 0xabe0 : 0xabe7));
        cp_fifo_write16(&cp, off + 2, 0xf456);
        assert(cp_fifo_read16(&cp, off) == (bounds ? 0xabe0 : 0xabe7));
        assert(cp_fifo_read16(&cp, off + 2) == (bounds ? 0x0056 : 0xf456));
        assert(cp_fifo_read16(&cp, off + 0x100) == cp_fifo_read16(&cp, off));
    }
    assert(cp_fifo_read16(&cp, 0x80) == 0);
    assert(c.count == 0);
    /* Raw pointer halves read back unchanged; address arithmetic masks them. */
    init(&cp, &c);
    ring(&cp, 0x1000, 0x10e0, 0x8000101f, 0x8400105f);
    cp_fifo_write16(&cp, 2, CP_FIFO_CR_READ);
    assert(c.count == 1 && c.starts[0] == 0x1000 && c.lengths[0] == 0x40);
    assert(cp.rptr == 0x1040);
    puts("PASS all register half merges, bounds masks and effective pointer masks");
}

static void breakpoints(void)
{
    CpFifo cp;
    Capture c;
    init(&cp, &c);
    ring(&cp, 0x1000, 0x10e0, 0x1000, 0x1080);
    reg32(&cp, 0x3c, 0x1040);
    cp_fifo_write16(&cp, 2, CP_FIFO_CR_READ | CP_FIFO_CR_BPEN | CP_FIFO_CR_BPINT);
    assert(c.count == 1 && c.bytes == 0x40 && cp.rptr == 0x1040);
    assert(cp.distance == 0x40 && (cp.sr & 0x10) && c.irq);
    assert((cp_fifo_read16(&cp, 0) & 12) == 8);
    cp_fifo_note_burst(&cp, 0x2000); /* Unlinked PI cannot redirect the GP. */
    assert(c.count == 1 && cp.wptr == 0x1080);
    cp_fifo_write16(&cp, 0x3c, 0x1060); /* Move a live breakpoint. */
    assert(c.count == 2 && c.lengths[1] == 0x20 && cp.rptr == 0x1060 && c.irq);
    cp_fifo_write16(&cp, 2, CP_FIFO_CR_READ | CP_FIFO_CR_BPINT);
    assert(c.count == 3 && c.lengths[2] == 0x20 && cp.rptr == 0x1080);
    assert(!(cp.sr & 0x10) && !c.irq && cp.distance == 0);
    /* Moving the high half also resumes a stopped GP. */
    init(&cp, &c);
    ring(&cp, 0x1000, 0x10e0, 0x1000, 0x1040);
    reg32(&cp, 0x3c, 0x1000);
    cp_fifo_write16(&cp, 2, CP_FIFO_CR_READ | CP_FIFO_CR_BPEN | CP_FIFO_CR_BPINT);
    assert(c.count == 0 && c.irq);
    cp_fifo_write16(&cp, 0x3e, 1);
    assert(c.bytes == 0x40 && !c.irq && !(cp.sr & 0x10));
    puts("PASS breakpoint halt, IRQ, idle bits, move and disable resume");
}

static void watermarks(void)
{
    CpFifo cp, other;
    Capture c, other_c;
    init(&cp, &c);
    init(&other, &other_c);
    ring(&cp, 0x1000, 0x10e0, 0x1000, 0x1000);
    reg32(&cp, 0x28, 0x40);
    reg32(&cp, 0x2c, 0x20);
    cp_fifo_write16(&cp, 2, CP_FIFO_CR_LINK | CP_FIFO_CR_OVFINT);
    cp_fifo_note_burst(&cp, 0x1040);
    assert(cp.distance == 0x40 && !(cp.sr & 1) && !c.irq); /* strict > */
    cp_fifo_note_burst(&cp, 0x1060);
    assert(cp.distance == 0x60 && (cp.sr & 1) && c.irq);
    cp_fifo_write16(&cp, 4, 1);
    assert((cp.sr & 1) && c.irq); /* Reassert while condition persists. */
    cp_fifo_write16(&cp, 2, CP_FIFO_CR_READ | CP_FIFO_CR_LINK | CP_FIFO_CR_UNFINT);
    assert(cp.distance == 0 && (cp.sr & 3) == 3 && c.irq);
    cp_fifo_write16(&cp, 2, CP_FIFO_CR_LINK); /* Disable interrupt enables only. */
    assert((cp.sr & 3) == 3 && !c.irq);
    cp_fifo_note_burst(&cp, 0x1080);
    assert(cp.distance == 0x20);
    cp_fifo_write16(&cp, 4, 3);
    assert(!(cp.sr & 3) && cp.clear == 3 && !c.irq); /* strict < */
    cp_fifo_write16(&cp, 2, CP_FIFO_CR_LINK | CP_FIFO_CR_UNFINT);
    cp_fifo_note_burst(&cp, 0x1060);
    assert(cp.distance == 0 && (cp.sr & 2) && c.irq);
    cp_fifo_write16(&cp, 2, CP_FIFO_CR_UNFINT); /* Unlinked CLEAR updates IRQ. */
    cp_fifo_write16(&cp, 4, 2);
    assert(!(cp.sr & 2) && !c.irq);
    cp_fifo_note_burst(&cp, 0x10c0);
    assert(cp.wptr == 0x1060 && cp.distance == 0 && !(cp.sr & 3));
    assert(other.sr == 0 && other.wptr == 0 && !other_c.irq);
    /* A CTRL write while LINK remains set must use CP, not stale PI, distance. */
    reg32(&cp, 0x34, 0x10a0);
    cp_fifo_write16(&cp, 2, CP_FIFO_CR_LINK | CP_FIFO_CR_OVFINT);
    cp_fifo_write16(&cp, 4, 3);
    assert(cp.wptr == 0x10a0 && cp.distance == 0x40 && !(cp.sr & 3));
    puts("PASS latched watermarks, strict thresholds, IRQ enables and context independence");
}

static void invalid_and_bounded(void)
{
    CpFifo cp;
    Capture c;
    init(&cp, &c);
    ring(&cp, MEM1_SIZE - 32, MEM1_SIZE + 32, MEM1_SIZE - 32, MEM1_SIZE);
    cp_fifo_write16(&cp, 2, CP_FIFO_CR_READ);
    assert(c.count == 0 && cp.rptr == MEM1_SIZE - 32);
    init(&cp, &c);
    ring(&cp, 0x02001000, 0x020010e0, 0x02001000, 0x02001040);
    cp_fifo_write16(&cp, 2, CP_FIFO_CR_READ);
    assert(c.count == 0); /* No folding physical addresses into MEM1. */
    init(&cp, &c);
    ring(&cp, 0x1000, 0x10e0, 0x1000, 0x2000);
    cp_fifo_write16(&cp, 2, CP_FIFO_CR_READ);
    assert(c.count == 0); /* Valid RAM but another FIFO's pointer. */
    init(&cp, &c);
    ring(&cp, 0x1000, 0x0fe0, 0x1000, 0x1040);
    cp_fifo_write16(&cp, 2, CP_FIFO_CR_READ);
    assert(c.count == 0); /* Empty/invalid bounds. */
    init(&cp, &c);
    ring(&cp, 0x1000, 0, 0x1000, 0x1040);
    cp_fifo_write16(&cp, 2, CP_FIFO_CR_READ);
    assert(c.count == 0); /* Unset end. */
    init(&cp, &c);
    cp.mem1_size = 0x1080;
    ring(&cp, 0x1000, 0x10e0, 0x1000, 0x1040);
    cp_fifo_write16(&cp, 2, CP_FIFO_CR_READ);
    assert(c.count == 0); /* Caller-provided RAM extent is also enforced. */
    init(&cp, &c);
    ring(&cp, MEM1_SIZE - 0x100, MEM1_SIZE - 32, MEM1_SIZE - 32, MEM1_SIZE - 0x100);
    cp_fifo_write16(&cp, 2, CP_FIFO_CR_READ);
    assert(c.count == 1 && c.bytes == 32); /* Exact MEM1 end is valid. */
    init(&cp, &c);
    ring(&cp, 0x1000, 0x10e0, 0, 0x1040);
    cp_fifo_write16(&cp, 2, CP_FIFO_CR_READ);
    assert(c.count == 1 && c.starts[0] == 0x1000); /* Upstream rptr clamp. */
    init(&cp, &c);
    ring(&cp, 0x1000, 0x10e0, 0x1000, 0x1000);
    cp_fifo_write16(&cp, 2, CP_FIFO_CR_READ);
    for (unsigned i = 0; i < 1000; i++) cp_fifo_note_burst(&cp, 0x1040);
    assert(c.count == 0 && cp.settled); /* Equal pointers never invent a full ring. */
    cp_fifo_init(&cp, MEM1_SIZE, NULL, NULL, NULL);
    ring(&cp, 0x1000, 0x10e0, 0x1000, 0x1040);
    cp_fifo_write16(&cp, 2, CP_FIFO_CR_READ);
    assert(cp.rptr == 0x1000);
    puts("PASS MEM1 guards, invalid bounds, foreign pointers and bounded empty FIFO");
}

int main(void)
{
    independent_fifos();
    backlog_and_wrap();
    partial_registers();
    breakpoints();
    watermarks();
    invalid_and_bounded();
    puts("CP FIFO selftest: all checks passed");
    return 0;
}
