#!/usr/bin/env python3
"""Assemble the self-build kit: our source + build scripts, nothing from the disc.

    python3 packaging/make_kit.py [--out dist]

Copies an explicit whitelist of port/ into <out>/DigimonBattleGC-builder-<version>/src,
adds build.py, build-windows.ps1 and the README, then writes a .zip and SHA256SUMS.
Refuses to finish if anything game-derived (lifter output, disc/DOL files, images,
logs, card images) ended up in the kit.
"""
import argparse
import fnmatch
import hashlib
import os
import shutil
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
PORT = os.path.dirname(HERE)

# (source directory or file relative to port/, file patterns)
INCLUDE = [
    ("CMakeLists.txt", None),
    ("hal", ["*.c", "*.h"]),
    ("recomp/runtime_glue.c", None),
    ("tools/lift_dol.py", None),
    ("tools/prepare_disc.py", None),
    ("tools/rvz_extract.py", None),
    ("tools/prepare-disc.bat", None),
    ("tools/prepare-disc.sh", None),
    ("tools/extra_roots.txt", None),
    ("tools/probe_blocks.txt", None),
    ("tools/gekko", ["*.py"]),
    ("platform/desktop", ["*.c", "*.cpp", "*.h", "*.md", "*.rc", "*.manifest", "launcher_bg.jpg"]),
    ("third_party/stb", ["stb_image.h", "COMMIT"]),
    ("third_party/gcn-recomp", ["*.c", "*.cpp", "*.h", "LICENSE", "*.md", "*.json"]),
    ("third_party/imgui", ["*.cpp", "*.h", "LICENSE.txt", "*.md"]),
]
KIT_FILES = ["build.py", "build-windows.ps1", "Build-Windows.bat", "README.md", "VERSION"]
# Lifter output and anything else made from the game must never be packaged.
FORBIDDEN = ["b_*.c", "blocks.c", "block_decls.h", "gen_prefix.h", "recomp_entry.c",
             "coverage.json", "*.dol", "*.iso", "*.gcm", "*.rvz", "*.raw", "*.png", "*.log",
             "*.bin", "*.elf", "*.o", "*.obj", "*.exe"]


def copy_tree(rel, patterns, dst_root):
    src = os.path.join(PORT, rel)
    if patterns is None:
        os.makedirs(os.path.dirname(os.path.join(dst_root, rel)), exist_ok=True)
        shutil.copy2(src, os.path.join(dst_root, rel))
        return 1
    n = 0
    for root, dirs, files in os.walk(src):
        dirs[:] = [d for d in dirs if d not in ("__pycache__", "build", "generated")]
        for name in files:
            if any(fnmatch.fnmatch(name, p) for p in patterns):
                path = os.path.join(root, name)
                out = os.path.join(dst_root, os.path.relpath(path, PORT))
                os.makedirs(os.path.dirname(out), exist_ok=True)
                shutil.copy2(path, out)
                n += 1
    return n


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join(PORT, "dist"))
    args = ap.parse_args()
    with open(os.path.join(HERE, "VERSION"), encoding="utf-8") as f:
        version = f.read().strip()
    name = f"DigimonBattleGC-builder-{version}"
    kit = os.path.join(os.path.abspath(args.out), name)
    shutil.rmtree(kit, ignore_errors=True)
    src = os.path.join(kit, "src")
    total = sum(copy_tree(rel, pats, src) for rel, pats in INCLUDE)
    for f in KIT_FILES:
        shutil.copy2(os.path.join(HERE, f), os.path.join(kit, f))
    bad = []
    for root, _, files in os.walk(kit):
        for f in files:
            if any(fnmatch.fnmatch(f, p) for p in FORBIDDEN):
                bad.append(os.path.relpath(os.path.join(root, f), kit))
    if bad:
        raise SystemExit("refusing to package game-derived files:\n  " + "\n  ".join(bad))
    archive = kit + ".zip"
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for root, _, files in os.walk(kit):
            for f in sorted(files):
                path = os.path.join(root, f)
                z.write(path, os.path.join(name, os.path.relpath(path, kit)))
    with open(os.path.join(os.path.abspath(args.out), "SHA256SUMS"), "w", encoding="utf-8", newline="\n") as f:
        f.write(f"{sha256(archive)}  {os.path.basename(archive)}\n")
    print(f"{total} source files + {len(KIT_FILES)} kit files -> {archive} "
          f"({os.path.getsize(archive) / 1e6:.1f} MB)")


if __name__ == "__main__":
    main()
