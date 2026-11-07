#!/usr/bin/env python3
"""Execute emitted integer memory C against independent byte/address fixtures.

Run: python3 port/tests/test_lift_memory.py
Requires clang (or CC). Fresh GEN_PREFIX_H and synthetic instructions/data only;
all compilation, including old-mask mutation checks, uses a temporary directory.
No generated game sources, HAL objects, ROM, or shared build directory is used.
"""

from dataclasses import dataclass, replace
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile

from test_lift_arithmetic import PORT, compile_c
from tools.gekko.decode import decode
from tools.gekko.lift import GEN_PREFIX_H, Lifter


@dataclass(frozen=True)
class Variant:
    name: str
    opcode: int
    indexed: bool
    width: int
    store: bool = False
    update: bool = False
    signed: bool = False
    reverse: bool = False
    d: int = 3
    a: int = 4
    b: int = 5
    displacement: int = 0

    def word(self):
        fields = self.d << 21 | self.a << 16
        if self.indexed:
            return 31 << 26 | fields | self.b << 11 | self.opcode << 1
        return self.opcode << 26 | fields | (self.displacement & 0xFFFF)

    @property
    def label(self):
        return (f"{self.name} d={self.d} a={self.a} b={self.b} "
                f"disp={self.displacement}")


# ISA encodings/semantics are explicit, independent of emitter dispatch tables.
D_FORMS = (
    ("lwz", 32, 4, False, False, False),
    ("lwzu", 33, 4, False, True, False),
    ("lbz", 34, 1, False, False, False),
    ("lbzu", 35, 1, False, True, False),
    ("stw", 36, 4, True, False, False),
    ("stwu", 37, 4, True, True, False),
    ("stb", 38, 1, True, False, False),
    ("stbu", 39, 1, True, True, False),
    ("lhz", 40, 2, False, False, False),
    ("lhzu", 41, 2, False, True, False),
    ("lha", 42, 2, False, False, True),
    ("lhau", 43, 2, False, True, True),
    ("sth", 44, 2, True, False, False),
    ("sthu", 45, 2, True, True, False),
)
X_FORMS = (
    ("lwzx", 23, 4, False, False, False, False),
    ("lwzux", 55, 4, False, True, False, False),
    ("lbzx", 87, 1, False, False, False, False),
    ("lbzux", 119, 1, False, True, False, False),
    ("stwx", 151, 4, True, False, False, False),
    ("stwux", 183, 4, True, True, False, False),
    ("stbx", 215, 1, True, False, False, False),
    ("stbux", 247, 1, True, True, False, False),
    ("lhzx", 279, 2, False, False, False, False),
    ("lhzux", 311, 2, False, True, False, False),
    ("lhax", 343, 2, False, False, True, False),
    ("lhaux", 375, 2, False, True, True, False),
    ("sthx", 407, 2, True, False, False, False),
    ("sthux", 439, 2, True, True, False, False),
    ("lwbrx", 534, 4, False, False, False, True),
    ("stwbrx", 662, 4, True, False, False, True),
    ("lhbrx", 790, 2, False, False, False, True),
    ("sthbrx", 918, 2, True, False, False, True),
    ("eciwx", 310, 4, False, False, False, False),
    ("ecowx", 438, 4, True, False, False, False),
)
PAYLOADS = tuple(bytes.fromhex(s) for s in
                 ("80feabcd", "7f018923", "00000000", "ffffffff"))


def variants():
    for fields in D_FORMS:
        v = Variant(fields[0], fields[1], False, *fields[2:])
        for displacement in (0, 2, -2, -32768, 32767):
            yield replace(v, displacement=displacement)
        if not v.update:
            yield replace(v, a=0, displacement=0x6040)
        if v.store or not v.update:
            yield replace(v, d=v.a, displacement=4)
    for fields in X_FORMS:
        v = Variant(fields[0], fields[1], True, *fields[2:])
        yield v
        yield replace(v, b=v.a)  # RA/RB alias: capture original EA.
        yield replace(v, d=v.b)  # Load overwrites RB; store reads original RB.
        if not v.update:
            yield replace(v, a=0)  # GPR0 contains poison; EA uses zero base.
        if v.store or not v.update:
            yield replace(v, d=v.a)


def addresses(width):
    # Literal EA/physical pairs, not values obtained from g_phys/the old mask.
    return (
        ("LC-start", 0xE0000000, 0x01800000, 1),
        ("LC-interior", 0xE000305C, 0x0180305C, 1),
        ("LC-end", 0xE0004000 - width, 0x01804000 - width, 1),
        ("LC-outside", 0xE0004000, 0x00004000, 0),
        ("MEM1-physical", 0x00006040, 0x00006040, 0),
        ("MEM1-cached", 0x80006040, 0x00006040, 0),
        ("MEM1-uncached", 0xC0006040, 0x00006040, 0),
        ("WGPIPE-physical", 0x0C008000, 0x0C008000, 2),
        ("WGPIPE-cached", 0x8C008000, 0x0C008000, 2),
        ("WGPIPE-uncached", 0xCC008000, 0x0C008000, 2),
    )


def emit(v, name, mutation=None):
    instruction = decode(v.word())
    assert instruction is not None and instruction.name == v.name, v
    assert (instruction.d, instruction.a) == (v.d, v.a), v
    assert (instruction.b if v.indexed else instruction.simm) == (
        v.b if v.indexed else v.displacement), v
    lifter = Lifter([], [], 0)
    lifter.out = []
    assert lifter.emit(instruction, 0x80003100) is None
    body = "\n".join(lifter.out)
    if mutation == ("x" if v.indexed else "d"):
        # Mutate only emitted D/X physical assignment, leaving g_phys intact.
        # Never touch lift.py, generated sources, or a shared imported class.
        body, count = re.subn(r"g_phys\((l\d+_ea)\)",
                              r"(\1 & 0x1fffffffu)", body)
        assert count == 1, (v, count)
    return f"static void {name}(void) {{\n{body}\n}}\n"


def octets(data):
    return "{" + ",".join(f"0x{b:02x}" for b in data) + "}"


def harness(items, mutation=None):
    prefix = GEN_PREFIX_H.replace('"../hal/', '"' + str(PORT / "hal") + '/')
    functions, ops, cases = [], [], []
    for index, v in enumerate(items):
        functions.append(emit(v, f"op_{index}", mutation))
        ops.append(f'{{"{v.label}", op_{index}, {v.d}, {v.width}, '
                   f'{int(v.store)}}}')
        locations = addresses(v.width)
        if not v.indexed and v.a == 0:
            locations = (("D-zero-base", 0x6040, 0x6040, 0),)
        for label, ea, physical, route in locations:
            regs = [(0x12340000 + i) for i in range(32)]
            regs[v.d] = 0x89ABCDEF
            regs[v.a] = (ea - v.displacement) & 0xFFFFFFFF
            if v.a == 0:
                regs[0] = 0xDEADBEEF
            if v.indexed:
                if v.a == 0:
                    regs[v.b] = ea
                elif v.a == v.b:
                    # Odd EA requires another layout; RA+RA cannot produce it.
                    if ea & 1:
                        continue
                    regs[v.a] = ea // 2
                else:
                    regs[v.b] = 0x20
                    regs[v.a] = (ea - 0x20) & 0xFFFFFFFF
            expected = regs.copy()
            if v.update:
                expected[v.a] = ea
            source = regs[v.d] & ((1 << (8 * v.width)) - 1)
            stored = source.to_bytes(v.width, "little" if v.reverse else "big")
            # Four independent load patterns exercise sign and zero extension.
            for payload in PAYLOADS:
                loaded = int.from_bytes(payload[:v.width],
                                        "little" if v.reverse else "big",
                                        signed=v.signed) & 0xFFFFFFFF
                wanted = expected.copy()
                if not v.store:
                    wanted[v.d] = loaded
                cases.append(f'{{{index}, "{label}/{payload.hex()}", '
                             f'0x{physical:08x}u, {route}, '
                             '{' + ','.join(f'0x{r:08x}u' for r in regs) + '}, '
                             '{' + ','.join(f'0x{r:08x}u' for r in wanted) + '}, '
                             f'{octets(payload)}, {octets(stored)}}}')
    # Exact failing load plus a synthetic indexed vertex write. Distinct offsets
    # expose a wrong lowMEM index overwriting the fixture's resource header.
    functions.append(emit(Variant("lhzu", 41, False, 2, update=True,
                                  d=20, a=7, displacement=2), "regression_load", mutation))
    functions.append(emit(Variant("stwx", 151, True, 4, store=True,
                                  d=3, a=6, b=20), "regression_store", mutation))
    return (prefix + C_SHIM + "\n".join(functions) +
            "static const Op ops[] = {\n" + ",\n".join(ops) + "\n};\n" +
            "static const Case cases[] = {\n" + ",\n".join(cases) + "\n};\n" + C_MAIN)


C_SHIM = r'''
GcCtx gc;
uint8_t gc_ram[0x01800000];
static uint8_t lc[0x4000], mmio[16];
static const char *current_op = "lhzu r20,2(r7)", *current_case = "LC-index regression";
static struct { uint32_t p, value; unsigned width, write, route; } trace[4];
static unsigned trace_count;
static void require(int ok, const char *message) {
    if (!ok) {
        fprintf(stderr, "FAIL memory: %s / %s: %s\n", current_op, current_case, message);
        exit(1);
    }
}
static uint8_t *backing(uint32_t p, unsigned width, unsigned route) {
    if (route == 1) {
        require(p >= 0x01800000u && p <= 0x01804000u - width, "invalid LC slow access");
        return lc + (p - 0x01800000u);
    }
    require(route == 2 && p >= 0x0c008000u && p <= 0x0c008010u - width,
            "invalid WGPIPE MMIO access");
    return mmio + (p - 0x0c008000u);
}
static uint32_t access_read(uint32_t p, unsigned width, unsigned route) {
    uint8_t *bytes = backing(p, width, route);
    uint32_t value = 0;
    for (unsigned i = 0; i < width; ++i) value = value * 256u + bytes[i];
    require(trace_count < 4, "too many accesses");
    trace[trace_count].p = p; trace[trace_count].value = value;
    trace[trace_count].width = width; trace[trace_count].write = 0;
    trace[trace_count++].route = route;
    return value;
}
static void access_write(uint32_t p, uint32_t value, unsigned width, unsigned route) {
    uint8_t *bytes = backing(p, width, route);
    require(trace_count < 4, "too many accesses");
    trace[trace_count].p = p; trace[trace_count].value = value;
    trace[trace_count].width = width; trace[trace_count].write = 1;
    trace[trace_count++].route = route;
    for (unsigned i = 0; i < width; ++i)
        bytes[i] = (uint8_t)(value >> (8u * (width - i - 1u)));
}
#define HAL_ACCESS(bits, type) \
    type hal_slow_read##bits(uint32_t p) { return (type)access_read(p, bits/8, 1); } \
    type hal_mmio_read##bits(uint32_t p) { return (type)access_read(p, bits/8, 2); } \
    void hal_slow_write##bits(uint32_t p, type v) { access_write(p, v, bits/8, 1); } \
    void hal_mmio_write##bits(uint32_t p, type v) { access_write(p, v, bits/8, 2); }
HAL_ACCESS(8, uint8_t)
HAL_ACCESS(16, uint16_t)
HAL_ACCESS(32, uint32_t)
void gc_watch_note(uint32_t p, uint32_t v) { (void)p; (void)v; }
typedef struct {
    const char *name;
    void (*run)(void);
    unsigned d, width, store;
} Op;
typedef struct {
    unsigned op;
    const char *name;
    uint32_t physical;
    unsigned route;
    uint32_t initial[32], expected[32];
    uint8_t payload[4], stored[4];
} Case;
'''


C_MAIN = r'''
static void index_regression(void) {
    memset(&gc, 0, sizeof(gc));
    memset(gc_ram, 0xa5, sizeof(gc_ram)); memset(lc, 0xa5, sizeof(lc));
    gc_ram[0x305e] = 0; gc_ram[0x305f] = 0x10;
    lc[0x305e] = 0; lc[0x305f] = 0x40;
    gc.r[7] = 0xe000305cu; gc.r[6] = 0x80006000u; gc.r[3] = 0x89abcdefu;
    trace_count = 0;
    regression_load(); regression_store();
    /* Check downstream damage first: old lhzu used lowMEM's index 0x10. */
    require(gc_ram[0x6010] == 0xa5 && gc_ram[0x6011] == 0xa5 &&
            gc_ram[0x6012] == 0xa5 && gc_ram[0x6013] == 0xa5,
            "resource header overwritten by vertex with lowMEM index");
    require(gc.r[20] == 0x40 && gc.r[7] == 0xe000305eu,
            "lhzu must load LC index and preserve guest EA in RA");
    require(gc_ram[0x6040] == 0x89 && gc_ram[0x6041] == 0xab &&
            gc_ram[0x6042] == 0xcd && gc_ram[0x6043] == 0xef,
            "vertex missing from correct destination");
    require(trace_count == 1 && trace[0].p == 0x0180305eu &&
            trace[0].width == 2 && !trace[0].write && trace[0].route == 1 &&
            trace[0].value == 0x40, "lhzu did not read HAL LC halfword");
}
int main(void) {
    index_regression();
    unsigned checked = 0;
    for (unsigned c = 0; c < sizeof(cases)/sizeof(cases[0]); ++c) {
        const Case *test = &cases[c];
        const Op *op = &ops[test->op];
        current_op = op->name; current_case = test->name;
        memset(lc, 0xa5, sizeof(lc)); memset(mmio, 0xa5, sizeof(mmio));
        memset(gc_ram, 0xa5, 0x4000);
        memset(gc_ram + 0x4000 - 8, 0xa5, 24);
        memset(gc_ram + 0x6040 - 8, 0xa5, 24);
        memset(&gc, 0, sizeof(gc)); memcpy(gc.r, test->initial, sizeof(gc.r));
        for (unsigned i = 0; i < 32; ++i) gc.fpr_bits[i] = 0x4050000040200000ull + i;
        gc.cr = 0x89abcdefu; gc.xer = 0xe155aa7fu; gc.lr = 0x81234560u;
        gc.ctr = 0x87654320u; gc.fpscr = 0x12345678u;
        GcCtx expected = gc;
        memcpy(expected.r, test->expected, sizeof(expected.r));
        uint8_t *target = test->route ? backing(test->physical, op->width, test->route)
                                     : gc_ram + test->physical;
        if (!op->store) memcpy(target, test->payload, op->width);
        uint8_t wanted_lc[sizeof(lc)], wanted_mmio[sizeof(mmio)];
        uint8_t wanted_ram[24], wanted_low[0x4000];
        uint32_t ram_address = test->route ? 0x6040u : test->physical;
        memcpy(wanted_lc, lc, sizeof(lc)); memcpy(wanted_mmio, mmio, sizeof(mmio));
        memcpy(wanted_ram, gc_ram + ram_address - 8, sizeof(wanted_ram));
        memcpy(wanted_low, gc_ram, sizeof(wanted_low));
        if (op->store) {
            uint8_t *wanted = test->route == 1 ? wanted_lc + (test->physical - 0x01800000u)
                              : test->route == 2 ? wanted_mmio : wanted_ram + 8;
            memcpy(wanted, test->stored, op->width);
        }
        trace_count = 0;
        op->run();
        require(memcmp(gc_ram, wanted_low, sizeof(wanted_low)) == 0,
                "lowMEM sentinel overwritten");
        require(memcmp(lc, wanted_lc, sizeof(lc)) == 0, "LC data/guards differ");
        require(memcmp(mmio, wanted_mmio, sizeof(mmio)) == 0, "WGPIPE data/guards differ");
        require(memcmp(gc_ram + ram_address - 8, wanted_ram, sizeof(wanted_ram)) == 0,
                "MEM1 data/guards differ");
        require(memcmp(&gc, &expected, sizeof(gc)) == 0,
                "CPU state differs (load value, guest RA, or unrelated registers)");
        require(trace_count == (test->route ? 1u : 0u), "access count/routing differs");
        if (trace_count) {
            require(trace[0].p == test->physical && trace[0].width == op->width &&
                    trace[0].write == op->store && trace[0].route == test->route,
                    "physical address/width/direction/route differs");
            uint32_t value = 0;
            const uint8_t *bytes = op->store ? test->stored : test->payload;
            for (unsigned i = 0; i < op->width; ++i) value = value * 256u + bytes[i];
            require(trace[0].value == value, "HAL transfer value/endian order differs");
        }
        ++checked;
    }
    printf("PASS memory: %u matrix cases + lhzu/vertex regression; %zu emitter variants\n",
           checked, sizeof(ops)/sizeof(ops[0]));
    return 0;
}
'''


def main():
    items = list(variants())
    cc = shlex.split(os.environ.get("CC", "clang"))
    modes = (("O0", ["-O0"]), ("O2", ["-O2"]),
             ("ASan+UBSan", ["-O1", "-g", "-fsanitize=address,undefined",
                             "-fno-sanitize-recover=all", "-fno-omit-frame-pointer"]))
    with tempfile.TemporaryDirectory(prefix="gekko-memory-") as temporary:
        directory = Path(temporary)
        source = directory / "memory.c"
        source.write_text(harness(items))
        for label, flags in modes:
            binary = directory / label
            compile_c(cc, source, binary, flags)
            run = subprocess.run([str(binary)], capture_output=True, text=True,
                                 check=True, timeout=30)
            print(f"{label} {run.stdout.strip()}", flush=True)
        for family in ("d", "x"):
            source.write_text(harness(items, mutation=family))
            for label, flags in modes[:2]:
                binary = directory / f"oldmask_{family}_{label}"
                compile_c(cc, source, binary, flags)
                run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
                assert run.returncode == 1 and "FAIL memory:" in run.stderr, (
                    f"old-mask {family.upper()} mutation was not rejected by an assertion", run)
                print(f"{label} PASS old-mask {family.upper()} mutation rejected: "
                      f"{run.stderr.strip()}", flush=True)


if __name__ == "__main__":
    try:
        main()
    except subprocess.CalledProcessError as error:
        print(f"Compiler/execution failed: {error}\n{error.stderr}", file=sys.stderr)
        sys.exit(2)
