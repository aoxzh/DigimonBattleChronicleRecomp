#!/usr/bin/env python3
"""Execute freshly emitted mixed scalar/paired FP and the game's matrix inverse.

The optional local DOL supplies the original 0x802b3464 matrix routine. No game
code is copied into this test, and generated recomp files are never used.
"""
import math
import os
from pathlib import Path
import shlex
import struct
import subprocess
import sys
import tempfile

from test_lift_arithmetic import PORT, compile_c
from tools.gekko.decode import decode
from tools.gekko.lift import GEN_PREFIX_H, Lifter
from tools.lift_dol import parse_dol


def d(op, reg, base, off=0):
    return op << 26 | reg << 21 | base << 16 | (off & 0xffff)


def a(op, dst, srca=0, srcb=0, srcc=0, xo=0):
    return op << 26 | dst << 21 | srca << 16 | srcb << 11 | srcc << 6 | xo << 1


def emit(name, words):
    lifter = Lifter([], [], 0)
    lifter.out = []
    for n, word in enumerate(words):
        insn = decode(word)
        assert insn is not None and lifter.emit(insn, 0x80003100 + 4*n) is None
    return f"static void {name}(void) {{\n" + "\n".join(lifter.out) + "\n}\n"


def actual_inverse(dol):
    _, _, _, text, _ = parse_dol(dol.read_bytes())
    lifter = Lifter(text, [], 0)
    lifter.out = []
    for pc in range(0x802b3464, 0x802b3558, 4):
        if pc == 0x802b34cc:
            # Original bne skips the singular-matrix early return.
            assert lifter.word_at(pc) == 0x4082000c
            lifter.out.append("if (gc.cr & 0x20000000u) { gc.r[3] = 0; return; }")
        elif pc not in (0x802b34d0, 0x802b34d4):
            insn = decode(lifter.word_at(pc))
            assert lifter.emit(insn, pc) is None
    return "static void game_inverse(void) {\n" + "\n".join(lifter.out) + "\n}\n"


SHIM = r'''
GcCtx gc;
uint8_t gc_ram[0x01800000];
void gc_watch_note(uint32_t p, uint32_t v) { (void)p; (void)v; }
#define READ_STUB(bits, type) \
 type hal_slow_read##bits(uint32_t p) { abort(); return 0; } \
 type hal_mmio_read##bits(uint32_t p) { abort(); return 0; }
#define WRITE_STUB(bits, type) \
 void hal_slow_write##bits(uint32_t p, type v) { abort(); } \
 void hal_mmio_write##bits(uint32_t p, type v) { abort(); }
READ_STUB(8, uint8_t) READ_STUB(16, uint16_t) READ_STUB(32, uint32_t)
WRITE_STUB(8, uint8_t) WRITE_STUB(16, uint16_t) WRITE_STUB(32, uint32_t)
uint64_t hal_slow_read64(uint32_t p) { abort(); return 0; }
void hal_slow_write64(uint32_t p, uint64_t v) { abort(); }
static unsigned checks, failures;
static void expect_float(const char *name, unsigned offset, double expected) {
    float actual = g_bf32(g_pread32(0x1200 + offset));
    ++checks;
    if (!isfinite(actual) || fabs((double)actual - expected) > 0.00005 * (1.0 + fabs(expected))) {
        if (failures < 10) fprintf(stderr, "%s offset=%u actual=%g expected=%.9g\n", name, offset, actual, expected);
        ++failures;
    }
}
static void reset(void) {
    memset(&gc, 0, sizeof(gc)); memset(gc_ram, 0, 0x2000);
    gc.r[3] = 0x1000; gc.r[4] = 0x1100; gc.r[5] = 0x1200;
}
'''


def harness(dol):
    prefix = GEN_PREFIX_H.replace('"../hal/', '"' + str(PORT / 'hal') + '/')
    funcs = [
        emit('lfs_pair', [d(48, 0, 3), a(4, 1, 0, 0, 0, 21), d(60, 1, 5)]),
        emit('pair_fres', [d(56, 7, 3), a(59, 0, srcb=7, xo=24),
                           a(4, 6, 0, 0, 0, 21), d(60, 6, 5), d(52, 0, 5, 8)]),
        emit('single_pair', [d(48, 0, 3), d(48, 1, 3, 4),
                             a(59, 2, 0, 1, xo=21), d(60, 2, 5)]),
        emit('double_pair', [d(56, 0, 3), d(50, 0, 4),
                             a(4, 1, 0, 0, 0, 21), d(60, 1, 5)]),
        emit('pair_scalar', [d(56, 0, 3), d(52, 0, 5), d(54, 0, 5, 8)]),
        emit('move_pair', [d(56, 0, 3), d(56, 1, 3, 8),
                           a(63, 1, srcb=0, xo=72), d(60, 1, 5)]),
        emit('double_add_pair', [d(56, 0, 3), d(56, 1, 3, 8),
                                 a(63, 1, 0, 1, xo=21), d(60, 1, 5)]),
        emit('round_pair', [d(56, 0, 3), d(50, 0, 4),
                            a(63, 0, srcb=0, xo=12), d(60, 0, 5)]),
        emit('merge_alias', [a(4, 0, 0, 0, xo=592)]),
        emit('select_alias', [a(4, 0, 1, 0, 2, 23)]),
        emit('pair_neg', [a(4, 0, srcb=0, xo=40)]),
        emit('pair_abs', [a(4, 0, srcb=0, xo=264)]),
        emit('pair_nabs', [a(4, 0, srcb=0, xo=136)]),
        emit('pair_mr', [a(4, 1, srcb=0, xo=72)]),
        # lfd supplies a true double; merge00 copies it into PS1 unchanged.
        # Both ps_sum lanes must then round to single, including the FRc bypass.
        emit('sum0_round', [d(50, 0, 3), a(4, 0, 0, 0, xo=528),
                            a(4, 0, 1, 2, 0, 10)]),
        emit('sum1_round', [d(50, 0, 3), a(4, 0, 1, 2, 0, 11)]),
    ]
    body = r'''
int main(void) {
    reset(); g_pwrite32(0x1000, 0x3fc00000u); lfs_pair();
    expect_float("lfs broadcasts both lanes", 0, 3); expect_float("lfs broadcasts both lanes", 4, 3);
    reset(); g_pwrite32(0x1000, 0x40800000u); g_pwrite32(0x1004, 0x41100000u); pair_fres();
    expect_float("paired -> fres -> paired", 0, .5); expect_float("fres broadcasts", 4, .5);
    expect_float("fres -> stfs", 8, .25);
    reset(); g_pwrite32(0x1000, 0x3fc00000u); g_pwrite32(0x1004, 0x40100000u); single_pair();
    expect_float("fadds -> psq_st", 0, 3.75); expect_float("fadds broadcasts", 4, 3.75);
    reset(); g_pwrite32(0x1000, 0x40800000u); g_pwrite32(0x1004, 0x41100000u);
    g_pwrite64(0x1100, 0x4000000000000000ull); double_pair();
    expect_float("lfd -> paired lane0", 0, 4); expect_float("lfd preserves lane1", 4, 18);
    reset(); g_pwrite32(0x1000, 0x3fc00000u); g_pwrite32(0x1004, 0xc0100000u); pair_scalar();
    expect_float("psq_l -> stfs", 0, 1.5); ++checks;
    if (g_pread64(0x1208) != 0x3ff8000000000000ull) { ++failures; fprintf(stderr, "psq_l -> stfd is not double 1.5\n"); }
    reset(); g_pwrite32(0x1000, 0x3fc00000u); g_pwrite32(0x1004, 0x40000000u);
    g_pwrite32(0x1008, 0x40800000u); g_pwrite32(0x100c, 0x41100000u); move_pair();
    expect_float("fmr changes lane0", 0, 1.5); expect_float("fmr preserves lane1", 4, 9);
    reset(); g_pwrite32(0x1000, 0x3fc00000u); g_pwrite32(0x1004, 0x40000000u);
    g_pwrite32(0x1008, 0x40800000u); g_pwrite32(0x100c, 0x41100000u); double_add_pair();
    expect_float("fadd changes lane0", 0, 5.5); expect_float("fadd preserves lane1", 4, 9);
    reset(); g_pwrite32(0x1000, 0x40800000u); g_pwrite32(0x1004, 0x41100000u);
    g_pwrite64(0x1100, 0x3ff0000000000001ull); round_pair();
    expect_float("frsp rounds PS0", 0, 1); expect_float("frsp broadcasts PS1", 4, 1);
    /* A lane move must preserve double precision and work with a swapped alias. */
    reset(); gc.fpr_bits[0] = 0x3ff0000000000001ull;
    gc.fpr_ps1_bits[0] = 0xc000000000000001ull; merge_alias(); ++checks;
    if (gc.fpr_bits[0] != 0xc000000000000001ull || gc.fpr_ps1_bits[0] != 0x3ff0000000000001ull) ++failures;
    gc.fpr_bits[1] = g_fbr(-1); gc.fpr_ps1_bits[1] = g_fbr(1);
    gc.fpr_bits[2] = g_fbr(4); gc.fpr_ps1_bits[2] = 0x4008000000000001ull;
    select_alias(); ++checks;
    if (gc.fpr_bits[0] != 0xc000000000000001ull || gc.fpr_ps1_bits[0] != 0x4008000000000001ull) ++failures;
    pair_neg(); ++checks;
    if (gc.fpr_bits[0] != 0x4000000000000001ull || gc.fpr_ps1_bits[0] != 0xc008000000000001ull) ++failures;
    pair_abs(); ++checks;
    if (gc.fpr_bits[0] != 0x4000000000000001ull || gc.fpr_ps1_bits[0] != 0x4008000000000001ull) ++failures;
    pair_nabs(); pair_mr(); ++checks;
    if (gc.fpr_bits[1] != 0xc000000000000001ull || gc.fpr_ps1_bits[1] != 0xc008000000000001ull) ++failures;
    reset(); g_pwrite64(0x1000, 0x3ff0000000000001ull);
    gc.fpr_bits[1] = g_fbr(2); gc.fpr_ps1_bits[2] = g_fbr(3);
    sum0_round(); ++checks;
    if (gc.fpr_bits[0] != g_fbr(5) || gc.fpr_ps1_bits[0] != 0x3ff0000000000000ull) {
        ++failures; fprintf(stderr, "ps_sum0 bypass must round to single\n");
    }
    sum1_round(); ++checks;
    if (gc.fpr_bits[0] != 0x3ff0000000000000ull || gc.fpr_ps1_bits[0] != g_fbr(5)) {
        ++failures; fprintf(stderr, "ps_sum1 bypass must round to single\n");
    }
'''
    if dol.exists():
        funcs.append(actual_inverse(dol))
        for index in range(32):
            theta = index * math.pi / 16
            c, s = math.cos(theta), math.sin(theta)
            sx, sy, sz = .25 + index / 8, .5 + index / 16, 2
            tx, ty, tz = 3, -2, 5
            src = [c*sx, -s*sy, 0, tx, s*sx, c*sy, 0, ty, 0, 0, sz, tz]
            inv = [c/sx, s/sx, 0, -(c*tx+s*ty)/sx,
                   -s/sy, c/sy, 0, (s*tx-c*ty)/sy,
                   0, 0, 1/sz, -tz/sz]
            body += 'reset(); gc.r[4] = 0x1200;\n'
            for n, value in enumerate(src):
                bits = struct.unpack('>I', struct.pack('>f', value))[0]
                body += f'g_pwrite32(0x{0x1000+4*n:x}, 0x{bits:08x}u);\n'
            body += 'game_inverse(); ++checks; if (gc.r[3] != 1) ++failures;\n'
            for n, value in enumerate(inv):
                body += f'expect_float("game inverse {index}", {4*n}, {value:.17g});\n'
        body += 'reset(); gc.r[4] = 0x1200; game_inverse(); ++checks; if (gc.r[3] != 0) ++failures;\n'
    body += 'printf("FP: %u checks, %u failures\\n", checks, failures); return failures ? 1 : 0; }\n'
    return prefix + SHIM + ''.join(funcs) + body


def main():
    cc = shlex.split(os.environ.get('CC', 'clang'))
    dol = PORT / 'extracted/main.dol'
    if not dol.exists():
        print('SKIP actual game matrix routine: extracted/main.dol unavailable')
    modes = (('-O0', ['-O0']), ('-O2', ['-O2']),
             ('ASan+UBSan', ['-O1', '-g', '-fsanitize=address,undefined',
                            '-fno-sanitize-recover=all', '-fno-omit-frame-pointer']))
    with tempfile.TemporaryDirectory(prefix='gekko-fp-') as temporary:
        root = Path(temporary)
        source = root / 'fp.c'
        source.write_text(harness(dol))
        for label, flags in modes:
            binary = root / ('fp_' + label + ('.exe' if os.name == 'nt' else ''))
            compile_c(cc, source, binary, flags)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
            print(label, result.stdout.strip(), flush=True)
            if result.returncode:
                raise RuntimeError(result.stderr)


if __name__ == '__main__':
    try:
        main()
    except (subprocess.CalledProcessError, RuntimeError) as error:
        print(str(error), getattr(error, 'stderr', ''), file=sys.stderr)
        sys.exit(1)
