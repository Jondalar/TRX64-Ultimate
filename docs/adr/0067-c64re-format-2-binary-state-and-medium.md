# 0067. `.c64re` format 2: binary sections, the medium once

Date: 2026-11-07

Status: Accepted

Amends [0064](0064-t66-built-in-full-load-is-exact.md) (T66 built in full, the load is exact), which
stands for what it decided; this changes what the file looks like and which menu entries write one.

## Context

T66 reached the board on 2026-11-07 and the file side did not work. Alex froze Wasteland
(`/SD/C64/wasteland_ef_v1.0.1.crt`, EasyFlash) and saved; measured on the files that came off the
stick:

- `.c64re` **2 987 081 bytes**, of which **93.7 % is the cartridge** — `cartBytes` (the CRT file)
  and `cartFlash` (the live flash), and concatenating the 128 CHIP payloads gives the flash **byte
  for byte**.
- `.c64rering` **60 828 557 bytes**, 21 entries, **one** distinct `cartBytes` and **one** distinct
  `cartFlash` across all of them: the same megabyte **42 times**. Its own accounting says
  `byte_size` 96 000 per entry against 2 895 682 real, and `budget_bytes` 5 505 024 against
  60 809 359 written.
- Both are gzip members made only of **stored** blocks — ratio **1.000**. That is by design:
  `trx_inflate.h` says the board has no compressor and only reads what trx64 writes.
- Loading the 2.9 MB file answered **`Out of memory`**: `trx_c64re_stage_entry()` holds the file
  and its unpacked document at once, 5 973 188 B of a 7 995 392 B FreeRTOS heap.
- The 59 MB write is what preceded `USB1 SanDisk 3.2Gen1 FAILED` and two 0-byte `.trxring` files,
  which the error path leaves behind.

A file the board writes, the board cannot read back. Details and every number:
[U8](../specs/U8-c64re-v2-state-and-medium.md).

The format is not ours alone: `.c64re` is C64RE's (spec 707), JSON with `$ta`+base64 under gzip,
and trx64 writes it too. Two things make that affordable there and neither holds here — real
deflate (trx64's own file of the same game: ratio 0.318, three cartridge copies in a smaller file
than our two), and the content-addressed media pool of C64RE spec 714.4, where "an unchanged disk
costs one stored copy across many checkpoints".

## Decision

Alex, 2026-11-07, after the measurements:

> "Ich will *state + medium* speichern, *state + medium* laden. Erstmal muss das für das C64U
> klappen, möglichst flott. Das trx64 passen wir dann einfach an."
> — "Also Cartridge muss in die Datei, den dump, und zwar 1 mal, zum Zeitpunkt des Freezes. Genau
> die Fassung." — "ersetzen. Scheiss auf den Ring, erstmal nur dump/undump." — "das Format heisst
> NICHT trxsnap1 … sondern `.c64re`."

**The name and the container stay; format 2 is binary and streamed.** The 41-byte head is
unchanged — `C64RESNP`, the format byte, the SHA-256 of the body — so one reader tells the two
apart at byte 8. The body is a short JSON header carrying provenance only, a section table of
`{id, flags, len}`, and the payloads. No base64, no document in RAM.

**The cartridge goes in once, as it stands at the freeze.** Section 5 is read from
`C64::get_cartridge_rom_addr()`: the image with everything the game has written into the flash.
`cartBytes` is dropped — the CRT file on disk is not the state, it is where the state came from,
and that is a line in the header.

**This board is the authority now and trx64 follows.** Until it does, it writes format 1 and this
firmware reads it — unchanged, including the coarse load of a trx64 file.

**One entry each way.** `Save State` replaces `Save Timewarp Frame`, `Save Timewarp Ring` and
`Save Timewarp Ring (native)`; the browser's `Load Snapshot` becomes `Load State`. Writing a ring
to a file is gone. Reading `.c64rering` and `.trxring` stays.

**The file goes beside the image that is running** — the folder of the cartridge's own source,
else of the disk on drive A — not onto whichever volume comes first in a fixed list.

## Consequences

- One freeze of Wasteland: **1.18 MB instead of 2.99 MB**, and the same size whether or not the
  game has written to the flash. Load peak **1.18 MB instead of 5.97 MB** of a 7.8 MB heap.
- **A format 1 file this board wrote still loads** only on a machine whose heap can take twice its
  size. Alex's 2.9 MB file from 2026-11-07 does not, and this change does not make it. Its state is
  not lost in a way that matters: the machine it was taken from is the one in front of him.
- The ring cannot be put in a file any more. Rebuilding that needs 714.4's de-duplication first,
  not a bigger budget.
- Nothing in the hardware path changes: `TrxLoadMem`, `trx_ring_load_state()`, the chip block and
  `TRX_BLOCK_WORDS` are what they were, so the engine, Timewarp and the monitor are untouched.
- A compressor would take the file to about 500 KB. It is a task of its own and nothing waits on it.

## Links

- [U8](../specs/U8-c64re-v2-state-and-medium.md), [T20 §9, §10](../specs/T20-delta-ring.md),
  [T66 board tests](../board-tests-pending.md)
- [0064](0064-t66-built-in-full-load-is-exact.md) (amended)
- C64RE spec 707 (the format 1 container), spec 714.4 (the content-addressed media pool)
