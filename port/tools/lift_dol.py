#!/usr/bin/env python3
"""lift_dol.py — lift a GameCube DOL into recompilable C.

Usage:
    python tools/lift_dol.py extracted/main.dol -o recomp

Outputs:
    recomp/gen_prefix.h, recomp/blocks.c, recomp/recomp_entry.c,
    recomp/generated/b_XXXXXXXX.c (<=400 blocks per file),
    recomp/coverage.json (opcode histogram + unknown-instruction list)
"""

import argparse
import json
import os
import struct
import sys
from collections import Counter

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))          # repo/port on sys.path

from tools.gekko.lift import Lifter                # noqa: E402


def parse_dol(blob):
    """Standard DOL header (big-endian):
    0x00 7x text offsets | 0x1C 11x data offsets | 0x48 7x text addrs
    0x64 11x data addrs  | 0x90 7x text sizes    | 0xAC 11x data sizes
    0xD8 bss addr | 0xDC bss size | 0xE0 entry
    """
    text_off = struct.unpack_from(">7I", blob, 0x00)
    data_off = struct.unpack_from(">11I", blob, 0x1C)
    text_addr = struct.unpack_from(">7I", blob, 0x48)
    data_addr = struct.unpack_from(">11I", blob, 0x64)
    text_size = struct.unpack_from(">7I", blob, 0x90)
    data_size = struct.unpack_from(">11I", blob, 0xAC)
    bss_addr, bss_size, entry = struct.unpack_from(">III", blob, 0xD8)
    text, data = [], []
    for k in range(7):
        if text_size[k]:
            text.append((text_addr[k], blob[text_off[k]:text_off[k] + text_size[k]]))
    for k in range(11):
        if data_size[k]:
            data.append((data_addr[k], blob[data_off[k]:data_off[k] + data_size[k]]))
    return entry, bss_addr, bss_size, text, data


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dol")
    ap.add_argument("-o", "--out", default="recomp")
    ap.add_argument("--extra-root", action="append", default=[],
                    metavar="VADDR",
                    help="额外代码入口（十六进制虚拟地址，可多次）；"
                         "用于运行时 MISS 的间接调用目标回填")
    ap.add_argument("--extra-roots-file", default=None, metavar="FILE",
                    help="每行一个入口地址的文件（批量回填，与 --extra-root 同级）")
    ap.add_argument("--hook-blocks", default=None, metavar="FILE",
                    help="每行一个块地址：在这些块入口生成 gc_probe_block() 调用")
    args = ap.parse_args()

    blob = open(args.dol, "rb").read()
    entry, bss_addr, bss_size, text, data = parse_dol(blob)
    print("DOL: entry=0x%08x bss=0x%08x+0x%x text=%d data=%d"
          % (entry, bss_addr, bss_size, len(text), len(data)))
    for a, b in text:
        print("  text 0x%08x + 0x%x" % (a, len(b)))
    for a, b in data:
        print("  data 0x%08x + 0x%x" % (a, len(b)))

    probe = []
    if args.hook_blocks:
        for line in open(args.hook_blocks, encoding="utf-8"):
            line = line.strip()
            if line and not line.startswith("#"):
                probe.append(int(line, 16))
        print("probe blocks: %d" % len(probe))
    lifter = Lifter(text, data, entry, probe_blocks=probe)
    lifter.extra_roots = [int(rs.replace("0x", ""), 16) for rs in args.extra_root]
    if args.extra_roots_file:
        with open(args.extra_roots_file, encoding="utf-8") as rf:
            for line in rf:
                line = line.strip()
                if line and not line.startswith("#"):
                    lifter.extra_roots.append(int(line.replace("0x", ""), 16))
    for rs in lifter.extra_roots:
        print("extra root: 0x%08x" % rs)
    lifter.run()
    files = lifter.write_outputs(args.out)

    total_insns = sum(b.ninsn for b in lifter.blocks.values())
    pseudo = sum(1 for b in lifter.blocks.values() if not b.conf)
    hist = lifter.hist
    top20 = hist.most_common(20)
    cov = {
        "entry": "0x%08x" % entry,
        "blocks": len(lifter.blocks),
        "blocks_pseudo": pseudo,
        "functions": len(lifter.func_starts),
        "instructions": total_insns,
        "generated_files": len(files),
        "opcode_count": len(hist),
        "opcode_histogram": dict(hist.most_common()),
        "top20": [[n, c] for n, c in top20],
        "unknown": [
            {"addr": "0x%08x" % a, "word": "0x%08x" % w, "why": why}
            for a, w, why in lifter.unknown],
        "data_skipped_roots": lifter.data_skipped,
    }
    with open(os.path.join(args.out, "coverage.json"), "w", encoding="utf-8", newline="\n") as f:
        json.dump(cov, f, indent=1)

    print("\nblocks=%d (pseudo=%d) functions=%d instructions=%d files=%d"
          % (len(lifter.blocks), pseudo, len(lifter.func_starts),
             total_insns, len(files)))
    print("distinct opcodes=%d" % len(hist))
    print("top20:")
    for n, c in top20:
        print("  %-12s %d" % (n, c))
    print("unknown=%d data_skipped=%d"
          % (len(lifter.unknown), lifter.data_skipped))
    for a, w, why in lifter.unknown[:20]:
        print("  UNKNOWN @%08x word=%08x %s" % (a, w, why))
    if lifter.unknown:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
