"""decode.py — Gekko (PowerPC 750 + Gekko paired-single extensions) instruction
decoder for the Digimon Battle Chronicle static recompilation port.

Pure decode layer: word -> Insn (mnemonic + extracted fields).  No memory, no
semantics — the lifter (tools/gekko/lift.py) owns C emission.

Opcode tables cross-checked against (fetched 2026-10 from master):
  * Dolphin Emulator Source/Core/Core/PowerPC/Interpreter/Interpreter_Tables.cpp
    (opcodes 4 / 19 / 31 / 59 / 63)
  * GQR layout from Source/Core/Core/PowerPC/Gekko.h UGQR:
      ld_type  = bits [18:16], ld_scale = bits [29:24]
      st_type  = bits [ 2: 0], st_scale = bits [13: 8]
    quantize types: 0=float 4=u8 5=u16 6=s8 7=s16 (1..3 invalid)
  * ps_sum0 / ps_sel lane semantics from Interpreter_Paired.cpp (hardware-
    verified behaviour; note ps_sum0's ps1 lane comes from FRc.ps1, which
    differs from the IBM PEM text).

Gekko retail PVR = 0x00083214 (Dolphin PowerPC.cpp: "gekko 2.4e (8SE) retail").
"""

MASK32 = 0xFFFFFFFF


def sx(v, bits):
    """Sign-extend the low `bits` bits of v."""
    m = 1 << (bits - 1)
    return (v ^ m) - m


def ppc_mask(mb, me):
    """32-bit rotate-mask for rlwi* family.  mb/me are MSB-indexed (0..31).

    mb <= me : contiguous run of ones from MSB-bit mb through me.
    mb >  me : wrapped mask (mb..31 plus 0..me).
    """
    mb &= 31
    me &= 31
    if mb <= me:
        ones = (1 << (me - mb + 1)) - 1
        return (ones << (31 - me)) & MASK32
    m1 = (1 << (me + 1)) - 1                       # MSB bits 0..me
    m1 = (m1 << (31 - me)) & MASK32
    m2 = ((1 << (32 - mb)) - 1) & MASK32           # MSB bits mb..31
    return m1 | m2


class Insn:
    __slots__ = ("addr", "word", "name", "d", "a", "b", "c", "simm", "uimm",
                 "bo", "bi", "aa", "lk", "rc", "oe", "sh", "mb", "me", "spr",
                 "crfd", "crfs", "crbd", "crba", "crbb", "crm", "to", "nb",
                 "w", "gqr")

    def __init__(self, addr, word, name):
        self.addr = addr
        self.word = word
        self.name = name
        for s in Insn.__slots__[3:]:
            setattr(self, s, None)

    @property
    def simm16(self):
        return sx(self.word & 0xFFFF, 16)

    def __repr__(self):
        return "Insn(%08x@%08x %s)" % (self.word, self.addr & MASK32, self.name)


# ---------------------------------------------------------------- field cuts
def _D(w):    return (w >> 21) & 31
def _A(w):    return (w >> 16) & 31
def _B(w):    return (w >> 11) & 31
def _C(w):    return (w >> 6) & 31
def _RC(w):   return w & 1
def _XO10(w): return (w >> 1) & 0x3FF
def _XO5(w):  return (w >> 1) & 0x1F
def _SPR(w):  return ((w >> 16) & 0x1F) | (((w >> 11) & 0x1F) << 5)


# primary opcode 31: fixed (non-OE) X-forms — XO = bits[10:1]
OP31_FIXED = {
    0: "cmp", 4: "tw", 11: "mulhwu", 19: "mfcr", 20: "lwarx", 23: "lwzx",
    24: "slw", 26: "cntlzw", 28: "and", 32: "cmpl", 54: "dcbst", 55: "lwzux",
    60: "andc", 75: "mulhw", 83: "mfmsr", 86: "dcbf", 87: "lbzx", 119: "lbzux",
    124: "nor", 144: "mtcrf", 146: "mtmsr", 150: "stwcx", 151: "stwx",
    183: "stwux", 193: "crxor", 210: "mtsr", 215: "stbx", 225: "crnand",
    242: "mtsrin", 246: "dcbtst", 247: "stbux", 257: "crand", 278: "dcbt",
    279: "lhzx", 284: "eqv", 289: "creqv", 306: "tlbie", 310: "eciwx",
    407: "sthx",
    311: "lhzux", 316: "xor", 339: "mfspr", 343: "lhax", 371: "mftb",
    375: "lhaux", 412: "orc", 417: "crorc", 438: "ecowx", 439: "sthux",
    444: "or", 449: "cror", 467: "mtspr", 470: "dcbi", 476: "nand",
    512: "mcrxr", 533: "lswx", 534: "lwbrx", 535: "lfsx", 536: "srw",
    566: "tlbsync", 567: "lfsux", 595: "mfsr", 597: "lswi", 598: "sync",
    599: "lfdx", 631: "lfdux", 659: "mfsrin", 661: "stswx", 662: "stwbrx",
    663: "stfsx", 695: "stfsux", 725: "stswi", 727: "stfdx", 758: "dcba",
    759: "stfdux", 790: "lhbrx", 792: "sraw", 824: "srawi", 854: "eieio",
    918: "sthbrx", 922: "extsh", 954: "extsb", 982: "icbi", 983: "stfiwx",
    1014: "dcbz",
}

# opcode 31 arithmetic (OE-able + Rc-able).  Key: funct9 = XO10 & 0x1FF (OE=0).
OP31_ARITH = {
    266: "add", 10: "addc", 138: "adde", 234: "addme", 202: "addze",
    104: "neg", 40: "subf", 8: "subfc", 136: "subfe", 232: "subfme",
    200: "subfze", 235: "mullw", 491: "divw", 459: "divwu",
}

OP19 = {
    0: "mcrf", 16: "bclr", 17: "bclr",          # 17 = bclrl (LK folded)
    33: "crnor", 50: "rfi", 129: "crandc", 150: "isync", 193: "crxor",
    225: "crnand", 257: "crand", 289: "creqv", 417: "crorc", 449: "cror",
    528: "bcctr", 529: "bcctr",                  # 529 = bcctrl (LK folded)
}

OP4_FIXED = {
    0: "ps_cmpu0", 32: "ps_cmpo0", 40: "ps_neg", 64: "ps_cmpu1", 72: "ps_mr",
    96: "ps_cmpo1", 136: "ps_nabs", 264: "ps_abs", 528: "ps_merge00",
    560: "ps_merge01", 592: "ps_merge10", 624: "ps_merge11", 1014: "dcbz_l",
}
OP4_A = {  # keyed on SUBOP5 = XO10 & 0x1F
    10: "ps_sum0", 11: "ps_sum1", 12: "ps_muls0", 13: "ps_muls1",
    14: "ps_madds0", 15: "ps_madds1", 18: "ps_div", 20: "ps_sub",
    21: "ps_add", 23: "ps_sel", 24: "ps_res", 25: "ps_mul", 26: "ps_rsqrte",
    28: "ps_msub", 29: "ps_madd", 30: "ps_nmsub", 31: "ps_nmadd",
}
OP4_PSQ = {6: "psq_lx", 7: "psq_stx", 38: "psq_lux", 39: "psq_stux"}  # XO10&0x3F

OP59_A = {  # SUBOP5
    18: "fdivs", 20: "fsubs", 21: "fadds", 22: "fsqrts", 24: "fres",
    25: "fmuls", 28: "fmsubs", 29: "fmadds", 30: "fnmsubs", 31: "fnmadds",
}
OP63_A = {  # SUBOP5
    18: "fdiv", 20: "fsub", 21: "fadd", 22: "fsqrt", 23: "fsel", 25: "fmul",
    26: "frsqrte",
    28: "fmsub", 29: "fmadd", 30: "fnmsub", 31: "fnmadd",
}
OP63_X = {
    0: "fcmpu", 12: "frsp", 14: "fctiw", 15: "fctiwz", 32: "fcmpo",
    38: "mtfsb1", 40: "fneg", 64: "mcrfs", 70: "mtfsb0", 72: "fmr",
    134: "mtfsfi", 136: "fnabs", 264: "fabs", 583: "mffs", 711: "mtfsf",
}

DOPS = {  # primary, integer D-form / branches / rotates
    3: "twi", 7: "mulli", 8: "subfic", 10: "cmpli", 11: "cmpi", 12: "addic",
    13: "addic", 14: "addi", 15: "addis", 24: "ori", 25: "oris", 26: "xori",
    27: "xoris", 28: "andi", 29: "andis",
    16: "bc", 17: "sc", 18: "b",
    20: "rlwimi", 21: "rlwinm", 23: "rlwnm",
}
LSOPS = {
    32: "lwz", 33: "lwzu", 34: "lbz", 35: "lbzu", 36: "stw", 37: "stwu",
    38: "stb", 39: "stbu", 40: "lhz", 41: "lhzu", 42: "lha", 43: "lhau",
    44: "sth", 45: "sthu", 46: "lmw", 47: "stmw",
    48: "lfs", 49: "lfsu", 50: "lfd", 51: "lfdu", 52: "stfs", 53: "stfsu",
    54: "stfd", 55: "stfdu",
    56: "psq_l", 57: "psq_lu", 60: "psq_st", 61: "psq_stu",
}


def decode(word, addr=0):
    """Decode a 32-bit big-endian instruction word.  Returns Insn or None."""
    word &= MASK32
    op = word >> 26

    # ---------------- D-form integer / branches / rotates ----------------
    n = DOPS.get(op)
    if n is not None:
        i = Insn(addr, word, n)
        if n in ("andi", "andis") or (word >> 26) == 13:
            i.rc = 1              # andi./andis. implicit; addic.(op13) always CR0
        else:
            i.rc = 0
        if n in ("twi", "mulli", "subfic", "addic", "addi", "addis",
                 "ori", "oris", "xori", "xoris", "andi", "andis"):
            i.d = _D(word)
            i.a = _A(word)
            i.uimm = word & 0xFFFF
            i.simm = i.simm16
        elif n in ("cmpi", "cmpli"):
            i.crfd = (word >> 23) & 7
            i.a = _A(word)
            i.uimm = word & 0xFFFF
            i.simm = i.simm16
        elif n == "b":
            i.simm = sx(word & 0x03FFFFFC, 26)     # signed byte displacement
            i.aa = (word >> 1) & 1
            i.lk = word & 1
        elif n == "bc":
            i.bo = _D(word)
            i.bi = _A(word)
            i.simm = sx(word & 0xFFFC, 16)         # signed byte displacement
            i.aa = (word >> 1) & 1
            i.lk = word & 1
        elif n == "sc":
            i.lk = word & 1
        else:                                      # rlwimi/rlwinm/rlwnm
            i.d = _D(word)
            i.a = _A(word)
            i.b = _B(word)                         # rlwnm runtime shift reg
            i.sh = (word >> 11) & 31
            i.mb = (word >> 6) & 31
            i.me = (word >> 1) & 31
            i.rc = _RC(word)                       # rlwinm. 记录形式（此前漏设！）
        return i

    # ---------------- load/store / psq D-form ----------------
    n = LSOPS.get(op)
    if n is not None:
        i = Insn(addr, word, n)
        i.d = _D(word)
        i.a = _A(word)
        i.simm = i.simm16
        i.uimm = word & 0xFFFF
        if n.startswith("psq_"):
            i.w = (word >> 15) & 1                 # W = bit 15
            i.gqr = (word >> 12) & 7               # I = bits 14-12
            i.simm = sx(word & 0xFFF, 12)          # 12-bit displacement
        return i

    # ---------------- opcode 31 ----------------
    if op == 31:
        xo10 = _XO10(word)
        n = OP31_FIXED.get(xo10)
        if n is not None:
            i = Insn(addr, word, n)
            i.d = _D(word)
            i.a = _A(word)
            i.b = _B(word)
            i.rc = _RC(word)
            _fill31(i, word)
            return i
        n = OP31_ARITH.get(xo10 & 0x1FF)
        if n is not None:
            i = Insn(addr, word, n)
            i.d = _D(word)
            i.a = _A(word)
            i.b = _B(word)
            i.oe = (xo10 >> 9) & 1
            i.rc = _RC(word)
            return i
        return None

    # ---------------- opcode 19 ----------------
    if op == 19:
        xo10 = _XO10(word)
        n = OP19.get(xo10)
        if n is None:
            return None
        i = Insn(addr, word, n)
        if n in ("bclr", "bcctr"):
            i.bo = _D(word)
            i.bi = _A(word)
            i.lk = word & 1          # LK is word bit0; xo10 never carries it
            i.aa = 0
        elif n == "mcrf":
            i.crfd = (word >> 23) & 7
            i.crfs = (word >> 18) & 7
        else:                                      # cr logical
            i.crbd = _D(word)
            i.crba = _A(word)
            i.crbb = _B(word)
        return i

    # ---------------- opcode 4 (paired single) ----------------
    if op == 4:
        xo10 = _XO10(word)
        n = OP4_FIXED.get(xo10)
        if n is not None:
            i = Insn(addr, word, n)
            i.d = _D(word)
            i.a = _A(word)
            i.b = _B(word)
            i.rc = _RC(word)
            if n.startswith("ps_cmp"):
                i.crfd = (word >> 23) & 7
            return i
        n = OP4_PSQ.get(xo10 & 0x3F)
        if n is not None:
            i = Insn(addr, word, n)
            i.d = _D(word)
            i.a = _A(word)
            i.b = _B(word)
            i.w = (word >> 10) & 1                 # W = bit 10
            i.gqr = (word >> 7) & 7                # I = bits 9-7
            return i
        n = OP4_A.get(xo10 & 0x1F)
        if n is not None:
            i = Insn(addr, word, n)
            i.d = _D(word)
            i.a = _A(word)
            i.b = _B(word)
            i.c = _C(word)
            i.rc = _RC(word)
            if n.startswith("ps_cmp"):
                i.crfd = (word >> 23) & 7
            return i
        return None

    # ---------------- opcode 59 (single-precision FP) ----------------
    if op == 59:
        n = OP59_A.get(_XO5(word))
        if n is None:
            return None
        i = Insn(addr, word, n)
        i.d = _D(word)
        i.a = _A(word)
        i.b = _B(word)
        i.c = _C(word)
        i.rc = _RC(word)
        return i

    # ---------------- opcode 63 (double FP / fpstatus) ----------------
    if op == 63:
        xo10 = _XO10(word)
        n = OP63_X.get(xo10)
        if n is not None:
            i = Insn(addr, word, n)
            i.d = _D(word)
            i.a = _A(word)
            i.b = _B(word)
            i.rc = _RC(word)
            if n in ("fcmpu", "fcmpo"):
                i.crfd = (word >> 23) & 7
            elif n in ("mtfsb0", "mtfsb1"):
                i.crbd = _D(word)
            elif n == "mtfsfi":
                i.crfd = (word >> 23) & 7
                i.uimm = (word >> 12) & 0xF        # IMM
            elif n == "mcrfs":
                i.crfd = (word >> 23) & 7
                i.crfs = (word >> 18) & 7
            elif n == "mtfsf":
                i.crm = (word >> 17) & 0xFF        # FM (binutils FLM @17)
            return i
        n = OP63_A.get(_XO5(word))
        if n is not None:
            i = Insn(addr, word, n)
            i.d = _D(word)
            i.a = _A(word)
            i.b = _B(word)
            i.c = _C(word)
            i.rc = _RC(word)
            return i
        return None

    return None


def _fill31(i, word):
    n = i.name
    if n in ("cmp", "cmpl", "mcrxr"):
        i.crfd = (word >> 23) & 7
    elif n == "mtcrf":
        i.crm = (word >> 12) & 0xFF
    elif n in ("mfspr", "mtspr"):
        i.spr = _SPR(word)
    elif n == "mftb":
        i.spr = _SPR(word)                         # TBR 268/269
    elif n in ("mtsr", "mfsr"):
        i.spr = (word >> 16) & 15                  # SR field bits 20-16
    elif n == "srawi":
        i.sh = (word >> 11) & 31
    elif n in ("lswi", "stswi"):
        i.nb = (word >> 11) & 31
    elif n == "tw":
        i.to = _D(word)


# ---- GQR decode (single authority for lifter; HAL must not re-derive) ----
def gqr_ld_type(gqr):  return (gqr >> 16) & 7
def gqr_ld_scale(gqr): return (gqr >> 24) & 0x3F
def gqr_st_type(gqr):  return gqr & 7
def gqr_st_scale(gqr): return (gqr >> 8) & 0x3F

GQR_TYPES = {0: "float", 4: "u8", 5: "u16", 6: "s8", 7: "s16"}

GEKKO_PVR = 0x00083214          # SPR 287 read value ("gekko 2.4e (8SE)")
