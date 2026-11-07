#!/usr/bin/env python3
"""Compile real IRQ code; compare guest results and diagnostic observation order.

Optional --baseline /path/to/prior/irq.c --benchmark measures both implementations
with the same 20M zero-cycle poll fixture. This is a host microbenchmark, not an
Android FPS prediction. No generated code or platform builds are involved.
"""
import argparse
import os
from pathlib import Path
import statistics
import subprocess
import tempfile

PORT = Path(__file__).resolve().parents[1]
FLAGS = ("GC_IRQ_DIAG", "GC_FP_TRACE", "GC_AX_DEPOP_TRACE", "GC_RAM_WATCH",
         "GC_VI_TRACE", "GC_RUNTIME_MANIFEST")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default=os.environ.get("CC", "clang"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--benchmark", action="store_true")
    parser.add_argument("--baseline", type=Path)
    args = parser.parse_args()
    base_env = os.environ.copy()
    for name in FLAGS:
        base_env.pop(name, None)

    with tempfile.TemporaryDirectory(prefix="irq-poll-") as directory:
        directory = Path(directory)

        def build(source, name):
            binary = directory / name
            command = [args.cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
                       "-O1" if args.sanitize else "-O2", "-I", str(PORT / "hal")]
            if args.sanitize:
                command += ["-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
            command += [str(PORT / "tests/test_irq_poll.c"), str(source), "-o", str(binary)]
            subprocess.run(command, check=True)
            return binary

        def run(binary, mode=None, env=None):
            process_env = base_env.copy()
            process_env.update(env or {})
            return subprocess.run([str(binary)] + ([mode] if mode else []),
                                  env=process_env, text=True, capture_output=True, check=True)

        current = build(PORT / "hal/irq.c", "current")
        configurations = [
            {}, {"GC_IRQ_DIAG": "1"}, {"GC_RUNTIME_MANIFEST": "1"},
            {"GC_IRQ_DIAG": "1", "GC_RUNTIME_MANIFEST": "1"},
            {"GC_IRQ_DIAG": "1", "GC_RUNTIME_MANIFEST": "1", "GC_FP_TRACE": "1",
             "GC_AX_DEPOP_TRACE": "1", "GC_VI_TRACE": "yes", "GC_RAM_WATCH": "1000:16"},
            {"GC_FP_TRACE": "2", "GC_AX_DEPOP_TRACE": "true", "GC_VI_TRACE": "0",
             "GC_IRQ_DIAG": "0"},
            {"GC_RAM_WATCH": "0"},
        ]
        outputs = [run(current, env=env).stdout for env in configurations]
        assert len(set(outputs)) == 1, outputs
        print(outputs[0].strip())

        trace = run(current, "trace", {"GC_FP_TRACE": "1", "GC_RAM_WATCH": "1000:16",
                                      "GC_VI_TRACE": "1", "GC_RUNTIME_MANIFEST": "1"}).stderr
        position = -1
        for marker in ("[fp-block]", "[tick-test]", "[ram-watch]", "[vi-trace]", "[manifest-test]"):
            position = trace.index(marker, position + 1)
        assert "[manifest-test] pc=802c32f4 cycles=12" in trace, trace
        ax = run(current, "ax", {"GC_AX_DEPOP_TRACE": "1"}).stderr
        assert "kind=collect" in ax and "result=ok" in ax and "mismatch" not in ax, ax
        print("IRQ poll tests passed: 7 diagnostic configurations, IRQ equivalence, FP/AX/RAM/VI ordering")

        if args.benchmark:
            if args.sanitize:
                raise SystemExit("Run the microbenchmark without --sanitize")
            previous = build(args.baseline.resolve(), "baseline") if args.baseline else None
            if previous:
                prior = run(previous, env={"GC_IRQ_DIAG": "1", "GC_RUNTIME_MANIFEST": "1"})
                assert prior.stdout == outputs[0], (prior.stdout, outputs[0])
                print("previous/current guest state digests match")
            samples = {"current": []}
            if previous:
                samples["baseline"] = []
            for repetition in range(5):
                candidates = [("current", current)]
                if previous:
                    candidates.append(("baseline", previous))
                if repetition & 1:
                    candidates.reverse()
                for name, binary in candidates:
                    samples[name].append(float(run(binary, "bench").stdout))
            for name, values in samples.items():
                print(f"{name}: median {statistics.median(values):.3f} ns/poll, samples={values}")
            if previous:
                speedup = statistics.median(samples["baseline"]) / statistics.median(samples["current"])
                print(f"isolated host zero-cycle poll speedup: {speedup:.3f}x")


if __name__ == "__main__":
    main()
