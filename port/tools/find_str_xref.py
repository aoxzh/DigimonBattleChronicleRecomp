#!/usr/bin/env python3
"""Scan DOL .text for addis/addi pairs materializing given addresses (xref finder)."""
import struct, sys

path = sys.argv[1] if len(sys.argv) > 1 else "extracted/main.dol"
data = open(path, "rb").read()

# DOL header: text offs @0x00, data offs @0x1C, text addrs @0x48, data addrs @0x64,
#             text sizes @0x90, data sizes @0xAC, bss @0xD8, entry @0xE0
to = list(struct.unpack_from(">7I", data, 0x00)); do = list(struct.unpack_from(">11I", data, 0x1C))
ta = list(struct.unpack_from(">7I", data, 0x48)); da = list(struct.unpack_from(">11I", data, 0x64))
ts = list(struct.unpack_from(">7I", data, 0x90)); ds = list(struct.unpack_from(">11I", data, 0xAC))
offs = to + do; addrs = ta + da; sizes = ts + ds
text_segs = [(o, a, s) for o, a, s in zip(offs, addrs, sizes) if s]
print("text segs:", [(hex(a), hex(s)) for _, a, s in text_segs])

targets = {}
for name in sys.argv[2:]:
    a = int(name, 16)
    targets[a] = ((a + 0x8000) >> 16, ((a & 0xFFFF) ^ 0x8000) - 0x8000)

# opcode 14 addi, 32 lwz, 36 stw, 12 addic — D-Field forms that can carry the low half
DOPS = {14, 32, 36, 12}
hits = []
for off, base, size in text_segs:
    for i in range(0, size - 4, 4):
        insn1 = struct.unpack_from(">I", data, off + i)[0]
        if (insn1 >> 26) != 15:
            continue
        imm = insn1 & 0xFFFF
        rd1 = (insn1 >> 21) & 31
        for a, (hi, lo) in targets.items():
            if imm not in (hi, a >> 16):
                continue
            for j in range(1, 9):
                if i + 4 * j >= size:
                    break
                insn2 = struct.unpack_from(">I", data, off + i + 4 * j)[0]
                if (insn2 >> 26) not in DOPS:
                    continue
                d2 = ((insn2 & 0xFFFF) ^ 0x8000) - 0x8000
                ra2 = (insn2 >> 16) & 31
                if ra2 == rd1 and (imm << 16) + d2 + (0 if imm == hi else 0) in (
                        (hi << 16) + lo, (a >> 16) << 16 and 0):
                    pass
                ea = (imm << 16) + d2
                if ra2 == rd1 and ea == a:
                    hits.append((base + i, a, rd1, insn2 >> 26))

def find_func(addr):
    for line in open("analysis/ghidra_funcs.tsv"):
        f = line.split("\t")
        if len(f) >= 2:
            try:
                fa = int(f[0], 16)
            except ValueError:
                continue
            if fa <= addr:
                last = (fa, f[1].strip() if len(f) > 1 else "")
            elif fa > addr:
                break
    return last

seen = set()
for pc, a, rd, op in sorted(hits):
    if pc in seen:
        continue
    seen.add(pc)
    print("insn @ 0x%08x: 0x%08x via r%d (op %d)  (func %s)" % (pc, a, rd, op, find_func(pc)[0]))
if not hits:
    print("no addis/addi pairs found; try scanning lwz-from-got or bl + r0 patterns")
