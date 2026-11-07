#!/usr/bin/env python3
"""Execute emitted timebase reads and DEC writes against the HAL contract."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

PORT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PORT))
from tools.gekko.decode import decode
from tools.gekko.lift import GEN_PREFIX_H, Lifter


def emit(spr, write):
    xo = 467 if write else 339
    word = (31 << 26) | (3 << 21) | ((spr & 31) << 16) | ((spr >> 5) << 11) | (xo << 1)
    insn = decode(word)
    assert insn.name == ('mtspr' if write else 'mfspr') and insn.spr == spr
    lifter = Lifter([], [], 0)
    lifter.out = []
    lifter.emit(insn, 0x80003100)
    return 'static void op_%d(void) {\n%s\n}\n' % (spr, '\n'.join(lifter.out))


def block_fixture(work):
    # Real output pipeline, unequal branch arms, return terminators, and a loop.
    base = 0x80004000
    words = (0x38630001, 0x2c030001, 0x4182000c,
             0x38840001, 0x4e800020, 0x38840002, 0x38a50003, 0x4e800020)
    blob = b''.join(w.to_bytes(4, 'big') for w in words)
    idle_pc = 0x802c7ae4
    idle = (0x800d9c38, 0x28000000, 0x4182fff8, 0x4e800020)
    idle_blob = b''.join(w.to_bytes(4, 'big') for w in idle)
    lifter = Lifter([(base, blob), (idle_pc, idle_blob)], [], base)
    lifter.todo.extend(((base, True), (idle_pc, True)))
    lifter._drain()
    out = work / 'emitted'
    out.mkdir()
    lifter.write_outputs(str(out))
    prefix = GEN_PREFIX_H.replace('"../hal/', '"' + str(PORT / 'hal') + '/')
    generated = '\n'.join('\n'.join(line for line in p.read_text().splitlines()
                                    if not line.startswith('#include'))
                          for p in sorted((out / 'generated').glob('*.c')))
    declarations = (out / 'block_decls.h').read_text()
    # Fast-forward is bound to exact instructions and original PC, not shape alone.
    idle_block = lifter.blocks[idle_pc]
    assert 'hal_scheduler_idle();' in lifter.render_term(idle_block)
    original = lifter.word_at
    lifter.word_at = lambda pc: (original(pc) ^ 1) if pc == idle_pc else original(pc)
    assert 'hal_scheduler_idle();' not in lifter.render_term(idle_block)
    lifter.word_at = original
    idle_block.addr += 4
    assert 'hal_scheduler_idle();' not in lifter.render_term(idle_block)
    return prefix + declarations + '''
#include <assert.h>
GcCtx gc;
uint32_t gc_spr_misc[1024];
uint8_t gc_ram[0x1800000];
static unsigned cycles, polls, irq_once, idle_calls, returns;
static uint32_t last_target;
uint32_t hal_irq_poll_cycles(uint32_t c) {
    assert(c==0); ++polls;
    if (irq_once) { irq_once=0; return 0x80000500u; }
    return 0;
}
void hal_clock_advance(uint32_t c) { cycles+=c; }
void hal_scheduler_idle(void) {
    ++idle_calls;
    gc_ram[0x3cfd5b]=1; /* next real loop load observes a runnable thread */
}
void gc_dispatch(uint32_t target) { last_target=target; ++returns; }
uint32_t hal_mmio_read32(uint32_t p) { (void)p; abort(); }
uint32_t hal_slow_read32(uint32_t p) { (void)p; abort(); }
''' + generated + '''
int main(void) {
    gc.lr=0x80005000u;
    b_80004000();
    assert(gc.r[3]==1 && gc.r[4]==2 && gc.r[5]==3);
    assert(cycles==6 && gc.icount==6 && polls==2 && returns==1);
    assert(last_target==gc.lr);
    cycles=polls=returns=0; gc.icount=0; gc.r[3]=1; gc.r[4]=gc.r[5]=0;
    b_80004000();
    assert(gc.r[3]==2 && gc.r[4]==1 && gc.r[5]==0);
    assert(cycles==5 && gc.icount==5 && polls==2);
    cycles=polls=returns=0; gc.icount=0; irq_once=1; gc.r[3]=gc.r[4]=gc.r[5]=0;
    b_80004000();
    assert(last_target==0x80000500u && gc.r[3]==0 && cycles==0 && gc.icount==0);
    b_80004000();
    assert(cycles==6 && gc.icount==6 && gc.r[3]==1 && polls==3);
    cycles=polls=returns=0; gc.icount=0; gc.r[13]=0x803d6120u;
    gc.r[0]=0xdeadbeefu; gc.cr=0x0abcdef0u;
    b_802c7ae4();
    assert(idle_calls==1 && cycles==7 && gc.icount==7 && polls==3);
    assert(gc.r[0]==1 && (gc.cr>>28)==4 && (gc.cr&0xfffffff)==0xabcdef0u);
    b_802c7ae4();
    assert(idle_calls==1 && cycles==11); /* non-taken arm cannot skip time */
    return 0;
}
'''


def main():
    prefix = GEN_PREFIX_H.replace('"../hal/', '"' + str(PORT / 'hal') + '/')
    source = prefix + '''
#include <assert.h>
GcCtx gc;
uint32_t gc_spr_misc[1024];
static uint64_t ticks;
static unsigned writes;
static uint32_t old_dec, input_dec;
uint64_t hal_timebase_ticks(void) { return ticks; }
void hal_decrementer_write(uint32_t value) {
    ++writes; old_dec=gc_spr_misc[22]; input_dec=value; gc_spr_misc[22]=value;
}
''' + emit(268, False) + emit(269, False) + emit(22, True) + '''
int main(void) {
    gc.icount=0xabcdef0012345678ull;
    ticks=0x0123456789abcdefull;
    op_268(); assert(gc.r[3]==0x89abcdefu);
    op_269(); assert(gc.r[3]==0x01234567u);
    ticks=0x100000000ull;
    op_268(); assert(gc.r[3]==0);
    op_269(); assert(gc.r[3]==1);
    gc_spr_misc[22]=0x12345678u; gc.r[3]=0x87654321u;
    op_22(); assert(writes==1 && old_dec==0x12345678u && input_dec==0x87654321u);
    assert(gc.r[3]==0x87654321u && gc_spr_misc[22]==0x87654321u);
    return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix='digimon-clock-emitter-') as work:
        c = Path(work) / 'test.c'
        c.write_text(source)
        for flags in (('-O0',), ('-O2',), ('-O1', '-fsanitize=address,undefined', '-fno-sanitize-recover=all')):
            binary = Path(work) / 'test'
            subprocess.run([os.environ.get('CC', 'clang'), '-std=c11', *flags, str(c), '-lm', '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)
            print(' '.join(flags), 'PASS shared TB reads and DEC hook ordering')
        c.write_text(block_fixture(Path(work)))
        for flags in (('-O0',), ('-O2',), ('-O1', '-fsanitize=address,undefined', '-fno-sanitize-recover=all')):
            subprocess.run([os.environ.get('CC', 'clang'), '-std=c11', *flags, str(c), '-lm', '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)
            print(' '.join(flags), 'PASS original block instruction cycles, branch arms, IRQ retry, exact idle loop')


if __name__ == '__main__':
    main()
