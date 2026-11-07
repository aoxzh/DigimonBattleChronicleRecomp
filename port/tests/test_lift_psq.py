#!/usr/bin/env python3
"""Execute emitted PSQ C, including the charselect locked-cache regression.

Run: python3 port/tests/test_lift_psq.py
Requires clang (or CC). Uses fresh GEN_PREFIX_H, never recomp/generated files;
all C sources and binaries live in a temporary directory. HAL stubs record
physical accesses, with separate locked-cache and WGPIPE backing stores.
"""

from dataclasses import dataclass
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


@dataclass(frozen=True)
class Variant:
    name: str
    opcode: int
    indexed: bool
    store: bool
    update: bool
    w: int
    gqr: int
    displacement: int = 0
    a: int = 4
    b: int = 5

    def word(self):
        fields = 3 << 21 | self.a << 16
        if self.indexed:
            return (4 << 26 | fields | self.b << 11 | self.w << 10 |
                    self.gqr << 7 | self.opcode << 1)
        return (self.opcode << 26 | fields | self.w << 15 |
                self.gqr << 12 | (self.displacement & 0xFFF))

    @property
    def label(self):
        return (f"{self.name} W={self.w} I={self.gqr} "
                f"d={self.displacement} a={self.a} b={self.b}")


# Independent literal fixtures: GQR, bytes, decoded values, store source values.
# Fractional stores also verify truncation. Different load/store expectations
# prevent a mutually wrong load/store round trip from passing.
FORMATS = (
    ("float", 0x00000000, "3fc00000c0100000", (1.5, -2.25), (1.5, -2.25)),
    ("u8-scale1", 0x01040104, "0309", (1.5, 4.5), (1.75, 4.75)),
    ("u16-scale1", 0x01050105, "01232345", (145.5, 4514.5), (145.75, 4514.75)),
    ("s8-scale1", 0x01060106, "fa0e", (-3.0, 7.0), (-3.25, 7.25)),
    ("s16-scale1", 0x01070107, "fedc1234", (-146.0, 2330.0), (-146.25, 2330.25)),
    ("s16-scale-1", 0x3F073F07, "fffe0003", (-4.0, 6.0), (-4.5, 6.5)),
)

# EA -> expected physical address and route (RAM=0, LC slow path=1, MMIO=2).
# These are deliberately literal, not computed using the helper under test.
ADDRESSES = (
    ("LC-start", 0xE0000000, 0x01800000, 1),
    ("LC-interior", 0xE0000120, 0x01800120, 1),
    ("LC-last-pair", 0xE0003FF8, 0x01803FF8, 1),
    ("RAM-physical", 0x00006040, 0x00006040, 0),
    ("RAM-cached", 0x80006040, 0x00006040, 0),
    ("RAM-uncached", 0xC0006040, 0x00006040, 0),
    ("WGPIPE-physical", 0x0C008000, 0x0C008000, 2),
    ("WGPIPE-cached", 0x8C008000, 0x0C008000, 2),
    ("WGPIPE-uncached", 0xCC008000, 0x0C008000, 2),
)


def variants():
    for name, opcode, indexed, store, update in (
        ("psq_l", 56, False, False, False),
        ("psq_lu", 57, False, False, True),
        ("psq_st", 60, False, True, False),
        ("psq_stu", 61, False, True, True),
        ("psq_lx", 6, True, False, False),
        ("psq_lux", 38, True, False, True),
        ("psq_stx", 7, True, True, False),
        ("psq_stux", 39, True, True, True),
    ):
        for w in (0, 1):
            for gqr in (0, 7):
                for displacement in ((0,) if indexed else (0, 16, -16)):
                    yield Variant(name, opcode, indexed, store, update, w, gqr,
                                  displacement)
                if indexed:
                    # EA must use the original RA/RB even when they alias.
                    yield Variant(name, opcode, True, store, update, w, gqr, b=4)
                    if not update:
                        # RA=0 means zero base, not the contents of GPR0.
                        yield Variant(name, opcode, True, store, False, w, gqr, a=0)


def raw_lanes(values):
    bits = [struct.unpack('<Q', struct.pack('<d', value))[0] for value in values]
    return '{' + ', '.join(f'0x{value:016x}ull' for value in bits) + '}'


def harness(items):
    prefix = GEN_PREFIX_H.replace('"../hal/', '"' + str(PORT / "hal") + '/')
    functions, descriptors = [], []
    for index, v in enumerate(items):
        instruction = decode(v.word())
        assert instruction is not None and instruction.name == v.name, v
        assert (instruction.d, instruction.a, instruction.w, instruction.gqr) == (
            3, v.a, v.w, v.gqr), v
        if v.indexed:
            assert instruction.b == v.b, v
        else:
            assert instruction.simm == v.displacement, v
        lifter = Lifter([], [], 0)
        lifter.out = []
        # The failing charselect store was lifted at this PC.
        assert lifter.emit(instruction, 0x8024C110) is None
        functions.append(f"static void op_{index}(void) {{\n" +
                         "\n".join(lifter.out) + "\n}")
        descriptors.append(f'{{"{v.label}", op_{index}, {int(v.store)}, '
                           f'{int(v.update)}, {v.w}, {v.gqr}, {v.a}, {v.b}}}')
    formats = []
    for label, gqr, data, loaded, stored in FORMATS:
        raw = bytes.fromhex(data)
        octets = ",".join(f"0x{byte:02x}" for byte in raw)
        formats.append(f'{{"{label}", 0x{gqr:08x}u, {len(raw) // 2}, '
                       f'{{{octets}}}, {raw_lanes(loaded)}, '
                       f'{raw_lanes((loaded[0], 1.0))}, {raw_lanes(stored)}}}')
    cases = []
    for index, v in enumerate(items):
        for label, ea, physical, route in ADDRESSES:
            base, rb = (ea - v.displacement) & 0xFFFFFFFF, 0x20
            if v.indexed:
                if v.a == 0:
                    base, rb = 0xDEADBEEF, ea
                elif v.a == v.b:
                    base = rb = ea // 2
                else:
                    base = (ea - rb) & 0xFFFFFFFF
            expected_ra = ea if v.update else base
            cases.append(f'{{{index}, "{label}", 0x{base:08x}u, 0x{rb:08x}u, '
                         f'0x{expected_ra:08x}u, 0x{physical:08x}u, {route}}}')
    return (prefix + C_SHIM + "\n".join(functions) +
            "\nstatic const Op ops[] = {\n" + ",\n".join(descriptors) + "\n};\n" +
            "static const Format formats[] = {\n" + ",\n".join(formats) + "\n};\n" +
            "static const Case cases[] = {\n" + ",\n".join(cases) + "\n};\n" + C_MAIN)


C_SHIM = r'''
GcCtx gc;
uint8_t gc_ram[0x01800000];
static uint8_t lc[0x4000], mmio[16];
static const char *current_op, *current_address, *current_format;
static struct { uint32_t p, value; unsigned width, write, route; } trace[4];
static unsigned trace_count;
static void require(int ok, const char *message) {
    if (!ok) {
        fprintf(stderr, "%s / %s / %s: %s\n", current_op, current_address,
                current_format, message);
        exit(1);
    }
}
/* Stub routing intentionally admits only the literal physical windows. */
static uint8_t *backing(uint32_t p, unsigned width, unsigned route) {
    if (route == 1) {
        require(p >= 0x01800000u && p <= 0x01804000u - width,
                "slow access outside locked cache");
        return lc + (p - 0x01800000u);
    }
    require(p >= 0x0c008000u && p <= 0x0c008010u - width,
            "MMIO access outside WGPIPE fixture");
    return mmio + (p - 0x0c008000u);
}
static uint32_t access_read(uint32_t p, unsigned width, unsigned route) {
    uint8_t *bytes = backing(p, width, route);
    uint32_t value = 0;
    for (unsigned i = 0; i < width; ++i) value = value * 256u + bytes[i];
    require(trace_count < 4, "too many memory accesses");
    trace[trace_count].p = p; trace[trace_count].value = value;
    trace[trace_count].width = width; trace[trace_count].write = 0;
    trace[trace_count++].route = route;
    return value;
}
static void access_write(uint32_t p, uint32_t value, unsigned width, unsigned route) {
    uint8_t *bytes = backing(p, width, route);
    require(trace_count < 4, "too many memory accesses");
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
    unsigned store, update, w, gqr, a, b;
} Op;
typedef struct {
    const char *name;
    uint32_t gqr;
    unsigned width;
    uint8_t bytes[8];
    uint64_t pair[2], scalar[2], source[2];
} Format;
typedef struct {
    unsigned op;
    const char *name;
    uint32_t base, rb, expected_ra, physical;
    unsigned route;
} Case;
'''


C_MAIN = r'''
int main(void) {
    unsigned checked = 0;
    memset(gc_ram, 0xa5, sizeof(gc_ram));
    for (unsigned c = 0; c < sizeof(cases)/sizeof(cases[0]); ++c) {
        const Case *test = &cases[c];
        const Op *op = &ops[test->op];
        current_op = op->name; current_address = test->name;
        for (unsigned f = 0; f < sizeof(formats)/sizeof(formats[0]); ++f) {
            const Format *format = &formats[f];
            current_format = format->name;
            memset(lc, 0xa5, sizeof(lc)); memset(mmio, 0xa5, sizeof(mmio));
            memset(gc_ram + 0x6040 - 8, 0xa5, 24);
            memset(&gc, 0, sizeof(gc));
            for (unsigned i = 0; i < 32; ++i) {
                gc.r[i] = 0x12340000u + i;
                gc.fpr_bits[i] = 0x4050000040200000ull;
                gc.fpr_ps1_bits[i] = 0xc010000000000000ull;
            }
            for (unsigned i = 0; i < 8; ++i) gc.gqr[i] = 0x3f043f07u;
            gc.gqr[op->gqr] = format->gqr;
            gc.r[op->a] = test->base; gc.r[op->b] = test->rb;
            gc.fpr_bits[3] = format->source[0];
            gc.fpr_ps1_bits[3] = format->source[1];
            gc.cr = 0x89abcdefu; gc.xer = 0xe155aa7fu;
            GcCtx expected = gc;
            expected.r[op->a] = test->expected_ra;
            if (!op->store) {
                const uint64_t *lanes = op->w ? format->scalar : format->pair;
                expected.fpr_bits[3] = lanes[0];
                expected.fpr_ps1_bits[3] = lanes[1];
            }
            uint8_t *target = test->route ? backing(test->physical, 8, test->route)
                                         : gc_ram + test->physical;
            if (!op->store) memcpy(target, format->bytes, 2 * format->width);
            uint8_t wanted_lc[sizeof(lc)], wanted_mmio[sizeof(mmio)], wanted_ram[24];
            memcpy(wanted_lc, lc, sizeof(lc)); memcpy(wanted_mmio, mmio, sizeof(mmio));
            memcpy(wanted_ram, gc_ram + 0x6040 - 8, sizeof(wanted_ram));
            unsigned count = op->w ? 1 : 2;
            if (op->store) {
                uint8_t *wanted = test->route == 1 ? wanted_lc + (test->physical - 0x01800000u)
                                  : test->route == 2 ? wanted_mmio : wanted_ram + 8;
                memcpy(wanted, format->bytes, count * format->width);
            }
            trace_count = 0;
            op->run();
            /* Check lowMEM first: the old EA mask overwrote the context here. */
            for (unsigned i = 0; i < 0x4000; ++i)
                require(gc_ram[i] == 0xa5, "lowMEM sentinel overwritten");
            require(memcmp(lc, wanted_lc, sizeof(lc)) == 0,
                    "locked-cache bytes/guards differ");
            require(memcmp(mmio, wanted_mmio, sizeof(mmio)) == 0,
                    "WGPIPE bytes/guards differ");
            require(memcmp(gc_ram + 0x6040 - 8, wanted_ram, sizeof(wanted_ram)) == 0,
                    "RAM bytes/guards differ");
            require(memcmp(&gc, &expected, sizeof(gc)) == 0,
                    "CPU state differs (loaded pair, RA EA alias, or unrelated registers)");
            require(trace_count == (test->route ? count : 0),
                    "scalar/paired access count or routing differs");
            for (unsigned i = 0; i < trace_count; ++i) {
                require(trace[i].p == test->physical + i * format->width,
                        "physical address or quantized element stride differs");
                require(trace[i].width == format->width && trace[i].write == op->store &&
                        trace[i].route == test->route, "access width/direction/route differs");
                uint32_t value = 0;
                for (unsigned b = 0; b < format->width; ++b)
                    value = value * 256u + format->bytes[i * format->width + b];
                require(trace[i].value == value, "big-endian access value differs");
            }
            ++checked;
        }
    }
    printf("PASS PSQ: %u execution cases; %zu emitter variants; 0 failures\n",
           checked, sizeof(ops)/sizeof(ops[0]));
    return 0;
}
'''


def main():
    items = list(variants())
    cc = shlex.split(os.environ.get("CC", "clang"))
    modes = (("-O0", ["-O0"]), ("-O2", ["-O2"]),
             ("ASan+UBSan", ["-O1", "-g", "-fsanitize=address,undefined",
                             "-fno-sanitize-recover=all", "-fno-omit-frame-pointer"]))
    with tempfile.TemporaryDirectory(prefix="gekko-psq-") as temporary:
        directory = Path(temporary)
        source = directory / "psq.c"
        source.write_text(harness(items))
        for label, flags in modes:
            binary = directory / ("psq_" + label)
            compile_c(cc, source, binary, flags)
            run = subprocess.run([str(binary)], capture_output=True, text=True,
                                 check=True, timeout=30)
            print(f"{label} {run.stdout.strip()}", flush=True)
    print("LC lowMEM guards, RAM/WGPIPE aliases, W=0/1, all GQR types, "
          "signed scales/displacements, and EA-preserving RA updates passed.")


if __name__ == "__main__":
    try:
        main()
    except subprocess.CalledProcessError as error:
        print(f"Compiler/execution failed: {error}\n{error.stderr}", file=sys.stderr)
        sys.exit(2)
