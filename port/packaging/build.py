#!/usr/bin/env python3
"""Build Digimon Battle Chronicle (GameCube, Japan) for this computer from your own disc.

    python3 build.py ["Digimon Battle Chronicle (Japan).iso"] [--portable] [--out DIR] [--no-zip]

With no disc argument it looks for one (a file dialog, or the common download/game
folders, or asks). Steps: check the disc image, read main.dol from it, translate the
game's PowerPC code to C (tools/lift_dol.py), compile it with the HAL, renderer and
launcher, put the program in a folder ready to play, and zip that folder for your own
backup. Nothing from the disc ships with this kit; everything game-derived is made
here, on your machine, from your copy.

If a step fails, build-report-<time>.txt next to build.py records what was run, the
tool versions and the full build output, ready to attach to a bug report. It holds no
game data; the disc is named by its file name only.

Needs Python 3.8+, CMake 3.20+, Ninja, clang/clang++ and SDL2 development files.
On Windows run build-windows.ps1 (or double-click Build-Windows.bat), which sets these
up through MSYS2.
"""
import argparse
import datetime
import json
import os
import platform
import shutil
import struct
import subprocess
import sys
import time
import zipfile
from pathlib import Path

GAME_ID = b"G2DJB2"
# main.dol up to the end of its last section; the generated code and the
# coverage below were produced from exactly this file.
DOL_SIZE = 3591200
DOL_FNV = 0x5FD8E0C4B410A2D2
EXPECT_BLOCKS = 165889
EXPECT_INSTRUCTIONS = 873283
COMPRESSED = (".rvz", ".wia", ".gcz", ".ciso", ".wbfs", ".nfs")
IMPORTABLE = (".rvz", ".gcz", ".ciso")
DISC_EXTS = (".iso", ".gcm") + IMPORTABLE


class BuildError(Exception):
    pass


# Everything printed and every command's output also goes here, for the error report.
_LOG = []
_STEP = ["starting"]
_COMMANDS = []


def say(msg):
    print(msg, flush=True)
    _LOG.append(msg)


def fail(msg):
    raise BuildError(msg)


def find_source():
    here = os.path.dirname(os.path.abspath(__file__))
    for cand in (os.path.join(here, "src"), os.path.dirname(here)):
        if os.path.isfile(os.path.join(cand, "CMakeLists.txt")) and \
           os.path.isfile(os.path.join(cand, "tools", "lift_dol.py")):
            return here, cand
    fail("cannot find the source tree (expected src/ next to build.py)")


def kit_version(kit, src):
    for path in (os.path.join(kit, "VERSION"), os.path.join(src, "packaging", "VERSION")):
        try:
            with open(path, encoding="utf-8") as f:
                return f.read().strip()
        except OSError:
            pass
    return "dev"


def fnv64(data):
    h = 14695981039346656037
    for b in data:
        h = ((h ^ b) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h


def disc_header(path):
    """(game id, revision, name) of a plain GameCube image, or None."""
    try:
        with open(path, "rb") as f:
            head = f.read(0x40)
    except OSError:
        return None
    if len(head) < 0x40 or struct.unpack_from(">I", head, 0x1C)[0] != 0xC2339F3D:
        return None
    return head[:6], head[7], head[0x20:0x40].split(b"\0")[0].decode("ascii", "replace")


def read_disc_dol(path):
    """Validate the disc image and return main.dol (to the end of its last section)."""
    if path.lower().endswith(COMPRESSED):
        fail("this compressed Dolphin image type is not supported; convert it to ISO first "
             "(Dolphin: right-click the game > Convert File > ISO).")
    try:
        f = open(path, "rb")
    except OSError as e:
        fail(f"cannot open {path}: {e}")
    with f:
        head = f.read(0x440)
        if len(head) < 0x440 or struct.unpack_from(">I", head, 0x1C)[0] != 0xC2339F3D:
            fail("this file is not a GameCube disc image (.iso/.gcm)")
        game_id, rev = head[:6], head[7]
        name = head[0x20:0x40].split(b"\0")[0].decode("ascii", "replace")
        if game_id != GAME_ID:
            fail(f"this disc is {name} ({game_id.decode('ascii', 'replace')}); "
                 "the kit needs Digimon Battle Chronicle, Japan (G2DJB2)")
        if rev != 0:
            fail(f"disc revision {rev} found; the kit was made for revision 0")
        dol_off = struct.unpack_from(">I", head, 0x420)[0]
        f.seek(dol_off)
        hdr = f.read(0x100)
        offs = struct.unpack_from(">18I", hdr, 0)
        sizes = struct.unpack_from(">18I", hdr, 0x90)
        size = max([0x100] + [o + s for o, s in zip(offs, sizes) if s])
        f.seek(dol_off)
        dol = f.read(size)
    if len(dol) != DOL_SIZE or fnv64(dol) != DOL_FNV:
        fail("main.dol on this disc differs from the one the kit was made for "
             "(modified or bad dump). Re-dump the disc or try another image.")
    return dol


# ---- choosing the disc ------------------------------------------------------------

def scan_for_discs(kit):
    """Plain images of this game, then compressed ones by name, in the usual places."""
    home = Path.home()
    dirs = [Path(kit), Path.cwd(), home / "Downloads", home / "Desktop", home / "Documents",
            home / "Games", home / "Documents" / "Dolphin Emulator" / "Games"]
    found, seen = [], set()
    for d in dirs:
        try:
            entries = list(d.iterdir()) if d.is_dir() else []
        except OSError:
            continue
        for p in entries:
            if p.suffix.lower() not in DISC_EXTS or not p.is_file():
                continue
            key = str(p.resolve()).lower()
            if key in seen:
                continue
            seen.add(key)
            if p.suffix.lower() in IMPORTABLE:
                if "digimon" in p.name.lower() or "g2dj" in p.name.lower():
                    found.append(p)
                continue
            h = disc_header(p)
            if h and h[0] == GAME_ID:
                found.append(p)
    return found


def dialog_pick():
    """A native file dialog where one is available; None if there is none or it is cancelled."""
    patterns = " ".join("*" + e for e in DISC_EXTS)
    try:
        import tkinter
        from tkinter import filedialog
        root = tkinter.Tk()
        root.withdraw()
        path = filedialog.askopenfilename(
            title="Choose your Digimon Battle Chronicle disc image",
            filetypes=[("GameCube disc image", patterns), ("All files", "*")])
        root.destroy()
        return path or None
    except Exception:
        pass
    if shutil.which("zenity") and (os.environ.get("DISPLAY") or os.environ.get("WAYLAND_DISPLAY")):
        try:
            r = subprocess.run(["zenity", "--file-selection", "--title=Choose your disc image",
                                f"--file-filter=GameCube disc | {patterns}"],
                               capture_output=True, text=True)
            if r.returncode == 0 and r.stdout.strip():
                return r.stdout.strip()
        except OSError:
            pass
    return None


def choose_disc(kit):
    found = scan_for_discs(kit)
    if len(found) == 1:
        say(f"      found {found[0]}")
        return str(found[0])
    if len(found) > 1 and sys.stdin.isatty():
        say("      disc images found:")
        for i, p in enumerate(found, 1):
            say(f"        {i}. {p}")
        ans = input("      number to use (Enter = 1): ").strip() or "1"
        if ans.isdigit() and 1 <= int(ans) <= len(found):
            return str(found[int(ans) - 1])
    path = dialog_pick()
    if path:
        return path
    if sys.stdin.isatty():
        path = input("Path of your disc image (.iso/.gcm/.rvz/.gcz/.ciso): ").strip().strip('"')
        if path:
            return path
    fail("no disc image chosen. Pass it on the command line: python build.py <disc image>")


# ---- running tools ---------------------------------------------------------------

def need(tool, hint):
    path = shutil.which(tool)
    if not path:
        fail(f"'{tool}' was not found. {hint}")
    return path


def run(cmd, cwd=None, env=None):
    line = "$ " + " ".join(f'"{c}"' if " " in c else c for c in cmd)
    say(line)
    _COMMANDS.append(line)
    # Output is shown as it comes and kept for the report.
    p = subprocess.Popen(cmd, cwd=cwd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                         text=True, encoding="utf-8", errors="replace", bufsize=1)
    for out in p.stdout:
        sys.stdout.write(out)
        _LOG.append(out.rstrip("\n"))
    p.wait()
    if p.returncode:
        fail(f"command failed with exit code {p.returncode}")


def tool_version(tool, arg="--version"):
    path = shutil.which(tool)
    if not path:
        return "not found"
    try:
        r = subprocess.run([path, arg], capture_output=True, text=True, timeout=20)
        return (r.stdout or r.stderr).strip().splitlines()[0] if (r.stdout or r.stderr) else "?"
    except Exception as e:
        return f"error: {e}"


def write_report(kit, version, disc, error):
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    path = os.path.join(kit, f"build-report-{stamp}.txt")
    lines = [
        "Digimon Battle Chronicle self-build kit: build report",
        f"kit version : {version}",
        f"time        : {datetime.datetime.now().isoformat(timespec='seconds')}",
        f"failed step : {_STEP[0]}",
        f"error       : {error}",
        f"disc file   : {os.path.basename(disc) if disc else '(none)'}",
        f"system      : {platform.platform()} ({platform.machine()})",
        f"python      : {sys.version.split()[0]} {sys.executable}",
        f"cmake       : {tool_version('cmake')}",
        f"ninja       : {tool_version('ninja')}",
        f"clang       : {tool_version('clang')}",
        f"MSYSTEM     : {os.environ.get('MSYSTEM', '')}",
        "",
        "commands run:",
        *("  " + c for c in _COMMANDS),
        "",
        "output (last 400 lines):",
        *_LOG[-400:],
        "",
    ]
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines))
    return path


def zip_folder(folder, archive):
    tmp = archive + ".part"
    base = os.path.basename(folder)
    with zipfile.ZipFile(tmp, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as z:
        for root, _, files in os.walk(folder):
            rel_root = os.path.relpath(root, folder)
            # Saves, settings, logs and the imported disc of a portable build stay out.
            if rel_root == "user" or rel_root.startswith("user" + os.sep):
                continue
            for name in sorted(files):
                path = os.path.join(root, name)
                z.write(path, os.path.join(base, os.path.relpath(path, folder)))
    os.replace(tmp, archive)


# ---- the build ---------------------------------------------------------------------

def build(args, kit, src, version, state):
    work = os.path.abspath(args.work or os.path.join(kit, "work"))
    out = os.path.abspath(args.out or os.path.join(kit, "DigimonBattleGC"))
    windows = os.name == "nt"
    hint = ("Run build-windows.ps1, which installs the MSYS2 CLANG64 toolchain."
            if windows else "Install: clang cmake ninja pkg-config SDL2 development package (see README).")
    _STEP[0] = "checking build tools"
    cmake = need("cmake", hint)
    ninja = need("ninja", hint)
    cc, cxx = need("clang", hint), need("clang++", hint)
    t0 = time.time()

    _STEP[0] = "choosing the disc image"
    say("[1/5] Checking/importing the disc image")
    disc = args.disc or choose_disc(kit)
    state["disc"] = disc
    if not os.path.isfile(disc):
        fail(f"{disc} does not exist")
    if disc.lower().endswith(IMPORTABLE):
        _STEP[0] = "importing the compressed disc image"
        sys.path.insert(0, os.path.join(src, "tools"))
        import prepare_disc  # local kit helper

        cache_dir = (os.path.join(out, "user", "cache") if args.portable
                     else str(prepare_disc.default_cache_dir()))
        cache = str(prepare_disc.cache_path(Path(disc), Path(cache_dir)))
        run([sys.executable, os.path.join(src, "tools", "prepare_disc.py"),
             disc, "--cache-dir", cache_dir])
        disc = cache
        say(f"      compressed input imported to {cache}")
    _STEP[0] = "checking the disc image"
    dol = read_disc_dol(disc)
    os.makedirs(work, exist_ok=True)
    dol_path = os.path.join(work, "main.dol")
    with open(dol_path, "wb") as f:
        f.write(dol)
    say("      Digimon Battle Chronicle (G2DJB2, rev 0): OK")

    _STEP[0] = "translating PowerPC code (lift_dol.py)"
    say("[2/5] Translating the game's PowerPC code to C")
    recomp = os.path.join(src, "recomp")
    gen = os.path.join(recomp, "generated")
    shutil.rmtree(gen, ignore_errors=True)
    env = dict(os.environ, PYTHONPATH=src, PYTHONUTF8="1")
    tools = os.path.join(src, "tools")
    run([sys.executable, os.path.join(tools, "lift_dol.py"), dol_path, "-o", recomp,
         "--extra-roots-file", os.path.join(tools, "extra_roots.txt"),
         "--hook-blocks", os.path.join(tools, "probe_blocks.txt")], env=env)
    with open(os.path.join(recomp, "coverage.json"), encoding="utf-8") as f:
        cov = json.load(f)
    if cov.get("blocks") != EXPECT_BLOCKS or cov.get("instructions") != EXPECT_INSTRUCTIONS:
        fail(f"translation coverage {cov.get('blocks')} blocks / {cov.get('instructions')} instructions "
             f"differs from the tested {EXPECT_BLOCKS} / {EXPECT_INSTRUCTIONS}")

    _STEP[0] = "compiling (cmake/ninja)"
    say("[3/5] Compiling (several minutes the first time)")
    build_dir = os.path.join(work, "build")
    run([cmake, "-S", src, "-B", build_dir, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
         "-DBUILD_UPSTREAM_AUDIO=ON", f"-DCMAKE_C_COMPILER={cc}", f"-DCMAKE_CXX_COMPILER={cxx}",
         f"-DCMAKE_MAKE_PROGRAM={ninja}"])
    cmd = [cmake, "--build", build_dir, "--target", "main_gx"]
    if args.jobs:
        cmd += ["--parallel", str(args.jobs)]
    run(cmd)

    _STEP[0] = "assembling the game folder"
    say("[4/5] Assembling the game folder")
    exe = "main_gx.exe" if windows else "main_gx"
    target = "DigimonBattleGC.exe" if windows else "DigimonBattleGC"
    os.makedirs(out, exist_ok=True)
    shutil.copy2(os.path.join(build_dir, exe), os.path.join(out, target))
    docs = os.path.join(src, "platform", "desktop", "upstream_gx", "PLAYING.md")
    shutil.copy2(docs, os.path.join(out, "PLAYING.md"))
    lic = os.path.join(out, "licenses")
    os.makedirs(lic, exist_ok=True)
    for name, path in (("gcn-recomp.txt", ("third_party", "gcn-recomp", "LICENSE")),
                       ("dear-imgui.txt", ("third_party", "imgui", "LICENSE.txt"))):
        shutil.copy2(os.path.join(src, *path), os.path.join(lic, name))
    if args.portable:
        open(os.path.join(out, "portable.txt"), "a").close()

    archive = None
    if not args.no_zip:
        _STEP[0] = "zipping the game folder"
        say("[5/5] Zipping the game folder (your personal backup)")
        system = {"nt": "windows"}.get(os.name, platform.system().lower())
        archive = os.path.join(os.path.dirname(out), f"DigimonBattleGC-{version}-{system}-{platform.machine().lower()}.zip")
        zip_folder(out, archive)
    else:
        say("[5/5] Zip skipped (--no-zip)")

    say(f"\nDone in {time.time() - t0:.0f} s: {os.path.join(out, target)}")
    if archive:
        say(f"Zip: {archive} ({os.path.getsize(archive) / 1e6:.1f} MB)")
    say("Start it, choose your disc image in the launcher (or drop it on the window), and press Play.")
    say("The program contains code translated from your disc: keep it for your own use, do not share it.")


def main():
    ap = argparse.ArgumentParser(description="Build Digimon Battle Chronicle from your own disc.")
    ap.add_argument("disc", nargs="?",
                    help="your Digimon Battle Chronicle (Japan) disc image: .iso/.gcm/.rvz/.gcz/.ciso "
                         "(omit to pick or find one)")
    ap.add_argument("--out", help="output folder (default: DigimonBattleGC next to build.py)")
    ap.add_argument("--work", help="build folder (default: work next to build.py)")
    ap.add_argument("--jobs", type=int, default=0, help="parallel compile jobs (default: all cores)")
    ap.add_argument("--portable", action="store_true",
                    help="keep settings, saves and logs in a 'user' folder beside the program")
    ap.add_argument("--no-zip", action="store_true", help="do not zip the finished game folder")
    args = ap.parse_args()
    if sys.version_info < (3, 8):
        print("ERROR: Python 3.8 or newer is required")
        return 1
    state = {"disc": args.disc}
    kit, src, version = os.path.dirname(os.path.abspath(__file__)), None, "dev"
    try:
        kit, src = find_source()
        version = kit_version(kit, src)
        say(f"Digimon Battle Chronicle self-build kit {version}")
        build(args, kit, src, version, state)
        return 0
    except BuildError as e:
        say("\nERROR: " + str(e))
        err = str(e)
    except KeyboardInterrupt:
        say("\nStopped.")
        return 130
    except Exception as e:  # an unexpected failure is still worth a report
        import traceback
        say("\nERROR (unexpected): " + repr(e))
        for line in traceback.format_exc().splitlines():
            _LOG.append(line)
        err = repr(e)
    report = write_report(kit, version, state.get("disc"), err)
    say(f"A build report was written to {report}")
    say("Attach it when you ask for help; it contains no game data.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
