# T95 — a new cartridge of the common kind is one JSON file: no code in the gateware, none in the tool, and one UI that both applications render

**Status:** PROPOSED (2026-11-06).
**Repo:** TRX64-Ultimate (tool `software/ultimate-app/apps/trex_crt`, descriptors `tools/cart_descriptors`, generator `tools/gen_cart_descriptors.py`, shared headers `software/io/c64`); the LVGL view itself is MultiCore's (T104.3).
**Number:** T95 (registry: `TODO.md`; 83-94 are MultiCore's, `TODO.md:123-124`).
**Milestone:** M2 (T57 §13: "TwoMegabyter dump/write — descriptor-based support is M2 with T33 data").
**Origin:** Alex, 2026-11-06, in his words: "Wir wollten das ja soweit abstrahieren dass neue cartridges (sofern sie nicht grundlegend andere Chips verwenden als das Markt übliche) per config gemanaged werden können und keine Code Anpassungen brauchen. Weder in der gateware noch in dem Tool." Then, on the probe: "Dann brauchen wir doch nur einen Probe Flow in einer toml und da steht drin in welcher Reihenfolge welcher descriptor rückwärts angewendet wird?" Then, on EasyFlash 3: "Ich bin mir gar nicht so sicher, ob nicht EasyFlash 3 im Grunde Deskriptor oder deklarativ eine Shell ist, wie ein Container. Und darin wiederum liegen 8 kleinere Shells. Und das meine ich mit nested. Ein ähnliches Phänomen werden wir bei Gmod 4 sehen." And on the UI: "Sehr wichtig ist mir auch, dass du beim UI darauf achtest, dass wir eine saubere Abstraktion haben zwischen dem Code in der App und zwischen der Darstellung im UI, weil wir wollen das ja noch mit dem Generic App UI von Multicore nochmal darstellen. Und ich hätte jetzt keine Lust, dass wir wegen der Benutzeroberfläche zwei Forks im Grunde von der gleichen Anwendung bauen."
**Depends on:** [PROP-002](PROP-002-cartridge-mapping-descriptor.md) Rev D (the descriptor), [PROP-001](PROP-001-cartridge-writeback-interface.md) (the flash chip as data), [T33](T33-generic-cart-decoder.md) (the gateware half, built 2026-10-03), [T57](T57-trex-crt-both-cores.md) (the tool as it is), MultiCore [T104.3](https://gitea.example/TREX/TRX64U-MultiCore) §"Applications" (the ABI 3 rebuild and the declarative app UI).

---

## §1 The problem

T33 made the **gateware** data: `cart_slot` and `c64_cartridge.v` carry no per-type code, every type is a
43-byte descriptor generated from one JSON file per type (`tools/cart_descriptors/*.json`). Adding
TwoMegabyter to the core was one JSON file.

The **tool** never got that. `apps/trex_crt` is a port of the firmware `CartWriter` from *before* T33 and
describes every cartridge a second time, in C. TwoMegabyter made it visible: the descriptor
`2f_twomegabyter.json` has been complete since 2026-10-03, including the flash profile, and the core
plays and programs the cartridge — but the dump/write tool cannot see it at all
(`apps/trex_crt/README.md:121` "TwoMegabyter is not supported", decided as Q6 in
[ADR-0006](../adr/0006-trex-crt-on-both-cores.md) because no hardware was here).

Parts (MECE):

- **P1 The flash profile is duplicated.** Six `static const flash_chip_info_t` in `cart_writer.c:16-60`
  repeat what the generator already emits as `trx_flash_part_t` in `software/io/c64/trx_cart_desc.h`
  (`c_flash_0f`, `_11`, `_12`, `_2f`, `_31`, `_48`, `_68`). The tool's copy carries no sector runs at all,
  so the erase scope is implicit in the code.
- **P2 There is no inverse of the window map.** `trx_flash_geom.h:trx_image_offset()` turns a chip address
  into an image offset. Nobody turns a chip address into *(bank, window, CPU address)*. That is why the
  structs hold `0x8555`, `0xEAAA`, `0x5555` instead of the data sheet's `0x555` / `0x2AA`, why GMod2 needs
  its own `gmod2_flash_cycle()`, and why every new type needs C.
- **P3 Detection is a hand-unrolled if/else chain.** `cart_dumper.c` STEP 1-6 plus the writer's
  `detect_chip()` (`cart_writer.c:300-360`). Every input it uses is already in the descriptor — reset lines
  from `table[]`, "write $DE00, does $8000 change" from `rules` + `bank` + `layout`, "$DE02 = $03 changes
  $E000" from a `table[]` row, the bank count from `bank.width` + `chip_size`, EasyFlash's $DF00 RAM from
  `io2.read = RAM` (T33 D6), EF3's slot read-back from `read_r2`. Only the ordering is not in the data,
  and today the ordering *is* the chain (`ef` → `megabyter` → `megacart` → `gmod2`, `bank_count > 256` →
  GMod3).
- **P4 Container types have no data model.** EF3's nesting is already declared — `12_easyflash3.json` has
  `bank: [{R2,3},{R0,6}]`, a 9-bit bank from two registers, and the generator handles `len(bank) == 2`
  (`gen_cart_descriptors.py:100-104`). What is *not* declared is that each slot is a self-contained
  cartridge image with its own CRT type, that there is a directory on the chip, that slot 0 is protected,
  and that the physical part (MX29LV640EBT) overrides the child's nominal part (AM29F040) for the EAPI
  driver. So all of that is C: slot dialog, partial erase, directory, EAPI choice.
- **P5 UI and logic are woven together.** Counted in the current tool: `cart_writer.c` touches the window
  211 times and `cart_screen_*` 33 times, `cart_dumper.c` 25 / 17, `cart_inspector.c` 24 / 0. The
  presentation is 40 x 22 character cells with fixed rows and columns (`cart_screen.h`), and the core
  blocks on `popup()` for an answer. MultiCore's generic application gives an app a container and expects
  it to bring its own **declarative LVGL** UI (T104.3 §"Applications": `trex_crt` = "own LVGL UI in the
  container: bank map, progress, results", built to the UX concept in `docs/lvgl_ui_project`), on **ABI 3**,
  while this repository's app is ABI 2. With the UI inside the logic, that is a second fork of the same
  application — which is exactly what Alex refuses.
- **P6 The platform is not abstracted either.** The core reaches the bus directly: `g_api->c64->`,
  `g_api->trx->regs[]` — 22 sites in `cart_base.c`, 22 in `cart_writer.c`, 4 in `cart_dumper.c`. ABI 3
  offers `machine` / `fs` / `job` instead. Without a port layer the same fork appears one level below the UI.
- **P7 TwoMegabyter is missing in the tool.** The concrete trigger: no probe, no dump type, no writer
  entry, no bank count (`cart_writer.c:2040-2041` has 86 and 19/88 only). A TMB cartridge is typed today as
  Ocean 16K (type 5), because it boots 16K (T57 §5, row "TwoMegabyter").

What it costs: every new cartridge of the common kind is C in two places instead of one JSON file, and the
ABI 3 move would double the tool.

## §2 Scenarios — what proves it is solved

```gherkin
Scenario: S1 nothing changes for the types that work today
  Given the five flat types the host harness models (ROM16, EasyFlash 1, MegaByter, GMod2, C64MegaCart)
  When  apps/trex_crt/host/run.sh runs dump, scan, inspect, write and flash on each
  Then  the recorded bus traffic is byte-identical to host/golden/stock.txt
  And   no flash_chip_info_t exists in the tool any more

Scenario: S2 the TwoMegabyter works without a line of type code
  Given the CRT 89 test cartridge from tests/cart/twomegabyter/build.sh in ue2emu's cartridge slot
  When  the tool probes, dumps, writes and verifies it
  Then  the probe reports "Protovision TwoMegabyter", 128 banks, 2 MiB
  And   the dumped image equals emu_cart_save's image (compared as the 2 MiB chip image, not as CRT bytes:
        the firmware writes one 16 K packet at $8000 per bank, ue2emu writes 8 K at $8000 plus 8 K at $A000)
  And   git diff shows no change under apps/trex_crt/src for the type itself

Scenario: S3 an ambiguous descriptor set fails the build
  Given two descriptors whose observable probe signature is identical and no exception entry for them
  When  gen_cart_descriptors.py runs
  Then  it exits non-zero and names both types
  And   with the exception entry present it emits a total order

Scenario: S4 a new cartridge is one file
  Given a synthetic descriptor added to the set in the generator's own test
  When  the generator and the host harness run
  Then  the type appears in the probe order and in the tool's type list with no C change

Scenario: S5 the core has no UI
  Given the core objects (probe, dump, write, verify) and the port layer
  When  the host harness links them WITHOUT cart_screen.o and without the window functions
  Then  it builds and the scenarios of S1 and S2 pass through the event and question callbacks

Scenario: S6 EasyFlash 3 is untouched
  Given the EF3 model of the host harness
  When  run.sh runs its dump and write
  Then  its golden rows are unchanged and its slot path is the same code as today

Scenario: S7 detection writes no flash
  Given any modelled cartridge
  When  the probe runs
  Then  only IO1/IO2 register writes and ROM-window reads occur, no flash command sequence
  And   every register is left at its descriptor reset value
```

| # | Solves | Scenario | Test | Where |
|---|---|---|---|---|
| S1 | P1 P2 P3 | equivalence of the five flat types | `apps/trex_crt/host/run.sh` vs `host/golden/stock.txt` | host, app CI |
| S2 | P7 P1 P2 | TMB round trip | ue2emu + `tests/cart/twomegabyter/build.sh` | Mac |
| S3 | P3 | ambiguity is a build error | `tools/test_gen_cart_descriptors.py` (new) | CI `lint` |
| S4 | P3 P1 | new type = one file | same | CI `lint` |
| S5 | P5 P6 | core links without a view | `host/run.sh` headless target (new) | host, app CI |
| S6 | P4 | EF3 unchanged | `host/run.sh` EF3 rows | host |
| S7 | P3 | probe is read-only on the flash | `host/run.sh` write-inhibit counter | host |

Negative, must stay true: the write inhibit (`ROM_WR_EN`) behaves as in T57 H-tests; the deferred reset on
our core still happens; `apps/trex_crt` still builds for both Nios II and RISC-V; the firmware's own
`trx_cart.cc` flash path is not changed by this task.

## §3 Decision

- **D1 The inverse goes into the shared header.** `software/io/c64/trx_flash_geom.h` gains
  `trx_chip_access(part, desc, chip_addr, chip) -> {bank, window, cpu_addr}`, the inverse of
  `trx_image_offset()`, plus the register writes the descriptor's `rules`/`bank` need to reach that bank.
  Header-only, `<stdint.h>` only, so firmware, app and host test share one implementation. The existing
  host test `software/io/c64/tests/trx_flash_geom_test.cc` is extended; it already covers 0x2F.
- **D2 The tool reads the generated table.** `apps/trex_crt/Makefile` gets
  `-I../../software/io/c64` (today `INC = -I.`), includes `trx_cart_desc.h` and `trx_flash_geom.h`, and the
  six `flash_chip_info_t` plus `FLASH_MODE_*` disappear. Flash command addresses become the data sheet's
  chip addresses from the profile; where they reach the chip is D1's job.
- **D3 The probe comes from the descriptor, the order from the generator.** For each candidate the generic
  probe derives from `rules`/`bank`/`table[]`/`layout`/`io1`/`io2` which register write to make and which
  window read must change, restores the reset values, and scores the candidates. The generator computes the
  discrimination order from the descriptor set, bakes it into `trx_cart_desc.h`, and **aborts the build**
  when two types are indistinguishable — the same class of check it already makes
  (`sectors do not add up to the chip`, `more than 6 sector runs`). Weighed and rejected: a hand-written
  probe-flow file as the single source. It is a second truth — a descriptor added without touching it would
  be silently invisible — and a probe order is this tool's concern, not a property of the cartridge, so it
  does not belong in the portable per-type record (PROP-002 §9.2 "one JSON object per type", shared with
  IcePi and TRX64).
- **D4 Exceptions are a short list, not the flow.** `tools/cart_descriptors/probe_exceptions.json`: true
  twins where the facts cannot decide (Magic Desk vs Ocean 8K), and the two probes that are not mappings at
  all (GMod2's M93C86 test, EF3's slot register). JSON, not TOML: the generator already reads JSON, the
  `.u2a` parses nothing at run time (everything lands as `const` in the header), so a second format buys
  nothing and costs maintenance.
- **D5 The UI is cut from the logic, one core and thin views.** Three modules instead of one:
  - `cart_core` — probe, dump, write, verify, erase. No window, no key codes, no `printf`. It reports
    through `cart_event_t` (phase, bank map change, progress, log line, result) and asks through
    `cart_ask_t` (a question with its choices, answered by the view).
  - `cart_port` — the platform: bus read/write, cartridge mode, file read/write, heap. One struct of
    function pointers, filled by the ABI 2 app (`g_api->c64`, `g_api->trx`), by ABI 3
    (`machine`/`fs`/`job`) and by the host harness's mock. P6's 48 direct `g_api->` sites move in here.
  - `cart_view_text` — today's 40 x 22 character screen, the only consumer of `cart_screen.c`.
  MultiCore builds `cart_view_lvgl` against the same event and question types (T104.3: declarative app UI,
  UX concept `docs/lvgl_ui_project`). The host harness linking without any view is scenario S5, so the cut
  is proven by a build, not by a claim.
- **D6 TwoMegabyter is a JSON entry.** `2f_twomegabyter.json` exists and is complete; the CRT id is 89
  (PROP-002 Rev C, confirmed by the TRX64 session 2026-11-06, their spec 918, and by ue2emu
  `crates/c64-bridge/src/slot.rs:199`). The tool gets no TMB code. The dumper writes the firmware's packet
  shape — one 16 K packet at `$8000` per bank — because `c64_crt.cc:548-551` regenerates exactly that, and
  the firmware accepts both shapes for a `linear` type (`c64_crt.cc:516-518`).
- **D7 The container concept belongs in PROP-002, not here.** EF3 is not special in kind, only in depth:
  its slot is already the high segment of a two-register bank field. What is missing is composition — a
  type that says "slot = `bank[0]`, n = 8, child = CART_TYPE 0x11, protected = [0], directory = {…}, the
  container's flash part wins". That is PROP-002 Rev E, written with Alex, and it also makes the EAPI
  choice and the erase scope fall out of the data. Until it exists EF3 keeps its own slot code (S6), as a
  sequencing decision, not a verdict about its nature.

Order of work: D1 → D2 → S1 green → D5 → D3/D4 → D6. The equivalence against the golden traffic must be
green **before** anyone writes a real cartridge again: EF1, MegaByter and GMod2 are on Alex's desk and
T57's B8 showed real highscores on them.

## §4 Out

Decided with Alex, not by omission:

- **The LVGL view itself.** MultiCore builds it (T104.3). This task delivers the core, the port and the
  event/question contract, and the text view.
- **The ABI 3 port of the tool.** Same place: the rebuild is MultiCore's T104.3 item 5. D5 is what makes it
  a port instead of a fork.
- **PROP-002 Rev E (containers) and the EF3 implementation on it.** Named in D7, a task of its own.
- **The EAPI swap as data.** Stays `eapi_drivers.c` plus a type test until Rev E gives the container its
  part; the EAPI driver must match the physical chip, not the child's nominal one.
- **GMod4.** Not a nesting problem: SPI flash, so the AMD command and sector model of the `flash` block
  does not apply (`T16.2:91`, `T33:19`), and it pulls EXROM/GAME from the *address* within 10 ns
  (`T16:204`, `T16:360-361`) — PROP-002 §2.1's "Timing" class. No hardware and no prototype
  (`T16:788`).
- **A board test of TwoMegabyter.** No hardware here (`docs/board-tests-pending.md:180`). S2 proves it
  against the emulator and the host models; that is the honest claim, and `README.md` and the T57 matrix
  will say so.
- **GMod3 (type 62).** Probe stays as it is today.
- **Erase Cartridge in the F5 menu** (U1) and the lens path (T57 §9).

## §5 Open questions

- **Q1** The probe today is read-only on the flash but it does write IO1/IO2 registers of an unknown
  cartridge, and a descriptor-driven probe will try more candidates than the hand-written chain does. Is
  that acceptable on your own cartridges, or should the generic probe stop at the first match and offer
  "not recognised — choose the type" instead of trying every candidate?
- **Q2** `probe_exceptions.json` or the same information as a `probe` block inside the two affected
  descriptors? (D4 proposes the separate file; the counter-argument is that it is a third file to keep in
  sync.)
- **Q3** PROP-002 Rev E now, together with this task, or after it? (D7 proposes after.)

## §6 Decisions taken while building

**D1 (done 2026-11-06), the inverse and the register side.** `trx_flash_geom.h` gained
`trx_chip_access()` (chip address -> bank, window, offset), `trx_window_cpu()` (where a window answers in a
given mode, with GAMEW counting only in a write cycle), `trx_flash_window()`, `trx_flash_cmd()`,
`trx_flash_cmd_needs_bank()`, and the register derivation `trx_desc_write_addr()` / `trx_desc_seg()` /
`trx_desc_bank()` / `trx_desc_mode()` / `trx_desc_reg_value()`. `tests/trx_flash_geom_test.cc` went from 25
to 134 checks: the round-trip invariant over every part and every address, all six command-address pairs the
tool used to carry (including GMod2's `$F555` in bank 2 and the C64MegaCart's `$EAAA`), the read direction,
and the $DE02 / $DF00 / $DE00 registers with their two values per type.

Four things turned out not to be derivable and became explicit data in the `flash` block, because guessing
them from the mode table is wrong:

- `flash.wmode` / `flash.rmode`: `"w": 1` in the table says *where writing is allowed*, not which mode a
  programmer selects - EasyFlash marks three of its eight modes. Both are required and validated against
  the table; a missing or impossible value fails the generator.
- `flash.select` already existed but was not in the app's profile; it decides the programming window
  (GMod2 and the C64MegaCart are written through ROMH and read through ROML), so `trx_flash_window()` takes
  the direction.
- `flash.unlock_bank` (new, `0` for the EasyFlash family, absent elsewhere): skoe's EAPI requires bank 0 for
  the command cycles and the target bank for the data byte. That is a property of the board, not of the
  address decode, so it is data rather than a `FLASH_MODE_DE02_EF` branch.

The 43 descriptor bytes did not change (checked line by line), so gateware, firmware and the Verilog
benches are untouched; `trx_cart.cc` uses only `trx_image_offset()` and `trx_erase_span()`, whose signatures
stayed.

**D2 (done 2026-11-06), the tool reads the generated table.** `apps/trex_crt/Makefile` and `host/run.sh`
carry `-I../../software/io/c64`; the new `src/cart_flash.c/.h` derives one `cart_flash_t` per type with
flash from `c_cart_descs` - name, CRT type, bank count, bank size, total size, ROMH, the bank and mode
registers with their two values, and whether bank and mode share a register. The six `flash_chip_info_t`
literals and `flash_mode_type_t` are gone. Every number the structs held comes out identical, TwoMegabyter
and EasyFlashXl come out for free (128 banks / 2 MiB and 256 banks / 4 MiB), and the generator now also
emits `crt_type` and a `short_name` for the 40-column title.

- **The traffic did not change.** `host/run.sh`: 34541 checks, 0 failures, and the per-cartridge access
  counts are the same to the access (ef1 8454, megabyter 1897, gmod2 372822, megacart 2720, ef3 12487), so
  the H1 golden digests of `golden/stock.txt` still match. That is **S1 green** - and the reason the
  rebuild kept every sequence byte for byte instead of tidying it.
- **GMod2 and EasyFlash 3 keep their own sequence**, as a sequencing decision: their addresses, banks,
  windows and register values now come from the descriptor (`gmod2_flash_cycle()` is four lines of
  `trx_flash_cmd()`, and even its "deselect the EEPROM first" value falls out of the EEPROM's CS pin in
  descriptor byte 33), but the *order* of their cycles stays hand-written. Making GMod2 take the generic
  erase path would change the poll address from `$8000` in read mode to `$F555` in flash mode: allowed by
  its `flash.select`, not proven on the board, and it would move the golden traffic. That belongs with D3,
  with Alex's cartridge on the desk.
- **`crt_type_banks()` is data now**, which widens what the writer accepts on purpose: CRT 89
  (TwoMegabyter, 128 banks) and 232 (EasyFlashXl, 256) are no longer refused with "no cartridge the writer
  can flash". The old aliases stay hand-written: 19 for old C64MegaCart dumps, and 90 for the whole
  EasyFlash 3 image, which is the container of D7.
- The cross build (nios2-elf-gcc, riscv32-unknown-elf-gcc) runs in the container and is **not** verified
  here; `cart_flash.c` compiles for rv32im freestanding, the rest only on the host (clang, `-std=gnu11`).

**D5 (done 2026-11-06), the UI is cut from the logic.** Three layers instead of one file per tool:

- `src/cart_port.h` + `cart_port.c` + `cart_port_u2p.c` — the platform as one struct of function pointers:
  bus (`peek`/`poke`/`dma_read`/`cart_mode`/`setup_bus`/`restore_bus`/`cart_detect`/`cart_preference`/
  `reset_machine`), the port's register block (`reg_rd`/`reg_wr`/`has_regs`), time, heap, files and `log`.
  All 48 `g_api->` sites of the cores moved into `cart_port_u2p.c`; a later ABI is one more file like it.
  `FRESULT`, `MODE_NORMAL` and `LOG` live in `cart_port.h` now, so no core file includes `u2p_app.h`.
  The one `#ifdef HOST_TEST` left in the tree sits in that filler, where the harness's model of `trx_sys.v`
  goes behind the registers — `cart_base.c` is free of it. `mock_trx_rd8()` answers the ID the scenario
  configured instead of a hard-wired "TRX2", which is what H8 (bad ID -> Gideon) now tests through the port.
- `src/cart_view.h` + `cart_view.c` — what the cores report (`cart_event_t`: status, bank map, header,
  progress, redraw, result) and what they ask (`cart_ask_t`: notify, confirm, choice, text, path, save).
  With no view installed every report is dropped and every question takes its safe answer, so the cores run
  headless.
- `src/cart_view_text.c/.h` — today's 40 x 22 screen, the only consumer of `cart_screen.c` and the only
  place that knows `KEY_F1`, a `Window` or an SDK dialog. `cart_writer_init/deinit/poll` became
  `cart_writer_open/close` plus `cart_writer_scan/write/verify/select`, same for the dumper, and the key
  switch, `draw_header/draw_status/draw_footer`, the layout, the window and the dialog rebuild sit in the
  view. `src/cart_fmt.c` holds the tools' `snprintf` (was `ui_snprintf` in the shim) so the cores format
  their lines without the UI.

- **The traffic still did not change.** `host/run.sh`: 34541 checks, 0 failures, the same access counts as
  D2 (ef1 8454, megabyter 1897, gmod2 372822, megacart 2720, ef3 12487). **S1 stays green.**
- **S5 is a second binary, not an argument.** `host/core_only.c` links `cart_base`, `cart_flash`, `cart_fmt`,
  `cart_port`, `cart_port_u2p`, `cart_view`, `cart_dumper`, `cart_writer` and the mock — without
  `cart_view_text.c`, `cart_screen.c`, `ui_shim.c` and `main.c` — and then probes, scans, writes and verifies
  a MegaByter with `g_view == NULL`, comparing the flash against the CRT (12 checks). A core that reaches for
  a window again fails that link. The CRT builder of the harness moved to `host/crtbuf.c` so both drivers
  share it.
- **Two lines of screen text became state instead of a draw call**: the writer's `img_slot` and the dumper's
  `slot_shown` (-1 = none). The core says which slot it is working on and the view decides what to write in
  the slot line.
- **The hex viewer stays a view.** `cart_inspector.c` is not part of the core: it shows bus bytes, has no
  bank map and no progress, and draws its own window. It goes through `cart_port` for the bus and through
  the shared questions, and `cart_view_text_inspector()` installs the view around it.
- The cross build is **still not verified** here: Apple clang has no `riscv32` target and no freestanding
  `string.h` for it, and there is no container on this machine. `<string.h>` now comes into the cores
  directly instead of through `u2p_app.h`, which both toolchains provide as before.

---
<!--
Closing: Status -> DONE (date) + which scenarios are green where; TODO row and history entry, plus
apps/trex_crt/README.md, T57's matrix rows F10 and "TwoMegabyter", ADR-0006 Q6 and
docs/board-tests-pending.md, in the same commit as the code.
-->
