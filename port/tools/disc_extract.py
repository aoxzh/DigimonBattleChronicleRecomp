#!/usr/bin/env python3
"""GameCube disc image parser: extract FST file tree + main.dol + apploader.

Outputs:
  extracted/main.dol          - executable
  extracted/files.json        - [{path, offset, size}] for runtime DI HLE
  extracted/root/...          - all files on disc
  analysis/disc_info.json     - boot fields, bi2, apploader info
"""
import json
import struct
import sys
from pathlib import Path


def be32(b, o):
    return struct.unpack_from(">I", b, o)[0]


def main(iso_path: str, out_dir: str):
    iso = open(iso_path, "rb")
    out = Path(out_dir)
    (out / "extracted" / "root").mkdir(parents=True, exist_ok=True)
    (out / "analysis").mkdir(parents=True, exist_ok=True)

    head = iso.read(0x440)
    dol_off = be32(head, 0x420)
    fst_off = be32(head, 0x424)
    fst_size = be32(head, 0x428)
    fst_max = be32(head, 0x42C)

    iso.seek(0x2440)
    appl = iso.read(0x20)
    appl_entry, appl_size, appl_trail = struct.unpack(">III", appl[:12])
    iso.seek(0x2460)
    appl_ver = iso.read(0x20)

    # bi2.bin at 0x440, 0x2000 bytes
    iso.seek(0x440)
    bi2 = iso.read(0x2000)
    bi2_fields = struct.unpack(">7I", bi2[:28])
    # disk time @0x2440+0x18? boot.bin has timestamps; keep boot.bin whole
    iso.seek(0)
    boot_bin = iso.read(0x440)

    # FST
    iso.seek(fst_off)
    fst = iso.read(fst_size)
    n_entries = be32(fst, 8)
    str_tab = fst[n_entries * 12:]

    def name_of(off):
        end = str_tab.index(b"\0", off)
        return str_tab[off:end].decode("shift_jis", "replace")

    entries = []
    for i in range(n_entries):
        e = fst[i * 12:(i + 1) * 12]
        kind = e[0]
        name_off = int.from_bytes(e[1:4], "big")
        a, b = struct.unpack(">II", e[4:12])
        entries.append((kind, name_off, a, b))

    files = []
    # walk tree from root (entry 0)
    def walk(idx, prefix):
        kind, _, a, b = entries[idx]
        assert kind == 1
        next_dir = b
        i = idx + 1
        while i < next_dir:
            k, noff, fa, fb = entries[i]
            nm = name_of(noff)
            if k == 0:
                path = f"{prefix}{nm}"
                files.append({"path": path, "offset": fa, "size": fb, "entry": i})
                i += 1
            else:
                walk(i, f"{prefix}{nm}/")
                i = entries[i][3]  # next_dir of this dir

    walk(0, "")
    print(f"FST: {n_entries} entries, {len(files)} files")

    # extract all files
    for fe in files:
        dst = out / "extracted" / "root" / fe["path"]
        dst.parent.mkdir(parents=True, exist_ok=True)
        iso.seek(fe["offset"])
        data = iso.read(fe["size"])
        dst.write_bytes(data)

    # main.dol
    iso.seek(dol_off)
    dol_size = fst_off - dol_off
    dol = iso.read(dol_size)
    (out / "extracted" / "main.dol").write_bytes(dol)

    # parse DOL header
    d = dol[:0x100]
    text_off = struct.unpack(">7I", d[0x00:0x1C])
    data_off = struct.unpack(">11I", d[0x1C:0x48])
    text_addr = struct.unpack(">7I", d[0x48:0x64])
    data_addr = struct.unpack(">11I", d[0x64:0x90])
    text_size = struct.unpack(">7I", d[0x90:0xAC])
    data_size = struct.unpack(">11I", d[0xAC:0xD8])
    bss_addr, bss_size, entry = struct.unpack(">III", d[0xD8:0xE4])
    print(f"DOL: entry=0x{entry:08X} bss=0x{bss_addr:08X}+0x{bss_size:X}")
    total_text = total_data = 0
    mem_lo = 0xFFFFFFFF
    mem_hi = 0
    for i in range(7):
        if text_size[i]:
            print(f"  text[{i}] file=0x{text_off[i]:X} addr=0x{text_addr[i]:08X} size=0x{text_size[i]:X}")
            total_text += text_size[i]
            mem_lo = min(mem_lo, text_addr[i])
            mem_hi = max(mem_hi, text_addr[i] + text_size[i])
    for i in range(11):
        if data_size[i]:
            print(f"  data[{i}] file=0x{data_off[i]:X} addr=0x{data_addr[i]:08X} size=0x{data_size[i]:X}")
            total_data += data_size[i]
            mem_lo = min(mem_lo, data_addr[i])
            mem_hi = max(mem_hi, data_addr[i] + data_size[i])
    print(f"  text total 0x{total_text:X}, data total 0x{total_data:X}, "
          f"mem 0x{mem_lo:08X}-0x{mem_hi:08X}, bss end 0x{bss_addr + bss_size:08X}")

    info = {
        "game_id": head[0:6].decode("ascii", "replace"),
        "game_name": head[0x20:0x3E].decode("shift_jis", "replace").rstrip("\0"),
        "dol_offset": dol_off, "dol_size": dol_size, "fst_offset": fst_off,
        "fst_size": fst_size, "fst_max_size": fst_max,
        "apploader": {"entry": appl_entry, "size": appl_size, "trailer": appl_trail},
        "bi2": {"disk_id": bi2_fields[0], "game_code": bi2_fields[1],
                "region": bi2_fields[2], "maker": bi2_fields[3], "disk_version": bi2_fields[6]},
        "dol": {"entry": entry, "bss_addr": bss_addr, "bss_size": bss_size,
                "text": [{"file": text_off[i], "addr": text_addr[i], "size": text_size[i]}
                         for i in range(7) if text_size[i]],
                "data": [{"file": data_off[i], "addr": data_addr[i], "size": data_size[i]}
                         for i in range(11) if data_size[i]],
                "mem_lo": mem_lo, "mem_hi": max(mem_hi, bss_addr + bss_size)},
        "n_files": len(files),
    }
    (out / "analysis" / "disc_info.json").write_text(json.dumps(info, indent=1))
    (out / "extracted" / "files.json").write_text(json.dumps(files, indent=1))
    print("game:", info["game_id"], info["game_name"], "region", bi2_fields[2])
    print(f"wrote extracted/ ({len(files)} files), analysis/disc_info.json")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
