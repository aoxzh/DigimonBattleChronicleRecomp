#!/usr/bin/env python3
"""Compile the real runtime dispatch boundary; optionally compare lookup caches.

The regular tests need no generated code or game image. --benchmark reads only
block addresses from recomp/blocks.c, using no-op callees and the same binary
search. Timings describe this synthetic dispatch workload, not game FPS. Stateful
malloc/rfi hook addresses stay in the table but are excluded from the trace.
Temporary runtime variants change only the cache declaration/helper/call.
"""
import argparse
import os
from pathlib import Path
import re
import shlex
import signal
import statistics
import subprocess
import tempfile

PORT = Path(__file__).resolve().parents[1]
RUNTIME = PORT / "recomp/runtime_glue.c"
TEST = PORT / "tests/test_runtime_dispatch.c"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default=os.environ.get("CC", "clang"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--benchmark", action="store_true")
    parser.add_argument("--iterations", type=int, default=2000000)
    args = parser.parse_args()
    compiler = shlex.split(args.cc)
    env = os.environ.copy()
    env["PATH"] = str(Path(compiler[0]).resolve().parent) + os.pathsep + env.get("PATH", "")
    for name in ("GC_MALLOC_TRACE", "GC_FP_TRACE", "GC_RAM_DUMP", "GC_PROBE", "GC_RUNTIME_MANIFEST"):
        env.pop(name, None)
    flags = ["-std=c11", "-D_POSIX_C_SOURCE=200809L", "-g", "-Wall", "-Wextra", "-Werror",
             "-fno-omit-frame-pointer", "-I", str(PORT / "recomp")]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all"]
    with tempfile.TemporaryDirectory(prefix="gc-runtime-dispatch-") as directory:
        work = Path(directory)

        def build(name, test, runtime, optimize="-O2"):
            output = work / (name + (".exe" if os.name == "nt" else ""))
            if test == TEST:
                # Expose the private helper only in this test translation unit,
                # so a repeated null result can be checked without recovering abort().
                lookup = "gc_dispatch_lookup" if "static GcBlockFn gc_dispatch_lookup" in runtime.read_text(encoding="utf-8") else "gc_block_lookup"
                wrapper = work / (name + "-wrapper.c")
                wrapper.write_text(f'#include "{runtime.as_posix()}"\n'
                                   f'GcBlockFn test_dispatch_lookup(uint32_t ea) {{ return {lookup}(ea); }}\n',
                                   encoding="utf-8")
                runtime = wrapper
            subprocess.run(compiler + flags + [optimize, str(test), str(runtime), "-o", str(output)],
                           check=True, env=env)
            return output

        def run(binary, *arguments, run_env=None):
            return subprocess.run([str(binary), *map(str, arguments)], cwd=work,
                                  env=run_env or env, capture_output=True, text=True, timeout=60)

        for opt in (["-O1"] if args.sanitize else ["-O0", "-O2"]):
            binary = build("dispatch" + opt, TEST, RUNTIME, opt)
            hook_env = dict(env, GC_MALLOC_TRACE="1")
            result = run(binary, run_env=hook_env)
            assert result.returncode == 0, result.stdout + result.stderr
            assert result.stderr.count("[malloc] enter") == 4, result.stderr
            assert result.stderr.count("[malloc] exit") == 4, result.stderr
            assert "REENTRANT" not in result.stderr, result.stderr
            print(result.stdout.strip())
            for address in ("80100101", "deadbeef"):
                result = run(binary, "miss", address)
                assert result.returncode in (-signal.SIGABRT, 3, 0xC0000409), result.stderr
                assert f"MISS pc={address}" in result.stderr, result.stderr
                assert "MISSROOTS end (0)" in result.stderr, result.stderr
                assert "trace_last.txt written (2 entries)" in result.stderr, result.stderr
            print(f"PASS {opt} missing/unaligned full-key targets retain fatal diagnostics")

        fp = build("fp-context", PORT / "tests/test_runtime_fp_context.c", RUNTIME,
                   "-O1" if args.sanitize else "-O2")
        result = run(fp)
        assert result.returncode == 0, result.stdout + result.stderr
        assert result.stdout.count("PASS ") == 6, result.stdout
        print(result.stdout.strip())
        print("PASS runtime dispatch + FP context" + (" (ASan/UBSan)" if args.sanitize else ""))

        if not args.benchmark:
            return
        if args.sanitize:
            parser.error("benchmark separately without sanitizer overhead")
        text = (PORT / "recomp/blocks.c").read_text(encoding="utf-8")
        addresses = [int(a, 16) for a in re.findall(r"\{ (0x[0-9a-f]+)u, b_[0-9a-f]+ \}", text)]
        assert addresses and addresses == sorted(set(addresses)), "expected a sorted generated block table"
        table = work / "addresses.txt"
        table.write_text(str(len(addresses)) + "\n" + "\n".join(f"{a:08x}" for a in addresses), encoding="ascii")
        runtime = RUNTIME.read_text(encoding="utf-8")
        begin = runtime.index("/* ---- bounded static-block dispatch cache ----")
        end = runtime.index("void gc_dispatch(uint32_t target_ea)", begin)
        baseline = runtime[:begin] + runtime[end:]
        old_call = "GcBlockFn fn = gc_dispatch_lookup(target_ea);"
        assert baseline.count(old_call) == 1
        baseline = baseline.replace(old_call, "GcBlockFn fn = gc_block_lookup(target_ea);")
        variants = {"binary-search": baseline, "folded-4096": runtime,
                    "folded-1024": runtime.replace("#define GC_DISPATCH_CACHE_CAP 4096u", "#define GC_DISPATCH_CACHE_CAP 1024u")}
        old_index = "((ea >> 2) ^ (ea >> 14)) & (GC_DISPATCH_CACHE_CAP - 1u)"
        assert runtime.count(old_index) == 1
        variants["multiply-4096"] = runtime.replace(old_index, "((ea >> 2) * 2654435761u) >> 20")
        print(f"BENCH table={len(addresses)} real block addresses; uniformly selected synthetic hot sets; median of 3")
        for name, source in variants.items():
            variant = work / (name + ".c")
            variant.write_text(source, encoding="utf-8")
            binary = build(name, TEST, variant)
            for hot in (1, 64, 256, 1024):
                elapsed, hits, lookups = [], [], []
                for _ in range(3):
                    result = run(binary, "bench", table, hot, args.iterations)
                    assert result.returncode == 0, result.stdout + result.stderr
                    assert not result.stderr, "benchmark must not time diagnostic output: " + result.stderr[:512]
                    match = re.search(r"ns_per_dispatch=([\d.]+) lookups=(\d+) hit_pct=([\d.]+)", result.stdout)
                    assert match, result.stdout
                    elapsed.append(float(match[1])); lookups.append(int(match[2])); hits.append(float(match[3]))
                print(f"BENCH {name:15} hot={hot:4} ns={statistics.median(elapsed):7.3f} "
                      f"hit={statistics.median(hits):6.3f}% lookups={int(statistics.median(lookups))}")


if __name__ == "__main__":
    main()
