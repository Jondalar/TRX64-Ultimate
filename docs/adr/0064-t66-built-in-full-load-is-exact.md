# 64. T66 is built in full, and loading puts the machine back exactly

Date: 2026-11-05

Status: Accepted

Supersedes in part [0030](0030-timewarp-dump-and-trace-export-out-of-m1.md), which took all of this
out of M1, and sits beside [0061](0061-c64re-snapshot-app-back-in-the-release.md), which brought the
snapshot app back.

## Context

Alex asked how a `.c64re` is loaded on the board. The answer was: it is not, anywhere - A13
([ADR-0030](0030-timewarp-dump-and-trace-export-out-of-m1.md)) removed the dump and the load, and
[ADR-0061](0061-c64re-snapshot-app-back-in-the-release.md) brought back only the app, which writes.
What the app writes cannot resume either: it reads the machine over DMA, so the 6510's registers and
the internal counters of VIC, CIA and SID are missing - "a machine to look at, not one that carries
on running".

His answer named the reason it can work anyway (2026-11-05):

> "mit dem Ring klappt es ja prima, dass ich zurück gehen und dann per Enter ab da weiter laufen
> lassen ... dann muss das Speichern und laden ja kein thema sein"

That is right, and the code says why: every timeline entry carries the whole chip block
(`TrxFrame.w[58]`), so `trx_ring_restore()` can put the CPU and the CIA flip-flops back over the
state bus and let the machine run. A file that carries that block is worth as much as a ring entry.

Asked how far to go - the firmware's own `.c64re` with the chip block, that plus the ring save, or
everything T66 holds - Alex chose **all of T66**.

## Decision

T66 is built, in full, and the file is the exact state of the machine:

- The firmware writes a `.c64re` with the `trx64Hw` node (the 58 chip-block words, the snap position,
  the RAM under the I/O, the colour RAM), so **board -> file -> board is exact**: the machine runs on
  from the file as it runs on after a rewind.
- Loading is reachable where a person meets the file: **"Load Snapshot" in the file browser**, for
  `.c64re` and `.c64rering` alike. It is also a REST route and a monitor verb.
- A file from TRX64 (no `trx64Hw`, real deflate) loads as the coarse cut of §9.4 instead of being
  refused.
- The ring save (`.c64rering`), the native ring copy (`.trxring`) and the trace export
  (`.c64retrace`) are built with it, as are the drive-A state by way B and the fuller node set of
  §9.3.

Nothing is "spiked": a part that turns out not to be worth building is a decision with Alex, before
it is dropped, and it goes into the spec's [T66](../specs/T20-delta-ring.md#t66) section.

## Consequences

- `trx_c64re.cc/.h` and the ring's `materialize` / `load_state` / `set_turbo` paths come back from
  the history of A13; the three ABI file slots (`reserved_files[3]` since `a93920be`) and
  `trace_writes` (`reserved_trace_writes`) become functions again at the same offsets, so no app
  built against the table moves.
- The load is in two halves: the file is read and checked when it is chosen (so a wrong file is
  answered in front of the user) and goes into the machine when the menu closes, because the menu
  holds the machine and would otherwise put its own backup back over the loaded state. That is the
  path a Timewarp rewind already takes (`C64::unfreeze()`).
- What a file cannot bring back stays as decided: the SID's analogue state, the VIC pixel pipeline
  (2026-10-04), and drive A until way B is built (2026-10-19).
- Board tests: a file written on the board, loaded again, must run on; a TRX64 file must load coarse;
  both go on the pending list.

**As built, 2026-11-05** (every part, app branch `t66-app`, CI 244; the detail is in
[T66](../specs/T20-delta-ring.md#t66)). Three things the decision could not foresee, each settled by
what the hardware can do rather than by preference:

- A file from TRX64 cannot bring the CPU's instruction sequencer, so the chip block is built on top
  of a **base block from this machine's newest ring frame**. Every frame snap sits in the same place
  (the opcode fetch cycle), so that state belongs to the machine, not to the file - but a coarse load
  therefore needs a running ring, and says so when there is none.
- `keyboard` / `joystick` / `paddles` are not written at all: the input sampler is not in the built
  capture table, and the physical matrix says what is held *now*, which is not what was held at a
  frame three seconds back. They are a gateware question, not a firmware one.
- Drive A's node is **`trx64Drive`**, not TRX64's `drive1541`: that one is a set of VICE modules whose
  layout cannot be checked against anything on this machine. The same holds for the inner field names
  of `iec`. Both wait for the cross-check with the TRX64 session (T20 §13.3).

## Links

- [ADR-0030](0030-timewarp-dump-and-trace-export-out-of-m1.md)
- [ADR-0061](0061-c64re-snapshot-app-back-in-the-release.md)
- [T66 in docs/specs/T20-delta-ring.md](../specs/T20-delta-ring.md#t66)
