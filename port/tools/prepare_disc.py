#!/usr/bin/env python3
"""Import a GameCube disc image for the desktop runner.

Mainstream GameCube recomp ports accept the disc formats users actually keep
(ISO/GCM/RVZ/GCZ/CISO).  The runner can read raw ISO/GCM directly; this helper
turns a compressed image into the cached raw ISO that settings_resolve_disc()
looks for:

    <data dir>/cache/<image stem>.iso

The cache name intentionally matches the C side in
platform/desktop/upstream_gx/settings.c.  Run with --check to validate an image
without writing anything.
"""
from __future__ import annotations

import argparse
import os
import struct
import sys
import zlib
from pathlib import Path

GAME_ID = "G2DJB2"
GAME_DOL_SIZE = 3_591_200
GAME_DOL_FNV = 0x5FD8E0C4B410A2D2
GC_MAGIC = b"\xc2\x33\x9f\x3d"


def default_cache_dir() -> Path:
    """Match SDL_GetPrefPath("", "DigimonBattleGC") on the three desktop OSes."""
    if sys.platform == "win32":
        base = os.environ.get("APPDATA")
        if base:
            return Path(base) / "DigimonBattleGC" / "cache"
    elif sys.platform == "darwin":
        return Path.home() / "Library" / "Application Support" / "DigimonBattleGC" / "cache"
    base = os.environ.get("XDG_DATA_HOME")
    return (Path(base) if base else Path.home() / ".local" / "share") / "DigimonBattleGC" / "cache"


def _be32(blob: bytes, off: int) -> int:
    return int.from_bytes(blob[off:off + 4], "big")


def _dol_extent(dol_header: bytes) -> int:
    n = 0x100
    for i in range(18):
        size = _be32(dol_header, 0x90 + 4 * i)
        if size:
            n = max(n, _be32(dol_header, 4 * i) + size)
    return n


def validate_iso(path: Path) -> dict:
    """Validate the exact DOL the generated code was lifted from."""
    with path.open("rb") as f:
        header = f.read(0x440)
        if len(header) != 0x440:
            raise ValueError("file is too small to be a GameCube disc image")
        if header[0x1C:0x20] != GC_MAGIC:
            raise ValueError("missing GameCube DVD magic (not an ISO/GCM?)")
        game_id = header[0:6].decode("ascii", "replace")
        if game_id != GAME_ID:
            raise ValueError(f"this is {game_id!r}; the port needs {GAME_ID}")
        revision = header[7]
        if revision != 0:
            raise ValueError(f"disc revision {revision} found; the recompiled build is revision 0")
        dol_off = _be32(header, 0x420)
        f.seek(dol_off)
        dol_header = f.read(0x100)
        if len(dol_header) != 0x100:
            raise ValueError("truncated main.dol header")
        dol_size = _dol_extent(dol_header)
        if dol_size != GAME_DOL_SIZE:
            raise ValueError(f"main.dol is {dol_size} bytes; expected {GAME_DOL_SIZE}")
        f.seek(dol_off)
        fnv = 14695981039346656037
        left = dol_size
        while left:
            block = f.read(min(left, 1 << 20))
            if not block:
                raise ValueError("truncated main.dol")
            for byte in block:
                fnv ^= byte
                fnv = (fnv * 1099511628211) & 0xFFFFFFFFFFFFFFFF
            left -= len(block)
        if fnv != GAME_DOL_FNV:
            raise ValueError("main.dol differs from the recompiled build (bad dump or modified disc)")
    return {
        "game_id": game_id,
        "revision": revision,
        "dol_size": GAME_DOL_SIZE,
        "dol_fnv": f"{GAME_DOL_FNV:016x}",
    }


def cache_path(image: Path, cache_dir: Path) -> Path:
    return cache_dir / (image.stem + ".iso")


def _decode_ciso(image: Path, output: Path) -> None:
    """CISO stores sparse raw blocks behind a 0x8000-byte map."""
    with image.open("rb") as f:
        head = f.read(8)
        if len(head) != 8 or head[0:4] != b"CISO":
            raise ValueError("not a CISO image")
        block_size = struct.unpack_from("<I", head, 4)[0]
        if block_size < 0x1000 or block_size > 0x100000:
            raise ValueError(f"implausible CISO block size {block_size}")
        block_map = f.read(0x8000 - 8)
        if len(block_map) != 0x8000 - 8:
            raise ValueError("truncated CISO block map")
        if any(v not in (0, 1) for v in block_map):
            raise ValueError("invalid CISO block map entry")
        used = [i for i, v in enumerate(block_map) if v]
        if not used:
            raise ValueError("CISO contains no data blocks")
        output.parent.mkdir(parents=True, exist_ok=True)
        with output.open("wb") as out:
            out.truncate((used[-1] + 1) * block_size)
            for sequence, block in enumerate(used):
                f.seek(0x8000 + sequence * block_size)
                data = f.read(block_size)
                if len(data) != block_size:
                    raise ValueError(f"truncated CISO block {block}")
                out.seek(block * block_size)
                out.write(data)


def _decode_gcz(image: Path, output: Path) -> None:
    """GCZ: zlib blocks with an optional raw-block high bit."""
    with image.open("rb") as f:
        header = f.read(32)
        if len(header) != 32 or header[0:4] not in (b"\x0b\xb1\x0b\xb1", b"\xb1\x0b\xb1\x0b"):
            raise ValueError("not a GCZ image")
        compressed_size, block_size, num_blocks = struct.unpack_from("<QII", header, 8)
        if block_size < 0x1000 or block_size > 0x100000:
            raise ValueError(f"implausible GCZ block size {block_size}")
        if num_blocks == 0 or num_blocks > 0x200000:
            raise ValueError(f"implausible GCZ block count {num_blocks}")
        pointers_blob = f.read(num_blocks * 8)
        hashes = f.read(num_blocks * 4)
        if len(pointers_blob) != num_blocks * 8 or len(hashes) != num_blocks * 4:
            raise ValueError("truncated GCZ block table")
        pointers = struct.unpack_from(f"<{num_blocks}Q", pointers_blob)
        data_offset = 32 + num_blocks * 8 + num_blocks * 4
        raw_flag = 1 << 63
        output.parent.mkdir(parents=True, exist_ok=True)
        with output.open("wb") as out:
            out.truncate(num_blocks * block_size)
            for block in range(num_blocks):
                start_word = pointers[block]
                start = start_word & ~raw_flag
                if block + 1 < num_blocks:
                    end = pointers[block + 1] & ~raw_flag
                else:
                    end = compressed_size
                size = end - start
                if size == 0:
                    continue
                f.seek(data_offset + start)
                blob = f.read(size)
                if len(blob) != size:
                    raise ValueError(f"truncated GCZ block {block}")
                if start_word & raw_flag:
                    if size != block_size:
                        raise ValueError(f"GCZ raw block {block} has wrong size")
                    data = blob
                else:
                    try:
                        data = zlib.decompress(blob)
                    except zlib.error as exc:
                        raise ValueError(f"GCZ block {block} is not valid zlib") from exc
                if len(data) > block_size:
                    raise ValueError(f"GCZ block {block} expands past block size")
                out.seek(block * block_size)
                out.write(data.ljust(block_size, b"\0"))


def _import_rvz(image: Path, output: Path) -> None:
    try:
        import zstandard  # noqa: F401
    except ImportError as exc:
        raise SystemExit(
            "RVZ import needs the 'zstandard' module: python -m pip install zstandard"
        ) from exc
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import rvz_extract  # type: ignore  # local tool

    output.parent.mkdir(parents=True, exist_ok=True)
    rvz_extract.main(str(image), str(output))


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path, help="disc image (.iso/.gcm/.rvz/.gcz/.ciso)")
    parser.add_argument("--check", action="store_true", help="validate only; write nothing")
    parser.add_argument("--force", action="store_true", help="re-extract an existing cache")
    parser.add_argument("--out", type=Path, help="explicit output ISO (default: data cache)")
    parser.add_argument("--cache-dir", type=Path, help="override the data cache directory")
    args = parser.parse_args(argv)

    image = args.image.expanduser()
    if not image.is_file():
        print(f"error: no such file: {image}", file=sys.stderr)
        return 2

    if args.check:
        try:
            info = validate_iso(image)
        except (OSError, ValueError) as exc:
            print(f"error: {exc}", file=sys.stderr)
            return 1
        print(f"OK: {info['game_id']} rev {info['revision']}, main.dol {info['dol_size']} bytes "
              f"fnv {info['dol_fnv']}")
        return 0

    suffix = image.suffix.lower()
    if suffix in (".iso", ".gcm"):
        try:
            info = validate_iso(image)
        except (OSError, ValueError) as exc:
            print(f"error: {exc}", file=sys.stderr)
            return 1
        print(f"OK: raw image already usable by the runner: {image.resolve()}")
        print(f"    {info['game_id']} rev {info['revision']}")
        return 0

    if suffix not in (".rvz", ".gcz", ".ciso"):
        print(
            f"error: {suffix or 'unknown format'} is not imported yet. "
            "RVZ/GCZ/CISO are supported directly; convert WIA to ISO with Dolphin "
            "(right-click the game > Convert File).",
            file=sys.stderr,
        )
        return 2

    cache_dir = (args.cache_dir or default_cache_dir()).expanduser()
    output = (args.out or cache_path(image, cache_dir)).expanduser()
    if output.exists() and not args.force:
        try:
            info = validate_iso(output)
        except (OSError, ValueError) as exc:
            print(f"existing import is invalid: {exc}", file=sys.stderr)
            print("re-run with --force to replace it", file=sys.stderr)
            return 1
        print(f"OK: already imported: {output.resolve()}")
        print(f"    {info['game_id']} rev {info['revision']}")
        return 0

    if output.exists():
        output.unlink()
    print(f"importing {image} -> {output}")
    try:
        if suffix == ".rvz":
            _import_rvz(image, output)
        elif suffix == ".gcz":
            _decode_gcz(image, output)
        else:
            _decode_ciso(image, output)
        info = validate_iso(output)
    except (OSError, ValueError, SystemExit) as exc:
        if output.exists():
            output.unlink()
        if isinstance(exc, SystemExit):
            raise
        print(f"error: {exc}", file=sys.stderr)
        return 1
    print(f"OK: imported {output.resolve()}")
    print(f"    {info['game_id']} rev {info['revision']}, main.dol {info['dol_size']} bytes fnv {info['dol_fnv']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
