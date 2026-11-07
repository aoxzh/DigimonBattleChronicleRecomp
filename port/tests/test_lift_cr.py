#!/usr/bin/env python3
"""Execute lifted CR transfers against a field-by-field reference model.

Run: python3 port/tests/test_lift_cr.py
Requires clang (or CC); generated C and binaries stay in a temporary directory.
"""

import os
from pathlib import Path
import random
import shlex
import struct
import subprocess
import tempfile

from test_lift_arithmetic import PORT, compile_c
from tools.gekko.decode import decode
from tools.gekko.lift import GEN_PREFIX_H, Lifter

INPUT = struct.Struct("<4I")
OUTPUT = struct.Struct("<34I")
SEED = 0x43524658


def variants():
    for register in (0, 3, 24, 31):
        for mask in range(256):
            yield "mtcrf", register, mask, (31 << 26 | register << 21 |
                                           mask << 12 | 144 << 1)
    for destination in range(8):
        for source in range(8):
            yield "mcrf", destination, source, (19 << 26 | destination << 23 |
                                                source << 18)
    for register in (0, 3, 24, 31):
        yield "mfcr", register, 0, (31 << 26 | register << 21 | 19 << 1)


def oracle(variant, value, cr, xer):
    name, register, selector, _ = variant
    registers = [(0x9E3779B9 * (i + 1)) & 0xFFFFFFFF for i in range(32)]
    fields = list(f"{cr:08x}")
    if name == "mtcrf":
        registers[register] = value
        source_fields = f"{value:08x}"
        for field, selected in enumerate(f"{selector:08b}"):
            if selected == "1":
                fields[field] = source_fields[field]
    elif name == "mcrf":
        fields[register] = fields[selector]
    else:
        registers[register] = cr
    return tuple(registers + [xer, int("".join(fields), 16)])


def harness(items):
    prefix = GEN_PREFIX_H.replace('"../hal/', '"' + str(PORT / "hal") + '/')
    functions = []
    for index, (name, register, selector, word) in enumerate(items):
        instruction = decode(word)
        assert instruction.name == name
        if name == "mtcrf":
            assert instruction.crm == selector and instruction.d == register
        lifter = Lifter([], [], 0)
        lifter.out = []
        assert lifter.emit(instruction, 0x80003100) is None
        setup = f"gc.r[{register}] = value;" if name == "mtcrf" else ""
        functions.append(f"static void op_{index}(uint32_t value) {{\n{setup}\n" +
                         "\n".join(lifter.out) + "\n}")
    table = ",".join(f"op_{index}" for index in range(len(items)))
    return prefix + "\n#include <stdio.h>\nGcCtx gc;\n" + "\n".join(functions) + f"""
static void (*const ops[])(uint32_t) = {{ {table} }};
int main(void) {{
    uint32_t input[4], output[34];
    while (fread(input, sizeof(input), 1, stdin) == 1) {{
        if (input[0] >= sizeof(ops) / sizeof(ops[0])) return 2;
        for (unsigned i = 0; i < 32; ++i) gc.r[i] = 0x9e3779b9u * (i + 1u);
        gc.cr = input[2]; gc.xer = input[3];
        ops[input[0]](input[1]);
        for (unsigned i = 0; i < 32; ++i) output[i] = gc.r[i];
        output[32] = gc.xer; output[33] = gc.cr;
        if (fwrite(output, sizeof(output), 1, stdout) != 1) return 3;
    }}
    return ferror(stdin) ? 4 : 0;
}}
"""


def main():
    items = list(variants())
    rng = random.Random(SEED)
    vectors = []
    for index in range(len(items)):
        # Both directions catch stale selected bits and lost unselected fields.
        for value, cr in ((0, 0xFFFFFFFF), (0xFFFFFFFF, 0),
                          (0x12345678, 0x89ABCDEF), (0x80000000, 0x20000000),
                          (0x20000000, 0x80000000)):
            vectors.append((index, value, cr, 0xE155AA7F))
        for _ in range(16):
            vectors.append((index, rng.getrandbits(32), rng.getrandbits(32),
                            rng.getrandbits(32)))
    payload = b"".join(INPUT.pack(*vector) for vector in vectors)
    cc = shlex.split(os.environ.get("CC", "clang"))
    with tempfile.TemporaryDirectory(prefix="gekko-cr-") as temporary:
        directory = Path(temporary)
        source = directory / "cr.c"
        source.write_text(harness(items))
        for optimization in ("-O0", "-O2"):
            binary = directory / ("cr" + optimization)
            compile_c(cc, source, binary, [optimization])
            run = subprocess.run([str(binary)], input=payload, capture_output=True,
                                 check=True, timeout=30)
            assert len(run.stdout) == len(vectors) * OUTPUT.size, "truncated C results"
            for vector, actual in zip(vectors, OUTPUT.iter_unpack(run.stdout)):
                index, value, cr, xer = vector
                expected = oracle(items[index], value, cr, xer)
                assert actual == expected, (items[index], vector, actual, expected)
            print(f"{optimization} PASS CR transfers: {len(vectors)} cases")
    print(f"{len(items)} emitter variants; {2 * len(vectors)} execution cases; "
          f"0 failures; seed=0x{SEED:08x}")


if __name__ == "__main__":
    main()
