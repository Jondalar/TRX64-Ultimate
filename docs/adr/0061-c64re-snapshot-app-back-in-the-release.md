# 61. The C64RE Snapshot app comes back into the release

Date: 2026-10-31

Status: Accepted

Supersedes in part [0030](0030-timewarp-dump-and-trace-export-out-of-m1.md).

## Context

[ADR-0030](0030-timewarp-dump-and-trace-export-out-of-m1.md) (A13) took the `.c64re` and `.c64rering`
dump and load and the trace export out of M1 and deferred all of it to T66. The app `apps/c64re` was
deleted with them, although it is the one piece that needs nothing from the firmware: it writes the
`C64RESNP` container itself (sha256 over gzip'd JSON with RAM, colour RAM, the CPU port, the VIC
registers and the media in the machine) and uses only the app ABI - `sys->log/delay_ms`,
`mem->malloc/free`, `ui->path_input/string_input/progress_*/popup`, `fs->*`, `media->source`,
`c64->dma_read/dma_read_frozen`. The firmware's `trx_c64re.cc/.h` and the three ring file slots in the
ABI (reserved NULL since `a93920be`) are a separate matter.

## Decision

Alex 2026-10-31: **"die app darf ins release, und muss es auch"**. `apps/c64re` is back on
`trx64/main` (merge `97bc7dfd`; the five files come byte for byte from `24572a66^`, the state after the
ABI split `f3ed17e4`) and ships as the fourth `.u2a`. Its menu entry is "C64 Machine > Save .C64RE
Dump".

What stays out, and stays T66: the ring save and load (`.c64rering`), the trace export
(`.c64retrace`), and the firmware's own `dump`/`undump`/`ringdump`/`ringload` verbs with the REST
routes `timewarp:dump/ringdump/load`.

## Consequences

- `ci/release.sh` builds every `apps/*/` with a Makefile except `EXCLUDE_APPS`, so the app is in the
  package and in the release without a change to the pipeline.
- Two points the removal had made moot are live again, in the application's `doc/TREX_TODO.md`: T6
  (`C64_CRT::get_source()` is never cleared, so the dump can name a cartridge that is not in the
  machine) and T7 (the dump of a physical cartridge). T5 belongs with them: `dma_read_frozen` hangs
  the firmware with a real cartridge in the port, and that is the path this app takes.
- The manual lists four apps ([docs/manual/08-apps.md](../manual/08-apps.md)).
- Not yet run on the board. The app was last built before the A13 removal (2026-10-06); the restored
  source is the post-`f3ed17e4` state and is built again from this merge on.

## Links

- [ADR-0030](0030-timewarp-dump-and-trace-export-out-of-m1.md)
- [T66](../specs/T20-delta-ring.md#t66)
- [docs/manual/08-apps.md](../manual/08-apps.md)
