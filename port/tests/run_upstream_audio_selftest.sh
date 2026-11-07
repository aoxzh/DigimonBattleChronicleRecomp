#!/bin/sh
# Standalone, leaves repository build files untouched. Pass DOL then optional RAM.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/digimon-audio-test.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
flags="-O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=all"
for file in tests/upstream_audio_selftest.c hal/dsp.c hal/ai.c; do
    name=$(basename "$file" .c)
    "${CC:-clang}" -std=c11 -DGC_UPSTREAM_AUDIO $flags -c "$root/$file" -o "$work/$name.o"
done
for file in platform/desktop/upstream_audio/adapter.cpp third_party/gcn-recomp/audio/ax.cpp; do
    name=$(basename "$file" .cpp)
    "${CXX:-clang++}" -std=c++17 $flags -c "$root/$file" -o "$work/$name.o"
done
"${CXX:-clang++}" $flags "$work"/*.o -o "$work/selftest"
if [ "$#" -ge 2 ]; then
    "$work/selftest" "$1" "$2"
else
    "$work/selftest" "${1:-$root/extracted/main.dol}"
fi
# Default HAL must still compile without the C++ adapter.
"${CC:-clang}" -std=c11 -Wall -Wextra -c "$root/hal/dsp.c" -o "$work/default-dsp.o"
"${CC:-clang}" -std=c11 -Wall -Wextra -c "$root/hal/ai.c" -o "$work/default-ai.o"
