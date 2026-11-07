# gcn-recomp reuse

The CP FIFO adaptation is documented below. The `gx/` subtree contains the separately
adapted graphics backend; its platform adapter lives in
`port/platform/desktop/upstream_gx/`. All components use the same pinned upstream
revision and MIT license. Component-specific limitations remain explicit.

## CP FIFO subset

Upstream repository: https://github.com/ethannicholas/gcn-recomp

Pinned commit: `3fad2f2deea7e563755819cf6f44da8743c61c55`

Derivation: [`runtime/gx/fifo.cpp`, lines 446–575 and 663–721](https://github.com/ethannicholas/gcn-recomp/blob/3fad2f2deea7e563755819cf6f44da8743c61c55/runtime/gx/fifo.cpp#L446-L575),
with the CP control-bit constants from line 40. The register implementation is
at [lines 663–721](https://github.com/ethannicholas/gcn-recomp/blob/3fad2f2deea7e563755819cf6f44da8743c61c55/runtime/gx/fifo.cpp#L663-L721).
The adjacent upstream parser, renderer, threading, PI registers, RAM accessors,
write-gather implementation and diagnostic facilities are not included.

Upstream license: MIT. `LICENSE` is an exact copy of the pinned upstream file,
including `Copyright (c) 2026 Ethan Nicholas`.

## Independent functionality and API

`cp_fifo.c` and `cp_fifo.h` implement a portable C11 CP FIFO register/state
machine with no HAL, renderer, platform, memory-mapping or build-system
dependency. State is per `CpFifo`, whose public fields also permit snapshots
(by copying fields; callback pointers and `user` are caller-owned process state).
Use `cp_fifo_init`, `cp_fifo_read16`, `cp_fifo_write16` and
`cp_fifo_note_burst`. Register arguments are byte offsets decoded through
`off & 0xff`. Control-bit constants are exposed as `CP_FIFO_CR_*`.

The caller supplies the contiguous MEM1 size and owns the RAM, PI registers,
write-gather bytes and GPU command parser. After writing a 32-byte PI burst and
advancing PI's pointer, pass that pointer to `cp_fifo_note_burst`. This stores a
PI snapshot even when unlinked; only this hook follows PI into CP while LINK is
set. A control-register write never follows PI. CP drains its own write pointer
while READ is enabled, including after unlinking and when enabling READ after
queued writes. Breakpoint move/disable resumes queued data.

The synchronous `consume(start_phys, len, user)` callback must consume the
entire ordered RAM span and retain any partial GPU command itself. The
`irq(asserted, user)` callback receives the current CP interrupt level; repeated
reports of the same level are allowed. Both callbacks must not reenter or mutate
the context. A NULL IRQ callback is allowed; a NULL consume callback prevents
pointer advancement. Initialization reports IRQ deasserted.

## Adaptation changes

- Replaced upstream C++ globals/references with explicit `CpFifo` state and C11
  pointer arguments; removed logging, direct PI interrupt writes and RAM/parser
  calls in favor of callbacks. Preserved the upstream drain, breakpoint, idle-bit,
  half-register merge and settled fast-path control flow.
- Extracted only the CP-facing follow/drain/watermark portion of `gp_flush32`
  into the burst hook. The caller performs the actual RAM write and PI wrap.
- Watermark distance uses CP's own write pointer instead of reading live PI.
  This keeps a newly programmed GP FIFO independent of another CPU FIFO,
  including during the interval before software clears the old LINK bit.
- Added a whole-ring physical MEM1 guard before consumption. The exclusive end
  must fit both the supplied RAM extent and GameCube MEM1's 24 MiB limit;
  out-of-range physical addresses are never folded into RAM. As upstream, an
  out-of-ring read pointer normalizes to base, whereas an out-of-ring write
  pointer prevents consumption.
- Tightened the drain loop from four iterations to two: a tail and head span
  suffice to reach the write pointer or halt at the breakpoint. The end register
  still names the last 32-byte line. Equal masked pointers mean empty; this
  subset does not infer full-versus-empty from PI's wrap bit.
- Explicitly refresh IRQ after status writes and CLEAR, including while
  unlinked, so clearing a pending flag cannot leave the callback asserted.

Watermark status follows upstream's actual implementation: overflow/underflow
bits latch when distance is strictly above the high or below the low threshold.
CLEAR acknowledges bits 0/1, and a linked, still-active condition immediately
reasserts them. IRQ enables gate those flags independently. Watermarks are
checked only while LINK is set. Base/end writes apply the upstream low/high
masks; pointer registers preserve raw halves on readback, while effective FIFO
addresses mask to `0x03ffffe0` for alignment and physical-address arithmetic.

## Standalone verification

From the repository root:

```sh
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  port/third_party/gcn-recomp/cp_fifo.c port/tests/cp_fifo_selftest.c \
  -o /tmp/digimon-cp-fifo-selftest
/tmp/digimon-cp-fifo-selftest
```

The standalone test exercises independent CPU/GP destinations, the LINK-clear
regression, READ-disabled backlog, last-line wrap, all register half merges,
masked pointers, breakpoint IRQ/resume, watermark thresholds/acknowledgment,
independent contexts, MEM1 rejection and bounded empty-ring behavior. No game
assets or other upstream subsystems are required.
