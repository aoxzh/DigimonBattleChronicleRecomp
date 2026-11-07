#!/usr/bin/env python3
"""Build the C/C++ audio integration selftest on Windows or POSIX, without SDL."""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dol", nargs="?")
    parser.add_argument("--cc", default=os.environ.get("CC", "clang"))
    parser.add_argument("--cxx", default=os.environ.get("CXX", "clang++"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    dol = Path(args.dol).resolve() if args.dol else root / "extracted/main.dol"
    flags = ["-O1", "-g", "-Wall", "-Wextra", "-fno-omit-frame-pointer"]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all"]
    with tempfile.TemporaryDirectory(prefix="gc-upstream-audio-test-") as directory:
        work = Path(directory)
        objects = []
        for source in ("tests/upstream_audio_selftest.c", "tests/test_device_clock_stubs.c",
                       "hal/dsp.c", "hal/ai.c",
                       "platform/desktop/upstream_audio/adapter.cpp",
                       "third_party/gcn-recomp/audio/ax.cpp"):
            path = root / source
            cpp = path.suffix == ".cpp"
            compiler = shlex.split(args.cxx if cpp else args.cc)
            obj = work / (path.stem + ".o")
            subprocess.run(compiler + ["-std=c++17" if cpp else "-std=c11",
                           "-DGC_UPSTREAM_AUDIO"] + flags + ["-c", str(path), "-o", str(obj)], check=True)
            objects.append(str(obj))
        binary = work / ("selftest.exe" if os.name == "nt" else "selftest")
        subprocess.run(shlex.split(args.cxx) + flags + objects + ["-o", str(binary)], check=True)
        env = os.environ.copy()
        compiler_dir = str(Path(shlex.split(args.cxx)[0]).resolve().parent)
        env["PATH"] = compiler_dir + os.pathsep + env.get("PATH", "")
        subprocess.run([str(binary), str(dol)], env=env, check=True, timeout=30)
        for source in ("hal/dsp.c", "hal/ai.c"):
            path = root / source
            subprocess.run(shlex.split(args.cc) + ["-std=c11", "-Wall", "-Wextra", "-c",
                           str(path), "-o", str(work / ("default-" + path.stem + ".o"))], check=True)
    print("PASS upstream audio integration (" + ("ASan + UBSan" if args.sanitize else "C11/C++17") + ")")


if __name__ == "__main__":
    main()
