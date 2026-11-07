#!/usr/bin/env python3
"""RVZ -> raw disc image decoder (GameCube).

Implements the RVZ 1.0 container per Dolphin's DiscIO:
  - WIAHeader1 (0x48) / WIAHeader2 (0xDC), big-endian, SHA-1 verified
  - zstd-compressed raw-data-entry and group-entry tables
  - RVZGroupEntry { data_offset>>2 u32, data_size u32 (MSB=compressed),
                    rvz_packed_size u32 }
  - RVZ pack layer: BE u32 run sizes, MSB => PRNG junk run seeded by 68 bytes,
    Lagged-Fibonacci generator (k=521, j=32, xor) advanced to offset%0x8000
License note: LFG/Pack semantics reimplemented from Dolphin (GPL-2+/CC0 parts).
"""
import hashlib
import struct
import sys
from pathlib import Path

import zstandard as zstd

MASK32 = 0xFFFFFFFF


class LFG:
    """Lagged Fibonacci generator, k=521 j=32, xor variant (Dolphin DiscIO)."""
    K, J = 521, 32
    WIN = K * 4

    def __init__(self):
        self.b = [0] * self.K
        self.pos = 0
        self.win = None

    def _forward(self):
        K, J = self.K, self.J
        for i in range(J):
            self.b[i] ^= self.b[i + K - J]
        for i in range(J, K):
            self.b[i] ^= self.b[i - J]
        self.win = None

    def set_seed(self, seed68: bytes):
        b = [int.from_bytes(seed68[i * 4:i * 4 + 4], "big") for i in range(17)]
        b += [0] * (self.K - 17)
        for i in range(17, self.K):
            b[i] = ((b[i - 17] << 23) ^ (b[i - 16] >> 9) ^ (b[i - 1])) & MASK32
        for i in range(self.K):
            x = b[i]
            y = (x & 0xFF00FFFF) | ((x >> 2) & 0x00FF0000)
            # stored word = swap32(y); output bytes are the BE serialization of y
            b[i] = int.from_bytes(y.to_bytes(4, "big"), "little")
        self.b = b
        self.pos = 0
        self.win = None
        for _ in range(4):
            self._forward()

    def _window(self) -> bytes:
        if self.win is None:
            self.win = b"".join(w.to_bytes(4, "little") for w in self.b)
        return self.win

    def get_bytes(self, n: int) -> bytes:
        out = bytearray()
        while n > 0:
            win = self._window()
            take = min(n, self.WIN - self.pos)
            out += win[self.pos:self.pos + take]
            self.pos += take
            n -= take
            if self.pos == self.WIN:
                self.pos = 0
                self._forward()
        return bytes(out)

    def skip(self, n: int):
        while n > 0:
            take = min(n, self.WIN - self.pos)
            self.pos += take
            n -= take
            if self.pos == self.WIN:
                self.pos = 0
                self._forward()


def rvz_unpack(data: bytes, data_offset: int, expected: int) -> bytes:
    """Expand one RVZ pack stream. data_offset = disc offset of this pack."""
    out = bytearray()
    i = 0
    off = data_offset
    while i + 4 <= len(data) and len(out) < expected:
        (sz,) = struct.unpack_from(">I", data, i)
        i += 4
        if sz & 0x80000000:
            n = min(sz & 0x7FFFFFFF, expected - len(out))
            if i + 68 > len(data):
                raise ValueError("truncated junk seed")
            lfg = LFG()
            lfg.set_seed(data[i:i + 68])
            i += 68
            lfg.skip(off % 0x8000)
            out += lfg.get_bytes(n)
        else:
            n = min(sz, expected - len(out))
            out += data[i:i + n]
            i += sz
        off += n
    if len(out) != expected:
        raise ValueError(f"rvz pack short: {len(out)} != {expected}")
    return bytes(out)


def be(b: bytes, off: int, n: int) -> int:
    return int.from_bytes(b[off:off + n], "big")


def main(rvz_path: str, iso_path: str):
    f = open(rvz_path, "rb")
    h1 = f.read(0x48)
    assert h1[:4] == b"RVZ\x01", "not an RVZ v1 file"
    version = be(h1, 4, 4)
    header_2_size = be(h1, 0x0C, 4)
    h1_hash_stored = h1[0x34:0x48]
    assert hashlib.sha1(h1[:0x34]).digest() == h1_hash_stored, "header1 hash mismatch"

    h2 = f.read(header_2_size)
    assert hashlib.sha1(h2).digest() == h1[0x10:0x24], "header2 hash mismatch"

    disc_type = be(h2, 0x00, 4)
    comp_type = be(h2, 0x04, 4)
    chunk_size = be(h2, 0x0C, 4)
    disc_header = h2[0x10:0x90]
    iso_size = be(h1, 0x24, 8)
    n_part = be(h2, 0x90, 4)
    n_raw = be(h2, 0xB4, 4)
    raw_off = be(h2, 0xB8, 8)
    raw_size = be(h2, 0xC0, 4)
    n_groups = be(h2, 0xC4, 4)
    grp_off = be(h2, 0xC8, 8)
    grp_size = be(h2, 0xD0, 4)
    comp_names = {0: "none", 1: "purge", 2: "bzip2", 3: "lzma", 4: "lzma2", 5: "zstd"}
    print(f"disc_type={disc_type} comp={comp_names.get(comp_type)} chunk=0x{chunk_size:x} "
          f"iso_size=0x{iso_size:x} n_part={n_part} n_raw={n_raw} n_groups={n_groups}")
    assert disc_type == 1 and comp_type == 5 and n_part == 0, "expect GameCube zstd, no partitions"

    dctx = zstd.ZstdDecompressor()

    def zdecomp(blob: bytes, want: int) -> bytes:
        out = dctx.decompress(blob, max_output_size=want + 64)
        return out

    # raw data entry table (zstd-compressed in RVZ)
    f.seek(raw_off)
    raw_tab = zdecomp(f.read(raw_size), n_raw * 0x18 + 64)
    assert len(raw_tab) >= n_raw * 0x18, f"raw table short {len(raw_tab)}"
    # group entry table (RVZGroupEntry = 12 bytes)
    f.seek(grp_off)
    grp_tab = zdecomp(f.read(grp_size), n_groups * 12 + 64)
    assert len(grp_tab) >= n_groups * 12, f"group table short {len(grp_tab)}"

    groups = []
    for g in range(n_groups):
        o = g * 12
        off4, dsize, packed = struct.unpack_from(">III", grp_tab, o)
        groups.append((off4 * 4, dsize, packed))

    out_f = open(iso_path, "wb")
    out_f.truncate(iso_size)
    written = 0
    for e in range(n_raw):
        o = e * 0x18
        data_offset, data_size = struct.unpack_from(">QQ", raw_tab, o)
        group_index, n_grp = struct.unpack_from(">II", raw_tab, o + 16)
        start = data_offset & ~0x7FFF  # round down to 0x8000
        end = data_offset + data_size
        pos = start
        print(f"raw[{e}] off=0x{data_offset:x} size=0x{data_size:x} groups={n_grp}")
        for g in groups[group_index:group_index + n_grp]:
            expected = min(chunk_size, end - pos)
            if expected <= 0:
                break
            goff, dsize, packed = g
            if dsize == 0:  # all-zero chunk
                pos += expected
                continue
            f.seek(goff)
            stored = f.read(dsize & 0x7FFFFFFF)
            if dsize & 0x80000000:
                dec = zdecomp(stored, packed if packed else expected)
            else:
                dec = stored
            if packed:
                chunk = rvz_unpack(dec, pos, expected)
            else:
                chunk = dec[:expected]
            out_f.seek(pos)
            out_f.write(chunk)
            written += len(chunk)
            pos += len(chunk)
        if pos < end:
            raise ValueError(f"raw[{e}] groups exhausted at 0x{pos:x} < 0x{end:x}")

    out_f.close()
    # validation: embedded disc header must match decompressed image head
    with open(iso_path, "rb") as vf:
        head = vf.read(0x80)
    assert head == disc_header, "disc header mismatch"
    assert be(head, 0x1C, 4) == 0xC2339F3D, "DVD magic missing"
    print(f"OK: wrote {written} bytes of real data; iso {iso_path} ({iso_size} bytes, "
          f"rest pre-zeroed); header verified")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
