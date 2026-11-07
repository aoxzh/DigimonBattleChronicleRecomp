"""test_gekko.py — decoder unit tests for the Gekko lifter.

Hand-encoded instructions (cross-checked against well-known assembler
constants and Dolphin/binutils opcode tables).  At least three assertions
per major instruction class, per the M0 deliverable spec.

Run:  python -m pytest tests/test_gekko.py   (or python tests/test_gekko.py)
"""

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(os.path.dirname(HERE)))

from tools.gekko.decode import (decode, ppc_mask, sx, gqr_ld_type,     # noqa
                                gqr_ld_scale, gqr_st_type, gqr_st_scale,
                                GEKKO_PVR)


# ---------------------------------------------------------------- encoders
def enc_d(op, d, a, imm):
    return (op << 26) | (d << 21) | (a << 16) | (imm & 0xFFFF)


def enc_x(op, d, a, b, xo, rc=0):
    return (op << 26) | (d << 21) | (a << 16) | (b << 11) | (xo << 1) | rc


def enc_a(op, d, a, b, c, xo, rc=0):
    return (op << 26) | (d << 21) | (a << 16) | (b << 11) | (c << 6) | \
        (xo << 1) | rc


def spr_bits(spr):
    """mfspr/mtspr split-field encoding (5-bit halves swapped)."""
    return ((spr & 31) << 16) | ((spr >> 5) << 11)


# ---------------------------------------------------------------- D-form ALU
def test_dform_integer():
    i = decode(0x38600005)                       # addi r3, r0, 5
    assert i.name == "addi" and i.d == 3 and i.a == 0 and i.simm == 5
    i = decode(0x3C608000)                       # lis r3, 0x8000 (addis)
    assert i.name == "addis" and i.d == 3 and i.simm == -32768
    assert (i.simm & 0xFFFF) == 0x8000
    i = decode(enc_d(8, 6, 7, 0x10))             # subfic r6, r7, 0x10
    assert i.name == "subfic" and i.simm == 16
    i = decode(enc_d(12, 4, 5, 0xFFFF))          # addic r4, r5, -1
    assert i.name == "addic" and i.simm == -1 and i.rc == 0
    i = decode(enc_d(13, 4, 5, 1))               # addic. r4, r5, 1
    assert i.name == "addic" and i.rc == 1
    i = decode(enc_d(7, 8, 9, 3))                # mulli r8, r9, 3
    assert i.name == "mulli"
    i = decode(0x60000000)                       # ori r0, r0, 0 (nop)
    assert i.name == "ori" and i.d == 0 and i.uimm == 0
    i = decode(enc_d(28, 3, 4, 0xF))             # andi. r3, r4, 0xF
    assert i.name == "andi" and i.rc == 1


def test_dform_compare_trap():
    i = decode(0x2C03FFFF)                       # cmpwi r3, -1 (cmpi cr0)
    assert i.name == "cmpi" and i.crfd == 0 and i.simm == -1
    i = decode(enc_d(10, 0, 3, 5) | (5 << 23))   # cmpli cr5, r3, 5
    assert i.name == "cmpli" and i.crfd == 5 and i.uimm == 5
    i = decode(enc_d(3, 0x1F, 3, 0x100))         # twi (trap gt/ge/lt...)
    assert i.name == "twi" and i.d == 0x1F


# ---------------------------------------------------------------- branches
def test_branches():
    i = decode(0x48000008)                       # b .+8
    assert i.name == "b" and i.simm == 8 and not i.lk and not i.aa
    i = decode(0x48000001)                       # bl .+0
    assert i.name == "b" and i.lk == 1
    i = decode(0x41820010)                       # beq +0x10 (bo=12, bi=2)
    assert i.name == "bc" and i.bo == 12 and i.bi == 2 and i.simm == 16
    i = decode(0x4082FFF0)                       # bne -0x10 (bo=4, bi=2)
    assert i.name == "bc" and i.bo == 4 and i.simm == -16
    # bdnzf: bo=0b00000 decrement, ctr!=0, cr bit==0
    i = decode(enc_d(16, 0, 0, 0xFFFC))
    assert i.name == "bc" and i.bo == 0 and i.simm == -4
    i = decode(0x4E800020)                       # blr (bo=20 => always)
    assert i.name == "bclr" and i.lk == 0
    i = decode(0x4E800021)                       # blrl
    assert i.name == "bclr" and i.lk == 1
    i = decode(0x4E800420)                       # bctr
    assert i.name == "bcctr" and i.lk == 0
    i = decode(0x4E800421)                       # bctrl
    assert i.name == "bcctr" and i.lk == 1
    # conditional bclr: bo=4 bi=2 (beqlr-)
    i = decode((19 << 26) | (4 << 21) | (2 << 16) | (16 << 1))
    assert i.name == "bclr" and i.bo == 4 and i.bi == 2


def test_bc_target_math():
    # bc at 0x80004300 with displacement -0x40 -> 0x800042C0 (rel mode)
    w = enc_d(16, 12, 2, 0xFFC0)
    i = decode(w, 0x80004300)
    tgt = (i.simm & 0xFFFFFFFF) if i.aa else (0x80004300 + i.simm) & 0xFFFFFFFF
    assert tgt == 0x800042C0
    # AA=1 absolute
    i = decode(w | 2, 0x80004300)
    assert i.aa == 1
    assert (i.simm & 0xFFFFFFFF) == 0xFFFFFFC0


# ---------------------------------------------------------------- rotates
def test_rotates_and_mask():
    i = decode(0x5464283E)                       # rlwinm r3, r4, 5, 0, 31
    assert i.name == "rlwinm" and i.sh == 5 and i.mb == 0 and i.me == 31
    assert ppc_mask(0, 31) == 0xFFFFFFFF
    assert ppc_mask(24, 31) == 0xFF              # low byte
    assert ppc_mask(0, 7) == 0xFF000000          # high byte
    assert ppc_mask(28, 3) == 0xF000000F         # wrapped
    assert ppc_mask(5, 5) == 0x04000000          # single bit
    i = decode(enc_d(20, 3, 4, 0) | (5 << 11) | (24 << 6) | (31 << 1))
    assert i.name == "rlwimi" and i.sh == 5 and i.mb == 24 and i.me == 31
    i = decode(enc_d(23, 3, 4, 0) | (5 << 11) | (0 << 6) | (31 << 1))
    assert i.name == "rlwnm" and i.b == 5        # shift register


# ------------------------------------------------------- opcode 31 arith
def test_op31_arithmetic():
    i = decode(0x7C642BD6)                       # divw r3, r4, r5
    assert i.name == "divw" and i.d == 3 and i.a == 4 and i.b == 5
    i = decode(enc_x(31, 3, 4, 5, 266))          # add r3, r4, r5
    assert i.name == "add" and i.oe == 0 and i.rc == 0
    i = decode(enc_x(31, 3, 4, 5, 266, 1))       # add. r3, r4, r5
    assert i.name == "add" and i.rc == 1
    i = decode(enc_x(31, 3, 4, 5, 266 + 512))    # addo
    assert i.name == "add" and i.oe == 1
    i = decode(enc_x(31, 3, 4, 5, 10))           # addc
    assert i.name == "addc"
    i = decode(enc_x(31, 3, 4, 5, 10 + 512, 1))  # addco.
    assert i.name == "addc" and i.oe == 1 and i.rc == 1
    i = decode(enc_x(31, 3, 4, 5, 40))           # subf
    assert i.name == "subf"
    i = decode(enc_x(31, 3, 4, 0, 104))          # neg r3, r4
    assert i.name == "neg"
    i = decode(enc_x(31, 3, 4, 5, 235))          # mullw
    assert i.name == "mullw"
    i = decode(enc_x(31, 3, 4, 5, 491))          # divw
    assert i.name == "divw"
    i = decode(enc_x(31, 3, 4, 5, 459))          # divwu
    assert i.name == "divwu"
    # carry family
    for xo, nm in ((138, "adde"), (202, "addze"), (234, "addme"),
                   (8, "subfc"), (136, "subfe"), (200, "subfze"),
                   (232, "subfme")):
        i = decode(enc_x(31, 3, 4, 5, xo))
        assert i.name == nm, (nm, i.name)


def test_op31_arithmetic_ov_flags():
    i = decode(enc_x(31, 3, 4, 5, 491 + 512))    # divwo
    assert i.name == "divw" and i.oe == 1
    i = decode(enc_x(31, 3, 4, 5, 235 + 512))    # mullwo
    assert i.name == "mullw" and i.oe == 1
    i = decode(enc_x(31, 3, 4, 5, 75, 1))        # mulhw.
    assert i.name == "mulhw" and i.rc == 1
    i = decode(enc_x(31, 3, 4, 5, 11, 1))        # mulhwu.
    assert i.name == "mulhwu"


# ------------------------------------------------------- logic / shifts
def test_op31_logic_shift():
    i = decode(enc_x(31, 3, 4, 5, 28))           # and
    assert i.name == "and"
    i = decode(enc_x(31, 3, 4, 5, 444))          # or
    assert i.name == "or"
    i = decode(enc_x(31, 3, 4, 5, 284))          # eqv
    assert i.name == "eqv"
    i = decode(enc_x(31, 3, 4, 5, 476))          # nand
    assert i.name == "nand"
    i = decode(enc_x(31, 3, 0, 4, 26))           # cntlzw r3, r4
    assert i.name == "cntlzw" and i.a == 0 and i.b == 4
    i = decode(enc_x(31, 3, 0, 4, 954))          # extsb
    assert i.name == "extsb"
    i = decode(enc_x(31, 3, 0, 4, 922))          # extsh
    assert i.name == "extsh"
    # shifts: shift count comes from RB & 0x3F
    i = decode(enc_x(31, 3, 4, 5, 24))           # slw
    assert i.name == "slw"
    i = decode(enc_x(31, 3, 4, 5, 536))          # srw
    assert i.name == "srw"
    i = decode(enc_x(31, 3, 4, 5, 792))          # sraw
    assert i.name == "sraw"
    i = decode(enc_x(31, 3, 4, 0, 824) | (7 << 11))  # srawi r3, r4, 7
    assert i.name == "srawi" and i.sh == 7


# ------------------------------------------------------- CR operations
def test_cr_ops():
    i = decode((19 << 26) | (0 << 21) | (0 << 16) | (0 << 11) | (193 << 1))
    assert i.name == "crxor" and i.crbd == 0 and i.crba == 0 and i.crbb == 0
    i = decode((19 << 26) | (0 << 21) | (0 << 16) | (0 << 11) | (289 << 1))
    assert i.name == "creqv"                     # crset 0, 0, 0
    i = decode((19 << 26) | (4 << 21) | (0 << 16) | (0 << 11) | (449 << 1))
    assert i.name == "cror" and i.crbd == 4
    for xo, nm in ((257, "crand"), (129, "crandc"), (225, "crnand"),
                   (33, "crnor"), (417, "crorc")):
        i = decode((19 << 26) | (1 << 21) | (2 << 16) | (3 << 11) | (xo << 1))
        assert i.name == nm
    i = decode((19 << 26) | (2 << 23) | (5 << 18) | (0 << 1))   # mcrf cr2, cr5
    assert i.name == "mcrf" and i.crfd == 2 and i.crfs == 5
    # mtcrf 0xFF, r3
    i = decode((31 << 26) | (3 << 21) | (0xFF << 12) | (144 << 1))
    assert i.name == "mtcrf" and i.crm == 0xFF
    # mfcr r3
    i = decode(0x7C600026)                       # mfcr r3
    assert i.name == "mfcr" and i.d == 3
    # mcrxr cr3
    i = decode((31 << 26) | (3 << 23) | (512 << 1))
    assert i.name == "mcrxr" and i.crfd == 3


def test_cr_bit_positions():
    # BI 2 of CR field 0 == EQ bit; global bit index = 31 - bi
    i = decode(0x41820010)
    assert 31 - i.bi == 29                       # EQ of CR0 -> bit 29 of u32
    i = decode(0x40820010)                       # bne uses same bit, inverted
    assert 31 - i.bi == 29


# ------------------------------------------------------- loads / stores
def test_loads_stores_dform():
    i = decode(0x80610010)                       # lwz r3, 0x10(r1)
    assert i.name == "lwz" and i.d == 3 and i.a == 1 and i.simm == 0x10
    i = decode(0x90610008)                       # stw r3, 8(r1)
    assert i.name == "stw" and i.simm == 8
    i = decode(enc_d(34, 3, 4, -4))              # lbz r3, -4(r4)
    assert i.name == "lbz" and i.simm == -4
    i = decode(enc_d(42, 3, 4, 2))               # lha r3, 2(r4)
    assert i.name == "lha"
    i = decode(enc_d(44, 3, 4, 6))               # sth r3, 6(r4)
    assert i.name == "sth"
    i = decode(enc_d(33, 1, 3, -8))              # lwzu r1, -8(r3)
    assert i.name == "lwzu" and i.simm == -8
    i = decode(enc_d(46, 26, 3, 0))              # lmw r26, 0(r3)
    assert i.name == "lmw" and i.d == 26


def test_loads_stores_xform():
    i = decode(0x7C64282E)                       # lwzx r3, r4, r5
    assert i.name == "lwzx" and i.a == 4 and i.b == 5
    i = decode(enc_x(31, 3, 0, 5, 23))           # lwzx r3, 0, r5 (rA=0)
    assert i.name == "lwzx" and i.a == 0
    i = decode(enc_x(31, 3, 4, 5, 343))          # lhax
    assert i.name == "lhax"
    i = decode(enc_x(31, 3, 4, 5, 87))           # lbzx
    assert i.name == "lbzx"
    i = decode(enc_x(31, 3, 4, 5, 151))          # stwx
    assert i.name == "stwx"
    i = decode(enc_x(31, 3, 4, 5, 534))          # lwbrx
    assert i.name == "lwbrx"
    i = decode(enc_x(31, 3, 4, 5, 790))          # lhbrx
    assert i.name == "lhbrx"
    i = decode(enc_x(31, 3, 4, 5, 662))          # stwbrx
    assert i.name == "stwbrx"
    i = decode(enc_x(31, 3, 4, 5, 20))           # lwarx
    assert i.name == "lwarx"
    i = decode(enc_x(31, 3, 4, 5, 150, rc=1))    # stwcx.
    assert i.name == "stwcx" and i.rc == 1
    i = decode(enc_x(31, 3, 4, 0, 597) | (7 << 11))  # lswi nb=7
    assert i.name == "lswi" and i.nb == 7
    i = decode(enc_x(31, 3, 4, 5, 533))          # lswx
    assert i.name == "lswx"
    i = decode(enc_x(31, 3, 4, 5, 1014))         # dcbz
    assert i.name == "dcbz"
    i = decode(enc_x(31, 3, 4, 5, 598))          # sync
    assert i.name == "sync"
    i = decode(enc_x(31, 3, 4, 5, 854))          # eieio
    assert i.name == "eieio"
    i = decode((19 << 26) | (150 << 1))          # isync
    assert i.name == "isync"


# ------------------------------------------------------- SPR / MSR
def test_spr():
    i = decode(0x7C0802A6)                       # mflr r0
    assert i.name == "mfspr" and i.spr == 8 and i.d == 0
    i = decode(0x7C0803A6)                       # mtlr r0
    assert i.name == "mtspr" and i.spr == 8
    # mfspr XER (1): split field = ((1 & 31) << 16) | ((1 >> 5) << 11)
    i = decode((31 << 26) | (3 << 21) | spr_bits(1) | (339 << 1))
    assert i.name == "mfspr" and i.spr == 1
    # mfspr CTR (9) / mtspr GQR5 (917)
    i = decode((31 << 26) | (3 << 21) | spr_bits(9) | (339 << 1))
    assert i.spr == 9
    i = decode((31 << 26) | (3 << 21) | spr_bits(917) | (467 << 1))
    assert i.name == "mtspr" and i.spr == 917 and 912 <= i.spr <= 919
    # mftb (TBR 268 low / 269 high)
    i = decode((31 << 26) | (3 << 21) | spr_bits(268) | (371 << 1))
    assert i.name == "mftb" and i.spr == 268
    i = decode((31 << 26) | (3 << 21) | spr_bits(269) | (371 << 1))
    assert i.name == "mftb" and i.spr == 269
    # mfmsr / mtmsr
    i = decode(enc_x(31, 3, 0, 0, 83))
    assert i.name == "mfmsr"
    i = decode(enc_x(31, 3, 0, 0, 146))
    assert i.name == "mtmsr"
    assert GEKKO_PVR == 0x00083214               # retail Gekko 2.4e


# ------------------------------------------------------- FP
def test_fp():
    i = decode(enc_a(63, 1, 2, 3, 0, 21))        # fadd f1, f2, f3
    assert i.name == "fadd" and i.d == 1 and i.a == 2 and i.b == 3
    i = decode(enc_a(63, 1, 2, 3, 0, 21, 1))     # fadd.
    assert i.rc == 1
    i = decode(enc_a(63, 1, 2, 3, 0, 25))        # fmul (uses A and C)
    assert i.name == "fmul" and i.c == 0
    i = decode(enc_a(63, 1, 2, 3, 4, 29))        # fmadd f1, f2, f4, f3
    assert i.name == "fmadd" and i.c == 4
    i = decode(enc_a(59, 1, 2, 3, 0, 21))        # fadds
    assert i.name == "fadds"
    i = decode(enc_a(59, 1, 2, 3, 0, 24))        # fres (frB only)
    assert i.name == "fres"
    i = decode(enc_a(63, 1, 2, 3, 0, 22))        # fsqrt
    assert i.name == "fsqrt"
    i = decode(enc_x(63, 1, 0, 2, 40))           # fneg f1, f2
    assert i.name == "fneg"
    i = decode(enc_x(63, 1, 0, 2, 264))          # fabs
    assert i.name == "fabs"
    i = decode(enc_x(63, 1, 0, 2, 136))          # fnabs
    assert i.name == "fnabs"
    i = decode(enc_x(63, 1, 0, 2, 72))           # fmr
    assert i.name == "fmr"
    i = decode(enc_x(63, 1, 0, 2, 12))           # frsp
    assert i.name == "frsp"
    i = decode(enc_x(63, 1, 0, 2, 14))           # fctiw
    assert i.name == "fctiw"
    i = decode(enc_x(63, 1, 0, 2, 15))           # fctiwz
    assert i.name == "fctiwz"
    i = decode((63 << 26) | (3 << 23) | (2 << 16) | (5 << 11) | (0 << 1))
    assert i.name == "fcmpu" and i.crfd == 3
    i = decode((63 << 26) | (3 << 23) | (2 << 16) | (5 << 11) | (32 << 1))
    assert i.name == "fcmpo"
    # FP memory
    i = decode(0xC0230000)                       # lfs f1, 0(r3)
    assert i.name == "lfs" and i.d == 1 and i.a == 3
    i = decode(0xD83FFFF8)                       # stfd f1, -8(r1)
    assert i.name == "stfd" and i.simm == -8
    i = decode(enc_x(31, 1, 3, 5, 535))          # lfsx
    assert i.name == "lfsx"
    i = decode(enc_x(31, 1, 3, 5, 983))          # stfiwx
    assert i.name == "stfiwx"


def test_fpscr_ops():
    # mtfsf: FM = (word >> 17) & 0xFF (binutils FLM @17)
    i = decode((63 << 26) | (0xFF << 17) | (1 << 11) | (711 << 1))
    assert i.name == "mtfsf" and i.crm == 0xFF
    i = decode((63 << 26) | (0x80 << 17) | (1 << 11) | (711 << 1))
    assert i.crm == 0x80
    i = decode(enc_x(63, 1, 0, 0, 583))          # mffs f1
    assert i.name == "mffs"
    i = decode((63 << 26) | (6 << 21) | (38 << 1))    # mtfsb1 crb6
    assert i.name == "mtfsb1" and i.crbd == 6
    i = decode((63 << 26) | (6 << 21) | (70 << 1))    # mtfsb0 crb6
    assert i.name == "mtfsb0"
    i = decode((63 << 26) | (7 << 23) | (3 << 12) | (134 << 1))  # mtfsfi 7,3
    assert i.name == "mtfsfi" and i.crfd == 7 and i.uimm == 3
    i = decode((63 << 26) | (2 << 23) | (4 << 18) | (64 << 1))    # mcrfs 2,4
    assert i.name == "mcrfs" and i.crfd == 2 and i.crfs == 4


# ------------------------------------------------------- paired single
def test_paired_single():
    # ps_add f1, f2, f3: opcode 4, SUBOP5=21
    i = decode(enc_a(4, 1, 2, 3, 0, 21))
    assert i.name == "ps_add" and i.a == 2 and i.b == 3
    i = decode(enc_a(4, 1, 2, 3, 4, 29))         # ps_madd
    assert i.name == "ps_madd" and i.c == 4
    i = decode(enc_a(4, 1, 2, 3, 0, 10))         # ps_sum0
    assert i.name == "ps_sum0"
    i = decode(enc_a(4, 1, 2, 3, 0, 12))         # ps_muls0
    assert i.name == "ps_muls0"
    i = decode((4 << 26) | (0 << 21) | (2 << 16) | (3 << 11) | (0 << 1))
    assert i.name == "ps_cmpu0" and i.crfd == 0
    i = decode((4 << 26) | (2 << 23) | (2 << 16) | (3 << 11) | (32 << 1))
    assert i.name == "ps_cmpo0" and i.crfd == 2
    i = decode((4 << 26) | (1 << 23) | (2 << 16) | (3 << 11) | (96 << 1))
    assert i.name == "ps_cmpo1" and i.crfd == 1
    # fixed-table ops
    for xo, nm in ((40, "ps_neg"), (72, "ps_mr"), (136, "ps_nabs"),
                   (264, "ps_abs"), (528, "ps_merge00"), (560, "ps_merge01"),
                   (592, "ps_merge10"), (624, "ps_merge11"), (1014, "dcbz_l")):
        i = decode((4 << 26) | (1 << 21) | (2 << 16) | (3 << 11) | (xo << 1))
        assert i.name == nm, (nm, i.name)
    # ps_sel lane semantics are per-lane (checked in lifter docs)
    i = decode(enc_a(4, 1, 2, 3, 4, 23))
    assert i.name == "ps_sel"


def test_psq_fields():
    # psq_l f4, 0x10(r5), 0, 3 — D-form: W=bit15, I=bits14-12
    i = decode((56 << 26) | (4 << 21) | (5 << 16) | (0 << 15) | (3 << 12) | 0x10)
    assert i.name == "psq_l" and i.w == 0 and i.gqr == 3 and i.simm == 0x10
    i = decode((56 << 26) | (4 << 21) | (5 << 16) | (1 << 15) | (5 << 12) | 0x8)
    assert i.name == "psq_l" and i.w == 1 and i.gqr == 5 and i.simm == 8
    i = decode((60 << 26) | (4 << 21) | (5 << 16) | (1 << 15) | (2 << 12) | 0x20)
    assert i.name == "psq_st" and i.w == 1 and i.gqr == 2
    i = decode((57 << 26) | (4 << 21) | (5 << 16) | (0 << 15) | (7 << 12) | 0x4)
    assert i.name == "psq_lu" and i.gqr == 7
    # 12-bit signed displacement
    i = decode((56 << 26) | (4 << 21) | (5 << 16) | (3 << 12) | 0xFF8)
    assert i.simm == -8
    # X-form: W=bit10, I=bits9-7 (binutils PSWM/PSQM)
    i = decode((4 << 26) | (4 << 21) | (5 << 16) | (6 << 11) | (0 << 10) | (3 << 7) | (6 << 1))
    assert i.name == "psq_lx" and i.w == 0 and i.gqr == 3 and i.b == 6
    i = decode((4 << 26) | (4 << 21) | (5 << 16) | (6 << 11) | (1 << 10) | (3 << 7) | (6 << 1))
    assert i.name == "psq_lx" and i.w == 1
    i = decode((4 << 26) | (4 << 21) | (5 << 16) | (6 << 11) | (1 << 10) | (3 << 7) | (38 << 1))
    assert i.name == "psq_lux" and i.w == 1
    i = decode((4 << 26) | (4 << 21) | (5 << 16) | (6 << 11) | (0 << 10) | (7 << 7) | (7 << 1))
    assert i.name == "psq_stx" and i.gqr == 7
    i = decode((4 << 26) | (4 << 21) | (5 << 16) | (6 << 11) | (0 << 10) | (0 << 7) | (39 << 1))
    assert i.name == "psq_stux"


def test_gqr_decode():
    # UGQR (Dolphin Gekko.h): st_type bits0-2, st_scale bits8-13,
    # ld_type bits16-18, ld_scale bits24-29
    g = 0
    g |= (4 << 0)    # st_type u8
    g |= (3 << 8)    # st_scale 3
    g |= (7 << 16)   # ld_type s16
    g |= (0x1F << 24)  # ld_scale 31
    assert gqr_st_type(g) == 4
    assert gqr_st_scale(g) == 3
    assert gqr_ld_type(g) == 7
    assert gqr_ld_scale(g) == 31
    # 0x3F only sets st_type bits; ld_scale must read 0
    assert gqr_ld_scale(0x3F) == 0
    assert gqr_ld_scale(0x3F << 24) == 0x3F


def test_sign_extend():
    assert sx(0xFFFF, 16) == -1
    assert sx(0x8000, 16) == -32768
    assert sx(0x7FFF, 16) == 32767
    assert sx(0x03FFFFFC, 26) == -4              # b -4


def test_sc_rfi():
    i = decode(17 << 26)
    assert i.name == "sc"
    i = decode((19 << 26) | (50 << 1))
    assert i.name == "rfi"


if __name__ == "__main__":
    fails = 0
    for name, fn in sorted(list(globals().items())):
        if name.startswith("test_") and callable(fn):
            try:
                fn()
                print("PASS %s" % name)
            except AssertionError as e:
                print("FAIL %s: %s" % (name, e))
                fails += 1
    print("done" if not fails else "%d FAILURES" % fails)
    sys.exit(1 if fails else 0)


# ------------------------------------------------------- lift emission level
def test_lift_adest_family():
    """A 字段目标的指令族（or/rlwinm/slw/extsb...）发射方向回归。"""
    import sys, os
    sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    import tools.gekko.lift as L

    def emit(fn, word):
        l = L.Lifter.__new__(L.Lifter); l.out = []; l.n = 0
        getattr(L.Lifter, fn)(l, decode(word), 0, '')
        return l.out[-1]

    # or r24,r5,r5 (0x7cb82b78): 目标必须是 r24
    s = emit("e_or", 0x7cb82b78)
    assert "gc.r[24] =" in s and "gc.r[5] | gc.r[5]" in s, s
    # rlwinm r0,r3,2,...: 源 r3 → 目标 r0
    s = emit("e_rlwinm", 0x5460103A)
    assert "gc.r[0] =" in s and "gc.r[3]" in s, s
    # slw: 源 D 字段 → 目标 A 字段（slw r6,r3,r4: 7c632214）
    s = emit("e_slw", (31 << 26) | (3 << 21) | (6 << 16) | (4 << 11) | (24 << 1))
    assert "gc.r[6] =" in s and "gc.r[3] <<" in s, s
    # extsb r5,r9: 7d28 0774
    s = emit("e_extsb", (31 << 26) | (9 << 21) | (5 << 16) | (954 << 1))
    assert "gc.r[5] =" in s and "(int8_t)gc.r[9]" in s, s


def test_lift_dform_logic_adest():
    """ori/andi 等 D-form 逻辑：目标在 A 字段。ori r5,r6,0x7 = 0x60? encoding."""
    import sys, os
    sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    import tools.gekko.lift as L
    l = L.Lifter.__new__(L.Lifter); l.out = []; l.n = 0
    L.Lifter.e_ori(l, decode((24 << 26) | (6 << 21) | (5 << 16) | 0x0007), 0, '')
    s = l.out[-1]
    assert "gc.r[5] =" in s and "gc.r[6] | 0x00000007u" in s, s


def test_rotate_record_form():
    i = decode(0x540007FF)                       # rlwinm. r0,r0,0,31,31
    assert i.name == "rlwinm" and i.rc == 1, (i.name, i.rc)
    i = decode(0x540007FE)                       # rlwinm（无点）
    assert i.rc == 0


def test_lift_carry_family_updates_xer():
    """Carry-family emitters must publish CA for the next instruction."""
    import tools.gekko.lift as L

    def emit(fn, word):
        l = L.Lifter.__new__(L.Lifter); l.out = []; l.n = 0
        getattr(L.Lifter, fn)(l, decode(word), 0, '')
        return l.out[-1]

    for xo, fn in ((10, "e_addc"), (138, "e_adde"), (202, "e_addze"),
                   (234, "e_addme"), (8, "e_subfc"), (136, "e_subfe"),
                   (200, "e_subfze"), (232, "e_subfme")):
        text = emit(fn, enc_x(31, 3, 4, 5, xo))
        assert "g_set_ca(" in text, (fn, text)

    subfc = emit("e_subfc", enc_x(31, 3, 4, 5, 8))
    assert "+ (uint64_t)(1u)" in subfc, subfc
