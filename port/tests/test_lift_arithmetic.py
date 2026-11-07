#!/usr/bin/env python3
"""Execute lifted integer C against a separate, unbounded Python integer oracle.

Run directly, without pytest: python3 port/tests/test_lift_arithmetic.py
Requires clang (or --cc / CC); all generated files live in a temporary directory.
Unspecified divide-by-zero result values are deliberately outside this suite.
"""

import argparse
from collections import Counter
from dataclasses import dataclass
import itertools
import os
from pathlib import Path
import random
import shlex
import shutil
import struct
import subprocess
import sys
import tempfile

PORT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PORT))
from tools.gekko.decode import decode  # noqa: E402
from tools.gekko.lift import GEN_PREFIX_H, Lifter  # noqa: E402

MASK = 0xFFFFFFFF
CA, OV, SO = 1 << 29, 1 << 30, 1 << 31
SEED = 0x47454B4B
EDGES = (0, 1, 2, 0x7FFE, 0x7FFF, 0x8000, 0xFFFF,
         0x7FFFFFFE, 0x7FFFFFFF, 0x80000000, 0x80000001,
         0xFFFFFFFD, 0xFFFFFFFE, 0xFFFFFFFF)
IMMEDIATES = (-32768, -32767, -2, -1, 0, 1, 2, 32766, 32767)
XO = {"addc": 10, "adde": 138, "addze": 202, "addme": 234,
      "subfc": 8, "subfe": 136, "subfze": 200, "subfme": 232,
      "add": 266, "subf": 40, "neg": 104, "mullw": 235,
      "mulhw": 75, "mulhwu": 11}
UNARY = {"addze", "addme", "subfze", "subfme", "neg"}
CARRY = {"addc", "adde", "addze", "addme", "subfc", "subfe",
         "subfze", "subfme", "addic", "subfic"}
INPUT = struct.Struct("<5I")
OUTPUT = struct.Struct("<34I")


@dataclass(frozen=True)
class Variant:
    name: str
    d: int
    a: int
    b: int | None
    oe: int = 0
    rc: int = 0
    immediate: int | None = None

    @property
    def label(self):
        return (f"{self.name} d={self.d} a={self.a} b={self.b} "
                f"oe={self.oe} rc={self.rc} imm={self.immediate}")

    def word(self):
        if self.immediate is not None:
            op = {"addic": 13 if self.rc else 12, "subfic": 8, "mulli": 7}[self.name]
            return op << 26 | self.d << 21 | self.a << 16 | (self.immediate & 0xFFFF)
        return (31 << 26 | self.d << 21 | self.a << 16 |
                (self.b or 0) << 11 | self.oe << 10 | XO[self.name] << 1 | self.rc)


def variants():
    result = []
    for name in XO:
        layouts = ((3, 4, None), (4, 4, None), (3, 0, None), (0, 0, None)) if name in UNARY else (
            (3, 4, 5), (4, 4, 5), (5, 4, 5), (4, 4, 4), (3, 0, 5), (0, 0, 5))
        for d, a, b in layouts:
            for oe in ((0,) if name in {"mulhw", "mulhwu"} else (0, 1)):
                for rc in (0, 1):
                    result.append(Variant(name, d, a, b, oe, rc))
    for name in ("addic", "subfic", "mulli"):
        for d, a in ((3, 4), (4, 4), (3, 0), (0, 0)):
            for immediate in IMMEDIATES:
                for rc in ((0, 1) if name == "addic" else (0,)):
                    result.append(Variant(name, d, a, None, rc=rc, immediate=immediate))
    return result


def signed(value):
    return value if value < 1 << 31 else value - (1 << 32)


def initial_registers(v, a, b):
    registers = [(0x9E3779B9 * (i + 1)) & MASK for i in range(32)]
    registers[v.a] = a
    if v.b is not None:
        registers[v.b] = b
    return registers


def oracle(v, a, b, xer, cr):
    """Model ISA arithmetic with Python ints, not C helper bit formulas."""
    regs = initial_registers(v, a, b)
    a = regs[v.a]
    b = regs[v.b] if v.b is not None else 0
    sa, sb = signed(a), signed(b)
    cin = int(bool(xer & CA))
    n = v.name
    if n in {"add", "addc", "adde"}:
        carry_in = cin if n == "adde" else 0
        unsigned_total, signed_total = a + b + carry_in, sa + sb + carry_in
    elif n in {"addze", "addme"}:
        extra = -1 if n == "addme" else 0
        unsigned_total = a + (extra & MASK) + cin
        signed_total = sa + extra + cin
    elif n in {"subf", "subfc", "subfe"}:
        carry_in = cin if n == "subfe" else 1
        unsigned_total = (MASK - a) + b + carry_in
        signed_total = sb - sa + carry_in - 1
    elif n in {"subfze", "subfme"}:
        extra = -1 if n == "subfme" else 0
        unsigned_total = (MASK - a) + (extra & MASK) + cin
        signed_total = -sa + extra + cin - 1
    elif n == "addic":
        unsigned_total = a + (v.immediate & MASK)
        signed_total = sa + v.immediate
    elif n == "subfic":
        unsigned_total = (MASK - a) + (v.immediate & MASK) + 1
        signed_total = v.immediate - sa
    elif n == "neg":
        unsigned_total, signed_total = -a, -sa
    elif n in {"mullw", "mulli"}:
        signed_total = sa * (v.immediate if n == "mulli" else sb)
        unsigned_total = signed_total
    elif n in {"mulhw", "mulhwu"}:
        unsigned_total = ((sa * sb) if n == "mulhw" else (a * b)) >> 32
        signed_total = unsigned_total
    else:
        raise AssertionError(n)
    value = unsigned_total & MASK
    regs[v.d] = value
    if n in CARRY:
        xer = (xer & ~CA) | (CA if unsigned_total >= 1 << 32 else 0)
    if v.oe:
        overflow = not (-(1 << 31) <= signed_total < 1 << 31)
        xer = (xer & ~OV) | (OV | SO if overflow else 0)
    if v.rc:
        field = 8 if signed(value) < 0 else (4 if value else 2)
        field |= int(bool(xer & SO))
        cr = (cr & 0x0FFFFFFF) | field << 28
    return tuple(regs + [xer, cr])


def emitted_function(v, index):
    instruction = decode(v.word())
    assert instruction.name == v.name, (v, instruction.name)
    assert (instruction.oe or 0) == v.oe and instruction.rc == v.rc, v
    lifter = Lifter([], [], 0)
    lifter.out = []
    assert lifter.emit(instruction, 0x80003100) is None
    init_b = "" if v.b is None else f"gc.r[{v.b}] = b;"
    return f"""static void op_{index}(uint32_t a, uint32_t b) {{
    gc.r[{v.a}] = a; {init_b}
{chr(10).join(lifter.out)}
}}
"""


def harness(items):
    # Use the freshly emitted helper header, never a possibly stale recomp copy.
    prefix = GEN_PREFIX_H.replace('"../hal/', '"' + str(PORT / "hal") + '/')
    functions = "\n".join(emitted_function(v, i) for i, v in enumerate(items))
    table = ",\n".join(f"op_{i}" for i in range(len(items)))
    return prefix + "\n#include <stdio.h>\nGcCtx gc;\n" + functions + f"""
static void (*const ops[])(uint32_t, uint32_t) = {{ {table} }};
int main(void) {{
    uint32_t input[5], output[34];
    while (fread(input, sizeof(input), 1, stdin) == 1) {{
        if (input[0] >= sizeof(ops) / sizeof(ops[0])) return 2;
        for (unsigned i = 0; i < 32; ++i) gc.r[i] = 0x9e3779b9u * (i + 1u);
        gc.xer = input[3]; gc.cr = input[4];
        ops[input[0]](input[1], input[2]);
        for (unsigned i = 0; i < 32; ++i) output[i] = gc.r[i];
        output[32] = gc.xer; output[33] = gc.cr;
        if (fwrite(output, sizeof(output), 1, stdout) != 1) return 3;
    }}
    return ferror(stdin) ? 4 : 0;
}}
"""


def cases(v, count, rng):
    pairs = itertools.product(EDGES, EDGES) if v.b is not None else ((a, 0) for a in EDGES)
    for a, b in pairs:
        for flags in range(8):
            # Preserve unrelated XER bits, test all initial CA/OV/sticky SO states.
            xer = 0x0155AA7F | flags << 29
            cr = 0xA5C39E71
            yield a, b, xer, cr
    for _ in range(count):
        yield rng.getrandbits(32), rng.getrandbits(32), rng.getrandbits(32), rng.getrandbits(32)


def compile_c(cc, source, binary, flags):
    subprocess.run(cc + ["-std=c11", "-Wall", "-Wextra", "-Wno-unused-function",
                         "-Wno-unused-parameter", *flags, str(source), "-o", str(binary)],
                   check=True, capture_output=True, text=True, timeout=120)


def execution_check(binary, items, random_cases, optimization):
    rng = random.Random(SEED)
    counts, failures, examples = Counter(), Counter(), {}
    # Batch per instruction variant: keep memory bounded and avoid a process per case.
    for index, v in enumerate(items):
        vectors = list(cases(v, random_cases, rng))
        data = b"".join(INPUT.pack(index, *vector) for vector in vectors)
        run = subprocess.run([str(binary)], input=data, capture_output=True, check=True, timeout=30)
        if len(run.stdout) != len(vectors) * OUTPUT.size:
            raise AssertionError(f"{v.label}: truncated C results")
        for vector, actual in zip(vectors, OUTPUT.iter_unpack(run.stdout)):
            counts[v.name] += 1
            expected = oracle(v, *vector)
            if actual != expected:
                failures[v.name] += 1
                key = (v.name, tuple(i for i, (x, y) in enumerate(zip(actual, expected)) if x != y))
                if key not in examples:
                    examples[key] = (v, vector, actual, expected)
    for name in counts:
        bad = failures[name]
        print(f"{optimization} {'FAIL' if bad else 'PASS'} {name}: {counts[name]} cases, {bad} mismatches")
    for (name, fields), (v, vector, actual, expected) in examples.items():
        a, b, xer, cr = vector
        delta = ", ".join(f"{('r' + str(i)) if i < 32 else ('XER' if i == 32 else 'CR')}="
                          f"0x{actual[i]:08x} expected 0x{expected[i]:08x}" for i in fields)
        print(f"  {v.label}; inputs a=0x{a:08x} b=0x{b:08x} xer=0x{xer:08x} cr=0x{cr:08x}; {delta}")
    return sum(counts.values()), sum(failures.values())


def sanitizer_check(cc, source, directory, items):
    binary = directory / "arithmetic_ubsan"
    compile_c(cc, source, binary, ["-O1", "-fsanitize=undefined", "-fno-sanitize-recover=undefined"])
    failures = 0
    for name in ("mulli", "mullw"):
        index = next(i for i, v in enumerate(items) if v.name == name and v.d != v.a and
                     (v.immediate == 2 if name == "mulli" else v.b != v.d))
        run = subprocess.run([str(binary)], input=INPUT.pack(index, 0x7FFFFFFF, 2, 0, 0),
                             capture_output=True, timeout=30)
        if run.returncode:
            failures += 1
            print(f"UBSan FAIL {name}: {run.stderr.decode(errors='replace').splitlines()[0]}")
        elif len(run.stdout) != OUTPUT.size or OUTPUT.unpack(run.stdout) != oracle(items[index], 0x7FFFFFFF, 2, 0, 0):
            failures += 1
            print(f"UBSan FAIL {name}: incorrect output")
        else:
            print(f"UBSan PASS {name}: signed product exceeds int32 range")
    return failures


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default=os.environ.get("CC", "clang"))
    parser.add_argument("--random-cases", type=int, default=64, help="random vectors per instruction variant")
    parser.add_argument("--only", nargs="+", choices=tuple(XO) + ("addic", "subfic", "mulli"))
    args = parser.parse_args(argv)
    if args.random_cases < 0:
        parser.error("--random-cases must be nonnegative")
    cc = shlex.split(args.cc)
    if not cc or shutil.which(cc[0]) is None:
        parser.error(f"C compiler unavailable: {args.cc}")
    items = [v for v in variants() if args.only is None or v.name in args.only]
    total, failures = 0, 0
    with tempfile.TemporaryDirectory(prefix="gekko-arithmetic-") as temporary:
        directory = Path(temporary)
        source = directory / "arithmetic.c"
        source.write_text(harness(items))
        for optimization in ("-O0", "-O2"):
            binary = directory / ("arithmetic" + optimization)
            compile_c(cc, source, binary, [optimization])
            checked, bad = execution_check(binary, items, args.random_cases, optimization)
            total += checked
            failures += bad
        if args.only is None or {"mulli", "mullw"}.issubset(args.only):
            failures += sanitizer_check(cc, source, directory, items)
    print(f"{len(items)} emitter variants; {total} execution cases; {failures} failures; seed=0x{SEED:08x}")
    return int(bool(failures))


if __name__ == "__main__":
    try:
        sys.exit(main())
    except subprocess.CalledProcessError as error:
        print(f"Compiler/execution failed: {error}\n{error.stderr}", file=sys.stderr)
        sys.exit(2)
