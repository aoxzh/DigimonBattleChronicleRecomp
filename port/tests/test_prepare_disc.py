#!/usr/bin/env python3
"""Regression checks for tools/prepare_disc.py.

The real disc is optional (it is not checked in).  The test still covers the
cache naming contract used by platform/desktop/upstream_gx/settings.c and the
validator's rejection of non-GameCube input.
"""
from __future__ import annotations

import importlib.util
import struct
import tempfile
import unittest
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("prepare_disc", ROOT / "tools" / "prepare_disc.py")
assert SPEC and SPEC.loader
prepare_disc = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(prepare_disc)


class PrepareDiscTests(unittest.TestCase):
    def test_cache_path_is_stem_iso(self) -> None:
        self.assertEqual(
            prepare_disc.cache_path(Path("Some Game.rvz"), Path("C:/cache")),
            Path("C:/cache/Some Game.iso"),
        )

    def test_rejects_non_gc_file(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            bad = Path(tmp) / "not-a-disc.iso"
            bad.write_bytes(b"not a gamecube image")
            with self.assertRaises(ValueError):
                prepare_disc.validate_iso(bad)

    @unittest.skipUnless((ROOT / "disc.iso").is_file(), "local disc.iso not present")
    def test_local_disc_matches_recompiled_dol(self) -> None:
        info = prepare_disc.validate_iso(ROOT / "disc.iso")
        self.assertEqual(info["game_id"], prepare_disc.GAME_ID)
        self.assertEqual(info["revision"], 0)
        self.assertEqual(info["dol_size"], prepare_disc.GAME_DOL_SIZE)

    def test_ciso_decoder_round_trip(self) -> None:
        block_size = 0x1000
        blocks = [bytes([n]) * block_size for n in range(3)]
        block_map = bytearray(0x8000 - 8)
        block_map[0] = 1
        block_map[2] = 1
        with tempfile.TemporaryDirectory() as tmp:
            image = Path(tmp) / "sample.ciso"
            image.write_bytes(b"CISO" + struct.pack("<I", block_size) + bytes(block_map)
                              + blocks[0] + blocks[2])
            output = Path(tmp) / "sample.iso"
            prepare_disc._decode_ciso(image, output)
            raw = output.read_bytes()
        self.assertEqual(raw[0:block_size], blocks[0])
        self.assertEqual(raw[block_size:2 * block_size], b"\0" * block_size)
        self.assertEqual(raw[2 * block_size:3 * block_size], blocks[2])

    def test_gcz_decoder_round_trip(self) -> None:
        block_size = 0x1000
        blocks = [bytes([n]) * block_size for n in range(3)]
        z0 = zlib.compress(blocks[0])
        z2 = zlib.compress(blocks[2])
        payload = z0 + blocks[1] + z2
        pointers = [
            0,
            (1 << 63) | len(z0),
            len(z0) + block_size,
        ]
        header = (b"\x0b\xb1\x0b\xb1" + struct.pack("<I", 0)
                  + struct.pack("<Q", len(payload))
                  + struct.pack("<I", block_size)
                  + struct.pack("<I", 3) + b"\0" * 8)
        hash_table = b"\0" * (3 * 4)
        with tempfile.TemporaryDirectory() as tmp:
            image = Path(tmp) / "sample.gcz"
            image.write_bytes(header + struct.pack("<3Q", *pointers) + hash_table + payload)
            output = Path(tmp) / "sample.iso"
            prepare_disc._decode_gcz(image, output)
            raw = output.read_bytes()
        self.assertEqual(raw, b"".join(blocks))


if __name__ == "__main__":
    unittest.main()
