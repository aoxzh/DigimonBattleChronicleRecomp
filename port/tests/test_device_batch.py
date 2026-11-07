#!/usr/bin/env python3
"""Exercise real device clocks with eager/batched advancement on Windows/POSIX."""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default=os.environ.get('CC', 'clang'))
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    port = Path(__file__).resolve().parents[1]
    compiler = shlex.split(args.cc)
    flags = ['-std=c11', '-O2', '-g', '-Wall', '-Wextra', '-fno-omit-frame-pointer',
             '-DGC_UPSTREAM_AUDIO', '-I' + str(port / 'hal')]
    if args.sanitize:
        flags += ['-fsanitize=address,undefined', '-fno-sanitize-recover=all']
    env = os.environ.copy()
    for name in ('GC_AUDIO_MODE', 'GC_IRQ_DIAG', 'GC_FP_TRACE', 'GC_AX_DEPOP_TRACE',
                 'GC_RAM_WATCH', 'GC_VI_TRACE', 'GC_RUNTIME_MANIFEST'):
        env.pop(name, None)
    env['PATH'] = str(Path(compiler[0]).resolve().parent) + os.pathsep + env.get('PATH', '')
    with tempfile.TemporaryDirectory(prefix='gc-device-batch-') as directory:
        work = Path(directory)
        exe = work / ('test.exe' if os.name == 'nt' else 'test')
        sources = ['tests/test_device_batch.c', 'hal/irq.c', 'hal/ai.c', 'hal/dsp.c',
                   'hal/vi.c', 'hal/exi.c', 'hal/mmio.c']
        subprocess.run(compiler + flags + [str(port / p) for p in sources] +
                       ['-o', str(exe)], check=True)
        subprocess.run([str(exe), str(work / 'card.raw')], env=env, check=True, timeout=90)
    print('PASS real-HAL device batch ' + ('ASan/UBSan' if args.sanitize else 'O2'))


if __name__ == '__main__':
    main()
