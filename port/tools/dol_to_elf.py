#!/usr/bin/env python3
"""Convert a GameCube DOL into a big-endian PPC32 ELF for Ghidra analysis.

Keeps the DOL's memory addresses (0x80003100+), so analysis results
(function addresses, xrefs) transfer 1:1 back to the recomp project.
BSS becomes a NOBITS section so Ghidra knows the extent without bloating
the file.
"""
import struct
import sys

EHDR_SZ = 52
PHDR_SZ = 32


def main(dol_path, elf_path):
    d = open(dol_path, "rb").read()
    text_off = struct.unpack_from(">7I", d, 0x00)
    data_off = struct.unpack_from(">11I", d, 0x1C)
    text_addr = struct.unpack_from(">7I", d, 0x48)
    data_addr = struct.unpack_from(">11I", d, 0x64)
    text_size = struct.unpack_from(">7I", d, 0x90)
    data_size = struct.unpack_from(">11I", d, 0xAC)
    bss_addr, bss_size, entry = struct.unpack_from(">III", d, 0xD8)

    segs = []
    for i in range(7):
        if text_size[i]:
            segs.append(("text", text_off[i], text_addr[i], text_size[i]))
    for i in range(11):
        if data_size[i]:
            segs.append(("data", data_off[i], data_addr[i], data_size[i]))
    segs.append(("bss", 0, bss_addr, bss_size))
    n = len(segs)

    # file layout: ehdr | phdrs | segment data
    data_start = EHDR_SZ + PHDR_SZ * n
    off = data_start
    body = bytearray()
    phdrs = bytearray()
    for kind, foff, vaddr, size in segs:
        flags = 5 if kind == "text" else 6 if kind == "data" else 6
        ptype = 1 if kind != "bss" else 1
        if kind == "bss":
            phdrs += struct.pack(">8I", ptype, off, vaddr, vaddr, 0, size, flags, 0x100)
        else:
            seg = d[foff:foff + size]
            phdrs += struct.pack(">8I", ptype, off, vaddr, vaddr, size, size, flags, 0x100)
            body += seg
            off += size

    ident = b"\x7fELF" + bytes([1, 2, 1, 0]) + b"\x00" * 8
    ehdr = ident + struct.pack(">HHIIIIIHHHHHH",
                               2, 20, 1, entry, EHDR_SZ, 0, 0,
                               EHDR_SZ, PHDR_SZ, n, 40, 0, 0)
    open(elf_path, "wb").write(ehdr + phdrs + body)
    print(f"wrote {elf_path}: {n} phdrs, entry 0x{entry:08X}, "
          f"file size {len(ehdr) + len(phdrs) + len(body)}")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
