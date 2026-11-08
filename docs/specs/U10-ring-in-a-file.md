# U10 — The ring in a file: Save Ring and Load Ring, in the firmware

**Status:** PROPOSED (2026-11-08).
**Repo:** TRX64-Ultimate (app: `software/io/c64/`, `software/filetypes/`).
**Number:** U10 (registry: the TODO table in `TODO.md`).
**Milestone:** the release U8/U9 are in (C64 Ultimate 0.2.x).
**Origin:** Alex, 2026-11-08, after U9 passed on the board:
> "ok, nächster schritt dann *save ring* / *load ring* ... auch genau so in die Firmware App rein.
> Nix app."

**Depends on:** [U8](U8-c64re-v2-state-and-medium.md) (the container),
[U9](U9-state-puts-the-medium-back.md) (the medium goes back in). Both built and on the board.

---

## §1 The problem

A `.c64re` carries one moment. The ring carries the seconds around it, and after a load you can
rewind through them again — which is the whole of Timewarp. There is no working way to put those
seconds in a file.

**Who:** Alex at the board, and after the release anyone who wants to keep a session rather than a
single frame: hand a bug to someone else with the minute that led to it, come back tomorrow and
scrub the same seconds.

**What happens today:**

- **`Save Timewarp Ring` is gone.** U8 removed it with Alex's agreement (*"Scheiss auf den Ring,
  erstmal nur dump/undump"*), because what it wrote was a full state per entry — 60 828 557 bytes
  for 21 entries, the same megabyte of cartridge 42 times, and the write that preceded
  `USB1 SanDisk 3.2Gen1 FAILED`.
- **`.trxring`, the ring's own format, has never produced a loadable file.** Its writer uses
  `%lu`/`%llu` seven times (`trx_trxring.cc:337` and on), which this firmware's printf copies out
  verbatim without taking the argument (U8 §3.2). So every `.trxring` ever written carries the
  literal text `%lu` as its `bitstreamId`, and its own reader compares that field at
  `trx_trxring.cc:599` and answers **"A .trxring of another bitstream"**. Every one of them. The
  two 0-byte files on Alex's stick are what its error path leaves behind.
- The REST and trxmon verbs `timewarp:ringdump` / `ringsave` still write format 1 and the broken
  `.trxring`; U8 §7 left them standing and called the question open.

**So there is no compatibility to preserve.** Nothing in the field loads. U10 is not a migration.

**What the ring is, measured 2026-11-08 on the board** (`GET /v1/timewarp:status` and
`trx_ring.cc:104`):

| | |
|---|---|
| Stream 0 (C64 RAM writes) | area 16 MiB, 4 bytes per record |
| Stream 1 (colour and cartridge RAM) | area 8 MiB, 8 bytes per record |
| Stream 2 (markers and chip blocks) | area 4 MiB, 8 bytes per record |
| Held right now | 2 661 frames, **53.09 s**, 1 572 writes in the last frame |

The window is limited by area 0: at about 1 500 writes a frame, 16 MiB of 4-byte records is
roughly a minute. A file of the whole window is therefore **about 18 MB** — 16 MiB of stream 0,
the little that stream 1 holds, around 745 KB of timeline (2 661 × `sizeof(TrxFrame)`), and the
1.18 MB of state and medium that U8/U9 already write. There is no compressor on this board.

**Parts (MECE):**

- **P1** The ring's own data — timeline and the undo records — is in no file.
- **P2** A ring file is tens of megabytes, and the last attempt at one cost a USB stick.
- **P3** The ring is the gateware's: a file of it is only meaningful on the bitstream that wrote it.
- **P4** Writing a ring must not reintroduce the defect U8 removed: the medium stored per entry.

## §2 Scenarios — what proves it is solved

```gherkin
Scenario: The seconds come back
  Given Wasteland has been running for a minute and Timewarp holds 50 seconds
  When  I choose Save Ring, switch the C64 off and on, and load that file
  Then  the machine stands where it stood when I saved
  And   Timewarp rewinds through the same seconds, to the same oldest frame
  And   the cartridge is in and the game runs on from any frame I pick

Scenario: The medium is in it once
  Given a ring file of 50 seconds of an EasyFlash game
  When  I look at the file
  Then  it holds exactly one copy of the cartridge, at the newest frame
  And   its size is the ring's records plus about the size of one .c64re

Scenario: Another bitstream
  Given a ring file written by a different bitstream
  When  I load it
  Then  the whole file is refused by name, with both bitstream ids
  And   nothing is touched: not the machine, not the cartridge, not the ring
  And   the state inside it is not loaded either, which the message says

Scenario: A write that cannot finish
  Given a stick that is pulled during a 18 MB write
  When  the write fails
  Then  no file is left behind
  And   the machine is untouched and still running
```

| # | Solves | Scenario | Test | Where |
|---|---|---|---|---|
| S1 | P1 | the seconds come back after a power cycle | `U10-RING` | board |
| S2 | P4 | one copy of the medium, size as predicted | `U10-SIZE` + `tools/c64re_read.py` | board + host |
| S3 | P3 | a ring from another bitstream is refused by name | `U10-BITS` | board |
| S4 | P2 | a failed write leaves nothing | `U10-FAIL` | board |
| S5 | P1 | the container round-trips with the ring sections | `tools/test_c64re_read.py` | host |
| S6 | — | **negative:** `Save State` / `Load State` are unchanged; U9-COLD stays green | `U9-COLD`, `run-tests -s timewarp-files` | board + CI |

## §3 Decision

**D1 — A ring file is a state file with three more sections.** Same container, same 41-byte head,
same digest, same header, format 2. New section ids:

| id | | |
|---|---|---|
| 8 | TIMELINE | `frames` × `sizeof(TrxFrame)`, oldest first, as they lie in memory |
| 9 | LOG0 | stream 0 records from the oldest entry's `s0` to the newest entry's `e0` |
| 10 | LOG1 | stream 1, the same |

Sections 1-7 are what they already are and describe **the newest frame**: chip block, RAM, colour
RAM, cartridge RAM, the medium, drive A, the EEPROM. So the medium is in the file **once**, which
is P4, and U9's whole load path — insert the cartridge, mount the disk, set the settings — applies
unchanged.

**One reader, one loader.** A file with sections 8-10 is a ring, one without is a state;
`stage_v2()` decides by what the table holds, never by the name. The name is for the user: a ring
is written as **`.c64rering`** (Q1), so an 18 MB file is visible as one in the list.

**D2 — The header says what the ring is.** Added beside `machineState`:

```json
"ring":{"frames":2661,"oldestSeq":54009,"headSeq":56669,"seconds_x100":5309,
        "frameBytes":280,"rec0":4,"rec1":8}
```

`frameBytes`, `rec0` and `rec1` are the sizes this build uses, so a reader refuses a file whose
records it would misread rather than loading nonsense — the same rule `blockWords` already follows.

**D3 — The counts are rebased on the way out**, so the newest entry's are 0 and the records land
where the engine's counters stand after the restart a load does. `.trxring` already did this
(`trx_trxring.cc`); it is the one piece of that file worth keeping.

**D4 — Stream 2 is not written.** It holds the markers and blocks the parse has already turned
into the timeline, and after a load the engine writes its own from count 0.

**D5 — Written and read by the firmware, in the menu.** `C64 Machine > Save Ring` beside
`Save State`; the browser offers `Load Ring` on a file that has the ring sections. No app, no REST
detour. Both get the progress box U9 built, counting the same `RAW_CHUNK` steps — at 18 MB the bar
is not a nicety.

**D6 — D0 still rules.** What a dump holds, an undump puts back, else there is no dump: the
cartridge gate from U9 D3 applies to `Save Ring` exactly as to `Save State`.

## §4 Out

- **A compressor.** It would take 18 MB to perhaps 6, and it is a body of work of its own (U8 §7).
- **`.trxring` and the format-1 ring readers.** They stay as they are, which is: not loadable. They
  are removed when nothing points at them any more, not in this spec.
- **The REST and trxmon ring verbs.** `timewarp:ringdump` / `ringsave` still write format 1; what
  happens to them is Q3.
- **Content-addressed de-duplication** (C64RE spec 714.4). Not needed once the medium is stored
  once; it is what a *multi-checkpoint* file would need, which this is not.

## §5 Decisions taken while building

**Q1 — answered 2026-11-08, Alex: (b).** A ring keeps **`.c64rering`**, so the file list says which
is which without opening anything and an 18 MB file is never loaded by accident. The extension is
already registered (`filetype_c64re.cc` takes `.c64re`, `.c64rering` and `.trxring` today), so this
is a change of what the name contains, not of what the browser knows.

The name is a label, not a guarantee: `stage_v2()` still decides what a file is from its section
table, and a `.c64rering` without sections 8-10 loads as the state it is. The extension picks the
menu entry; the table picks the behaviour.

**Q2 — answered 2026-11-08, Alex: (a), refuse the whole file.** A ring file whose `bitstreamId`
is not this bitstream's is refused by name with both ids, before anything is touched. The state in
it is not loaded either, although sections 1-7 would be valid on their own.

D0 is read at the level of the file, not the section: a `.c64rering` is a promise to put the ring
back, and a load that silently delivers less than the file promises is the shape U8 and U9 exist to
remove. The consequence is stated rather than hidden: the state inside a foreign ring file cannot
be had, and anyone who wants it writes a `.c64re` on the machine that can read the ring.

**Q3 — answered 2026-11-08, Alex: (c).** The two verbs become one, because under Q4 there is only
one kind of ring file.

- **`timewarp:ringdump` goes**, with its `seconds` and `stride`. They sample a window into several
  entries, which is the multi-entry shape U8 removed - one medium per entry - and Q4 leaves nothing
  to sample: a ring file is the whole window. It has no U10 equivalent to be ported to.
- **`timewarp:ringsave` keeps its name** and writes the U10 `.c64rering`, through the same code as
  `Save Ring`. Menu, REST and trxmon then produce the same file.
- The `.trxring` and format-1 **readers stay** until nothing points at them. Read yes, written no.
- `software/ultimate-app/tests/e2e/api/timewarp_file_test.py` is rewritten onto the one path. It is
  the only automated coverage ring files have, which is why deleting the verbs outright was not an
  option; after this it tests a format that can actually be loaded, instead of two of which one
  never could.

Board test `T66-MON` still describes `ringdump`/`ringsave` as they were and is corrected with the
build.

---

**All four questions are answered. The spec is ready to build.**

**Q4 — answered 2026-11-08, Alex: (a), the whole window.** *"das ist ok, das laden darf lange
dauern, das saven auch."* No bounded slice, no second menu entry: `Save Ring` writes everything the
hardware holds, about 18 MB and 53 s today, and both ways may take as long as they take. What that
buys is that a ring file is the ring, not a sample of it.

Two consequences to build for rather than hope about: the progress box is not optional at this size
(D5), and a write that fails must leave nothing behind (S4) - the last 18 MB-class write on this
board is what preceded `USB1 SanDisk 3.2Gen1 FAILED`.
