#!/usr/bin/env python3
"""Execute emitted SPR writes against the locked-cache DMA call contract."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

PORT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PORT))
from tools.gekko.decode import decode
from tools.gekko.lift import GEN_PREFIX_H, Lifter


def emit(spr):
    word = (31 << 26) | (3 << 21) | ((spr & 31) << 16) | ((spr >> 5) << 11) | (467 << 1)
    instruction = decode(word)
    assert instruction.name == "mtspr" and instruction.spr == spr
    lifter = Lifter([], [], 0)
    lifter.out = []
    lifter.emit(instruction, 0x80003100)
    return f"static void write_{spr}(void) {{\n" + "\n".join(lifter.out) + "\n}\n"


def main():
    prefix = GEN_PREFIX_H.replace('"../hal/', '"' + str(PORT / "hal") + '/')
    source = prefix + """
#include <assert.h>
GcCtx gc;
uint32_t gc_spr_misc[1024];
static unsigned calls;
static uint32_t upper, lower;
bool hal_lc_dma(uint32_t u, uint32_t l) {
    ++calls; upper=u; lower=l;
    return calls != 2;
}
""" + emit(922) + emit(923) + """
int main(void) {
    gc.r[3]=0x00100003; write_922();
    assert(calls==0 && gc_spr_misc[922]==0x00100003);
    gc.r[3]=0xe0001000; write_923();
    assert(calls==0 && gc_spr_misc[923]==0xe0001000);
    gc.r[3]=0xe000101e; write_923();
    assert(calls==1 && upper==0x00100003 && lower==0xe000101e);
    assert(gc_spr_misc[923]==0xe000101c && gc.r[3]==0xe000101e);
    gc.r[3]=0xe0002002; write_923();
    assert(calls==2 && lower==0xe0002002);
    assert(gc_spr_misc[923]==0xe0002000);
    gc.r[3]=0x80002000; write_922();
    assert(calls==2 && gc_spr_misc[922]==0x80002000);
    return 0;
}
"""
    with tempfile.TemporaryDirectory(prefix="digimon-lc-emitter-") as work:
        c = Path(work) / "test.c"
        c.write_text(source)
        for flags in (("-O0",), ("-O2",), ("-O1", "-fsanitize=address,undefined", "-fno-sanitize-recover=all")):
            binary = Path(work) / "test"
            subprocess.run([os.environ.get("CC", "clang"), "-std=c11", *flags, str(c), "-lm", "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)
            print(" ".join(flags), "PASS LC SPR trigger, arguments, self-clear, rejected transfer")


if __name__ == "__main__":
    main()
