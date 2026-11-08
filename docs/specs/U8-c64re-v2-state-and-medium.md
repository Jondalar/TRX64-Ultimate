# U8 — `.c64re` format 2: state and medium, binary and streamed

Status: **in progress** (2026-11-07)

Task: U8 · Spec owner: maintainer session · Code: `software/ultimate-app` (submodule), `docs/` here

## 1 Why

T66 built the file side of Timewarp and it was tried on the board on 2026-11-07 for the first
time. It does not work, and the reason is one thing seen from three sides.

Alex froze Wasteland (`/SD/C64/wasteland_ef_v1.0.1.crt`, EasyFlash, cartridge type 17) and saved.
Measured on the files that came out:

| | |
|---|---|
| `ring-2540-t34.c64re` | **2 987 081 bytes**, of which **93.7 % cartridge** |
| `ring-2540-t44.c64rering` | **60 828 557 bytes**, 21 entries, **96.7 % cartridge per entry** |
| `ring-2105-t2.trxring`, `ring-2540-t11.trxring` | **0 bytes** each, left behind by a failed write |

**a) The cartridge is in the file more than once.** `checkpoint.cartBytes` is the CRT file as it
is on disk (1 050 688 bytes, 128 CHIP packets); `checkpoint.cartFlash` is the live flash
(1 048 576 bytes). Concatenating the CHIP payloads and hashing gives `e9c3af1ca9e71c01c015e768…`
— **byte for byte the flash**. In the `.c64rering` the same MiB is in all 21 entries: one distinct
`cartBytes` and one distinct `cartFlash` over the whole file, so the same megabyte is stored **42
times**, plus the copy `wasteland_ef_v1.0.1.crt` that the native save put beside it (43).

**b) Nothing is compressed.** `manifest.checkpoint.encoding` says `ta-json-gz/1` and the body is a
valid gzip member — made only of **stored** deflate blocks (`00 | LEN=0x4000 | NLEN=0xBFFF`, 182 of
them). Ratio **1.000**. This is by design and the design is written down: `trx_inflate.h` says the
board has no compressor, only `trx_inflate`/`trx_gunzip` for reading what trx64 writes. The same
file written by trx64 (`dumps/dump-1788688201142.c64re`, same game) has ratio **0.318** — and gets
away with **three** cartridge copies in a smaller file than ours with two.

**c) The ring's accounting does not see any of it.**

```
byte_size per entry:      96 000      really 2 895 682      factor 30
budget_bytes:          5 505 024      file  60 809 359      factor 11
```

The ring counts the non-cartridge part only, believes 21 entries cost 2 MB, and keeps 21.

**What this costs on the board.** `trx_c64re_stage_entry()` mallocs the whole file and then the
whole gunzip output, and `free(raw)` comes only after `gunzip()` returns, so both live at once:

```
file             2 987 081 B
gunzip output    2 986 107 B
peak             5 973 188 B      = 75 % of the FreeRTOS heap (0x7A0000 = 7 995 392 B, heap_4)
```

Alex's board answered `This file cannot be loaded: Out of memory`. **A file the board writes, the
board cannot read back.** The 58 MB one never can.

And the 59 MB write to the USB stick is what preceded `USB1  SanDisk 3.2Gen1  FAILED` and two
0-byte `.trxring` corpses — the failure path closes the file and leaves it (`trx_trxring.cc:467`).

## 2 What Alex decided (2026-11-07)

> "Ich will: *state + medium* speichern, *state + medium* laden. Erstmal muss das für das C64U
> klappen, möglichst flott. Das trx64 passen wir dann einfach an."

> "Also Cartridge muss in die Datei, den dump, und zwar 1 mal, zum Zeitpunkt des Freezes. Genau
> die Fassung."

> "ersetzen. Scheiss auf den Ring, erstmal nur dump/undump bitte."

> "das Format heisst NICHT trxsnap1 … sondern `.c64re` … wir passen dann die trx64 Seite an"

Naming fixed in the same breath: **trx64** = the Rust emulator, **trx64u** = this board firmware.

So: the name `.c64re` stays, the container stays, **format version 2** is binary and streamed, and
trx64 follows. Version 1 keeps being *read* — the files Alex already has, and every file trx64
writes until it is changed, must still load.

## 3 The format

Little-endian throughout (RISC-V and the Rust side are both LE).

```
0    "C64RESNP"                       8 B    unchanged
8    u8  version                      1 B    1 = the JSON of §1, 2 = this
9    sha256 over everything from 41  32 B    unchanged in meaning
41   body:
       u32 header_len
       header_len B      header, JSON, ASCII, no payload in it
       u16 section_count
       u16 0                                 reserved
       section_count x { u8 id, u8 flags, u16 0, u32 len }      8 B each
       the payloads in table order, each padded to a multiple of 4
```

The 41-byte container head is byte-identical in both versions, so one reader tells them apart at
`raw[8]` — the check `trx_c64re.cc:1225` already does exactly that.

### 3.1 Sections

| id | name | length | what |
|---|---|---|---|
| 1 | `BLOCK` | `4 * TRX_BLOCK_WORDS + 8` | `u32 w[TRX_BLOCK_WORDS]`, `u16 x`, `u16 flags` (bit 0: the turbo word is valid), `u32 turbo_pack` |
| 2 | `RAM` | 65536 | C64 RAM, `$D000-$DFFF` already the RAM under the I/O |
| 3 | `COLOR` | 1024 | colour RAM, low nibbles |
| 4 | `CARTRAM` | `TRX_CART_RAM_BYTES` (65536) | the cartridge RAM window, only with a cartridge on |
| 5 | `CARTROM` | `C64_CRT::get_image_bytes()` | **the medium**: the cartridge contents at the freeze |
| 6 | `DRIVE` | `sizeof(TrxDriveState)` | drive A, read by the stub on its own CPU |

A section that does not apply is simply not in the table. Unknown ids are skipped by their length,
which is what makes a later section additive.

**Section 5 is the whole of Alex's decision.** It is read from `C64::get_cartridge_rom_addr()`
for `C64_CRT::get_image_bytes()` bytes — the image as it stands at the freeze, with everything the
game wrote into the flash. `cartBytes` is gone: the CRT file on disk is not the state, it is where
the state came from, and that belongs in the header. For a cartridge whose flash the C64 cannot
write, the same read gives the unchanged ROM, so one rule covers both and `trx_cart_writable()`
stops being a case.

### 3.2 The header

```json
{"kind":"c64re","version":2,"writer":"trx64u","appVersion":"0.2.0","machine":"c64-pal",
 "bitstreamId":2166136261,"caps":7,"caps2":3,"blockWords":58,
 "createdAt":"2026-11-07T15:11:34Z","frame":2540,"cycles":49944903,"pc":2300,
 "cartridgeType":17,"source":"/SD/C64/wasteland_ef_v1.0.1.crt","mediumBytes":1048576}
```

It carries provenance, not state: enough to say what wrote the file, for which machine, from which
image — and `blockWords` so a reader can refuse a block it cannot lay out. `mediumBytes` is the
length of section 5, not the size of the file on disk — a `.crt` is 2 112 bytes longer than the
flash it holds. `source` is **not** hashed: hashing it means reading a megabyte off the medium for
nothing, and the body's own sha256 already covers every byte that is restored.

**No `%l` in the format string.** The app's printf (`software/system/small_printf.cc:126`) knows
`d u i f s c p x X b B` and no length modifier: a `%lu` or `%llu` is copied out verbatim *and its
argument is never taken off the stack*, so every conversion behind it reads the one before. A
`uint32_t` is `unsigned int` on this target and goes out as `%u`; `cycles` is 64 bit and is laid
out by hand (`u64dec()`). §9 has what this cost.

### 3.3 Sizes

For Alex's Wasteland freeze, counted from the sections:

```
BLOCK        240 B
RAM       65 536 B
COLOR      1 024 B
CARTRAM   65 536 B
DRIVE      2 100 B           (when drive A is on)
CARTROM  1 048 576 B
header + table ~ 600 B
                           = 1 183 612 B   against 2 987 081 B today
```

**2.5x smaller, and it no longer matters whether the game has written to the flash** — the same
MiB is there either way, once.

## 4 Memory, which is the point

| | today | version 2 |
|---|---|---|
| save, peak heap | ram + col + cart + the whole document buffered through `Gz` | `ram` 64 K + `col` 1 K + `cartRam` 64 K + frame + drive ≈ **135 KB** |
| load, peak heap | **5 973 188 B** (file + gunzip output together) | `ram` 64 K + `col` 1 K + `cartRam` 64 K + `rom` 1 MiB ≈ **1 183 KB** |

Writing streams: each section goes to the file in 16 KiB chunks as it is produced, and `CARTROM`
is written straight from its DDR2 pointer without ever being buffered. Reading streams the same
way: the section table says where every payload is and how long it is, so each one is read once,
directly into the buffer that `TrxLoadMem` hands the hardware. Nothing is parsed into a second
representation, which is what JSON forced.

The 1 MiB on the load side is `TrxLoadMem::rom`, which `trx_ring_load_state()` wants as one block;
that is the hardware path, not the format, and it is out of this task's scope.

## 5 What changes for the user

- **`Save State`** — one entry in `C64 Machine`, replacing `Save Timewarp Frame`,
  `Save Timewarp Ring` and `Save Timewarp Ring (native)`.
- **`Load State`** — the file browser's entry on a `.c64re`, replacing `Load Snapshot`.
- **The file lands beside the running image**, Alex's decision of 2026-11-07: the folder of
  `C64_CRT::get_source()`, else the folder of the image mounted on drive A, else the first valid of
  `/Usb0`, `/Usb1`, `/SD` in a `timewarp/` folder as before. Wasteland's state goes to `/SD/C64/`,
  not onto the USB stick that the old fixed order picked.
- **Name:** `<image>-<YYYYMMDD-HHMMSS>.c64re`, with the real clock. The `y < 2024` branch of
  `save_path()` that produced `ring-2540-t34` is fixed in the same change: the file system gets the
  right FAT stamp while `rtc.get_time()` returned an unset year, so the name took the fallback.
- **A failed write deletes its file.** The two 0-byte `.trxring` corpses on the stick are what the
  current error path leaves behind; no path in this task leaves a partial file.

## 6 What stays

- **Timewarp itself** — the ring in the hardware, scrubbing, rewind, the monitor's verbs. Untouched.
- **Reading version 1** — `.c64re`, `.c64rering` and `.trxring` load exactly as they do today,
  including the coarse load of a trx64-written file (T66-TRX64). Alex's existing files keep working.
- `trx_c64re_stage` / `_apply` / `_set_pending` / `_unfreeze` keep their shape: the browser's
  deferred load is unchanged, only the reader behind it gains a version-2 branch.

## 7 Deliberately not built

Decided with Alex before the work, 2026-11-07:

- **Writing the ring to a file.** `Save Timewarp Ring` and `Save Timewarp Ring (native)` go away;
  `.c64rering` and `.trxring` stay readable. *"Scheiss auf den Ring, erstmal nur dump/undump."*
  The content-addressed de-duplication that trx64 has (C64RE spec 714.4: a refcounted,
  sha256-addressed pool, "an unchanged disk costs one stored copy across many checkpoints") is what
  a future ring file needs; it is not in this task.
- **A deflate compressor in the firmware.** It would take 1.18 MB to about 500 KB. Nothing here
  depends on it and it is a body of work of its own.
- **The trx64 side.** It follows afterwards, Alex: *"wir passen dann die trx64 Seite an."* Until it
  does, trx64 writes version 1 and this firmware reads it.
- **The REST and trxmon verbs that write a ring file stay.** `timewarp:ringdump` / `ringsave`
  (`route_timewarp.cc:406`, `:440`) and trxmon's `ringdump` / `ringsave` still write format 1
  `.c64rering` and `.trxring`, so the 59 MB file can still be made from there — just not from the
  menu. `timewarp:dump` and trxmon's `dump` go through `trx_c64re_dump()` and therefore write
  format 2 from now on, with no change of their own. Whether the ring verbs should go too is open.
- **A C host bank for the reader.** `stage_v2()` sits in `trx_c64re.cc` behind `trx_ring.h`,
  `trx_drive.h` and the state bus, so testing it on the host needs a shim like
  `software/io/c64/tests/c64_crt_host.h`. What is built instead is `tools/test_c64re_read.py`
  (§8 A8): the container laid out from §3 of this spec and read back by an implementation that
  shares no code with the firmware. It pins the format; it does not execute the firmware's reader.

## 8 Acceptance

| | how |
|---|---|
| A1 | `Save State` on the Wasteland freeze writes to `/SD/C64/`, name with date and time, about 1.18 MB |
| A2 | `Load State` on that file puts the machine back and it runs on — typing works, not just the picture |
| A3 | The flash comes back as it was at the freeze: save in the game, `Save State`, play on, load — the saved game is there |
| A4 | The same file read twice gives the same machine (sha256 of the body checked on every load) |
| A5 | A version-1 `.c64re` still loads, exact when it has `trx64Hw`, coarse when it comes from trx64 |
| A6 | A write that fails leaves no file |
| A7 | Peak heap on load stays under 1.3 MB — measured against `System Info`'s heap figure before and after |
| A8 | `python3 tools/test_c64re_read.py`: the container, the section table, padding, a truncated file, a broken digest, a changed byte, an unknown section id, a wrong section length, trailing bytes, a file with no medium, a header
       whose conversions were never expanded |

A1-A7 are board tests and are in `docs/board-tests-pending.md` as `U8-SAVE`, `U8-LOAD`, `U8-FLASH`,
`U8-CHECK`, `U8-HEAP`, `U8-OLD`, `U8-NOCART`, `U8-FAIL`.

`tools/c64re_read.py` is the second implementation A4 rests on: it reads a file off the board, checks
the body's SHA-256 and every section length, and shares no line of code with the writer. The e2e
suite `timewarp-files` needs no change — it asserts behaviour (sizes above a floor, the ring's state
after a load), not the bytes of the format.

## 9 Status and history

- **2026-11-07** — board session on `t66` (package `t66-12e293fb`, app `0c0f5fc8`): T66-FILES ran,
  the three files were written, `Load Snapshot` answered `Out of memory`, and the USB stick dropped
  off the bus during the 59 MB write (`USB1 … FAILED`). Files pulled off the stick over FTP and
  measured; the numbers in §1 are from those files. Alex decided §2.
- **2026-11-07, built.** Format 2 writer (`trx_c64re_dump_ex()`), reader (`stage_v2()`), the
  placement rule (`trx_c64re_place()`), `Save State` in place of the three old entries, `Load State`
  in the browser, and the failed write that deletes its file. `make -C target/u64ii/riscv/ultimate`
  in `u64-build-env`: **green, no new warnings** (the two that remain are the known
  `c64.cc:1341 wait_10us(400)` and trxmon's `yn`). `python3 tools/test_c64re_read.py`: **12 of 12**, 13 after the
  header test below.
  Not yet on the board — §8 A1-A7 are open and are what the next session answers.
- **2026-11-07, on the board and broken (U8-SAVE passed, U8-LOAD failed).** Alex flashed the
  format 2 firmware and froze Wasteland. The write was right in every way the §1 defect was wrong:
  `/SD/C64/wasteland_ef_v1.0.1-20261107-200156.c64re`, **1 183 369 bytes**, beside the image, name
  with date and time, body digest good, six sections, the 1 MiB cartridge **once**, no trailing
  bytes. `Load State` refused it: `The file's chip block is not this machine's`.
  `tools/c64re_read.py` named it on the first run — `!! the header is not JSON`:

  ```
  {"kind":"c64re",...,"bitstreamId":%lu,"caps":844649044,"caps2":255,"blockWords":247,
   "frame":%lu,"cycles":%llu,"pc":1921,...,"mediumBytes":%lu}
  ```

  `header_json()` used `%lu`/`%llu`, which this printf copies out verbatim without taking the
  argument (see §3.2). Everything behind each one shifted by a field: `caps` ate the bitstream id,
  `caps2` ate `caps`, and **`blockWords` ate `caps2` — 247 against `TRX_BLOCK_WORDS` 58**, which is
  exactly what the loader refused. `pc` 1921 was the frame number. The payload was never touched,
  so the defect was one line of text in an otherwise correct file.

  Fixed: `%u` throughout, `u64dec()` for `cycles`, and the same at the four other `%lu` sites in
  `trx_c64re.cc` (two debug lines, the sizes in `[C64RE] .c64re format …`, and the user-visible
  `"The file unpacks to %u KB, which did not fit"`). Two more that came out of the same session:
  `header_json()` carried a **second** dead `y >= 2024` on the raw `Rtc::get_time()` year, so no
  file ever got a `createdAt`; and the progress box still said **"Saving the ring"** where one
  state is written. Rebuilt, packaged as `u8dev2`, on the board.
- **2026-11-07, fixed on the way:** `save_path()` tested `y >= 2024` on a year that
  `Rtc::get_time()` returns as **years since 1980** (`set_time_utc()` stores `tm_year - 80`,
  `get_date_string()` prints `1980 + y`). The date branch could never be taken, which is why every
  file was called `ring-2540-t34` while its FAT stamp was right.
