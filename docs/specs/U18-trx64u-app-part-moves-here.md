# U18 — trx64u's application part moves here: a library on MultiCore's app core

**Status:** IN PROGRESS (2026-11-22, Alex's go; branch `u18`). SDK 0.1.0 pinned, MultiCore's sources in
`software/trx64u`, their boundary measured (`software/trx64u/BOUNDARY.md`) and given to MultiCore; nothing builds yet.
Under discussion: the trx64u tests leave MultiCore's pipeline (§3).
**Repo:** TRX64-Ultimate (the trx64u library, its menu, its tests, the `.cor`); TREX/TRX64U-MultiCore (app core, SDK).
**Number:** U18 (registry: the TODO table in `TODO.md`).
**Milestone:** MultiCore T239 step B (T239.3), after their steps A (GitHub home, T239.1) and D (platform checkpoint,
T239.2).
**Origin:** Alex, 2026-11-20 (translated): "Read T239 and T239.3 in MultiCore's docs/specs. We are moving the app build
into the cores, so then you have your UI here completely too - a change to the contract, I know. Leave the session
alone, but you can prepare your part and review / challenge the concept." Then: "Open a spec for it (U?), with your
questions and wishes."
**Depends on:** MultiCore T239 (D9, D10, D13), T239.3 (D1, D2, D5), their hand-over note `.claude/handover-trx64u-ui.md`;
here [U3](U3-generic-app-replaces-gideons-app.md) and ADR-0057 / ADR-0058; the contract
[`docs/contracts/trx64u-firmware-handover.md`](../contracts/trx64u-firmware-handover.md).

---

## §1 The problem

MultiCore decided (T239 D9, D10; T239.3 D2) that `trx64u`'s menu, its code and its limits belong to TRX64-Ultimate,
built against MultiCore's SDK as a library on their app core. Today all of it lives in MultiCore and is linked into the
one app image every blueprint core runs.

**What comes here** (read in MultiCore `main` 7213212a2, 2026-11-20):

| Part | Where it is today | Note |
|---|---|---|
| Menu | `coreapp/cores/trx64u/menu.toml` (1,313 lines), compiled to `menu_rec.inc` | record 6 of the `.cor` |
| Feature code | `coreapp/src/trx64u_ops.cc`, `u64cfg.cc` | T228-R20 ledger exceptions |
| Monitor, RPC server, ring | `coreapp/src/monitor.cc`, `monsrv.cc`, `ring.cc`, `ringfile.cc`, `ringops.cc` | T239.3 D2 |
| Monitor engine | `coreapp/src/trxmon/` | a copy of our `apps/trxmon/src` (its README, PROVENANCE) |
| Tests | `tests/coreapp/golden/ops_trx64u.txt` | pinned as `OPS=` in `release/imports/trx64u.pin` |
| Files | `release/imports/trx64u_files` (SID player) | record 8; `module.bin` is dropped (T239.3 D5) |

The code rows plus `trxmon/` are 24,884 lines (`wc -l`, headers included).

**What the code says about the move:**

1. **The TRX service tables stay behind.** `coreapp/src/appsvc.cc` implements the tables a sub-app sees,
   `SvcC64` (`g_c64`, :95), `SvcTrx` and `SvcRing` (`svctab.h:166-209`), directly on `trx_io.h` and `trx_ring.h`
   (:11, :13). D2 moves the ring code but not these tables. `apps/trex_crt/src/trex_main.c` and `cart_base.c` call
   them.
2. **The RPC server is polled by the app's main loop.** `run.cc:1068` calls `mon_serve(&ctx)` ("the RPC on TCP 4312,
   whatever the menu does"). A library's poll is declared (`LibPollDecl`, `libapi.h:115`) but nothing in `coreapp/src`
   calls it.
3. **No C++ constructors in a library.** `app/lib/lib.ld` has no `.init_array`, and `libload.cc` runs none. Global
   objects with a constructor in the moved `.cc` files would never be built.
4. **1 MiB per library.** `U2A3_IMAGE_MAX` = `0x00100000` (payload + bss, `libfmt.h:40`); libraries are `rv32im`
   without compressed instructions (`lib.mk:23`). Whether trx64u fits in one library is not measured.
5. **A large core needs the app core image.** `tools/pack_import_cor.sh` packs `trx64u.cor` with `--app-bin coreapp.bin`.
   The SDK contents of T239.3 S18 (headers, ABI document, `lib.mk`, `u2a_pack.py`, `descrec.py`, menu simulator,
   example) do not include the app core image.
6. **Two copies of trxmon have drifted.** Compared with our `software/ultimate-app/apps/trxmon/src`: `asm6502.h`,
   `dis6502.h`, `rpc.h` are the same; `mon.h` and `ring.h` differ. MultiCore also carries `apps/trex_crt` beside the copy
   of ours.
7. **The documents disagree.** Contract §10 gives the feature logic (ring, monitor, streams, U64 registers) to MultiCore
   as generic modules and leaves TRX64-Ultimate the TOMLs; T239 D9 and T239.3 D2 give that logic to us as a library.
   ADR-0057 point 6 (TRX64-FW works in a clone of MultiCore, MultiCore reviews) and U3 §2 ("Where the code lives") assume
   the old split.

**Parts:** P1 what we deliver and how it is packed; P2 the library (code, size, API use); P3 one trxmon source; P4 build
and test here; P5 the documents (contract, ADR, U3).

## §2 Scenarios - what proves it is solved

| # | Part | Scenario | Proof |
|---|---|---|---|
| S1 | P2 | The three libraries of D5 build here from the SDK alone, each within the loader's limits, and the app core image built for the other cores holds no trx64u symbol | our CI; MultiCore T239.3 S8 |
| S2 | P2 | On the board trx64u starts, lists its menu, answers RPC on TCP 4312 from the start, runs a TREX CRT dump, a drive mount and Timewarp scrub | board step with Alex (the T228 §5 checklist plus RPC and Timewarp) |
| S3 | P3 | One trxmon source: our firmware's trxmon and the trx64u library build from the same files | build of both; no copy left in MultiCore |
| S4 | P4 | Our CI runs the trx64u menu and ops tests (golden ops, menu simulator) without a board | CI stage |
| S5 | P5 | Contract, ADR-0057 and U3 say the same as T239 | `diff` of the two contract copies; review by Alex |
| S6 | P4 | Lint, apptests and emu tests of trx64u run here as three stages of their own; MultiCore's pipeline runs none of them again | our CI; MultiCore's `ci/lint.sh`, `tests/coreapp` and `tests/coreapp/emu` hold no trx64u unit |

## §3 Decision

Applies from MultiCore: T239 D9, D10, D13; T239.3 D1 (libraries declare their budget and load with the app core;
sub-apps load on demand), D2, D5.

- **D1 (Alex, 2026-11-20): our package delivers the complete `trx64u.cor`** until the registry exists. It is built here:
  our bitstream, a pinned app core image, our libraries, the menu record and the files. MultiCore pins that one file.
  Asked against "we deliver the pieces, MultiCore packs as today". Reason given with the question: it is already the
  shape the registry needs (T239 D5, D9). Price: we pin MultiCore's app core version (R1).
- **D2 (Alex, 2026-11-20): U3 and ADR-0057 stand.** The standalone TRX64 Ultimate firmware becomes the app core plus our
  libraries; Gideon's app stays only as `trx64g`. Until then both UIs are kept here; the goal is one. Asked against
  "the standalone firmware stays Gideon's app, trx64u is only the MultiCore core".
- **D3 (Alex, 2026-11-20): our libraries implement `SvcTrx`, `SvcRing` and `SvcC64`** and export them through `SvcLib`;
  MultiCore's app core keeps no TRX hardware code (D24). trex_crt and trxmon then find the tables through `lib->find`
  instead of `api->trx` / `api->c64`. Both apps are ours. Asked against "the app core keeps them" (its price: a change of
  our bitstream needs MultiCore again).
- **D4 (Alex, 2026-11-20): one trxmon source, here.** Our firmware and the monitor library build from the same files;
  the drift (`mon.h`, `ring.h`) is merged once, here, and MultiCore drops its copy with the move.
- **D5 (Alex, 2026-11-20): one library per function, the cartridge tool stays a sub-app.** Alex asked: "Doesn't it make
  sense to build one lib per function? One for the ring, one for the monitor, one for the cartridge tool?" (translated).
  The cut:

  | Part | Form | Holds |
  |---|---|---|
  | `trx64u` | library, the base | menu ops, settings (`u64cfg`), `SvcTrx`, `SvcC64` |
  | ring | library | the ring code, `SvcRing` |
  | monitor | library, needs ring | the trxmon engine, the monitor, the RPC server on TCP 4312 |
  | trex_crt | sub-app | as today, loaded on demand |

  The cartridge tool stays a sub-app, because T239.3 D1 loads sub-apps on demand into the free heap, so it costs memory
  only while it runs. Ring and monitor are resident anyway: the ring records all the time, and the RPC server answers
  from the start. The split saves no heap; it buys clear edges, every library well below 1 MiB, and the same pieces for
  our firmware (D2). Price: the call tables between our own libraries (ring to monitor) are versioned by us; the loader
  carries the dependency (`needs`, at most 4) and the exports (`SvcLib find(name, min_version)`).

- **D6 (MultiCore, by Alex's delegation, 2026-11-22): the ring library's copy is the master; the firmware's ring is
  frozen.** The library's ring files (`software/trx64u/ring/`) are brought once to the firmware's state (64 sections,
  `load_entry`, `save_native` / `.trxring`) and are the master from then on. The firmware's copy (`trx_ring.cc`,
  `trx_c64re*.cc`, `trx_trxring.cc` in the application submodule) takes bug fixes only, and every such fix goes into
  the library in the same change; new ring features go only into the library. Asked against "one source with the
  firmware, as trxmon (D4)": that refactors code with an end date (D2, U3) and needs a firmware build plus a board
  check of Timewarp.

### Decided: the trx64u tests leave MultiCore's pipeline (Alex, 2026-11-20 and 2026-11-23)

Alex (translated): "Lint, apptests and emu tests must then also run separately in the pipeline, *out* of MultiCore's big
monolithic pipeline and over to you. MultiCore then does not have to do all of that again."

What MultiCore's push pipeline runs for trx64u today (`ci/children.txt`, read in `main` 7213212a2):

| Stage | Where | The trx64u part |
|---|---|---|
| Lint | `ci/lint.sh`, a step of the parent pipeline | "TRX64U menu record" (`tools/gen_menu_rec.py --check`, :80), "TRX64U menu against the UX concept" (`tools/check_menu_concept.py` on `tests/coreapp/golden/menu_c64.txt`, :84), the import `.cor` pack test (`tools/test_pack_import_cor.sh`, :75) |
| Apptests | child `apptests` (`apptests.yml`, `ci/apptests.sh`, `tests/coreapp/run.sh`) | 18 test sources under `tests/coreapp` name trx64u. `tests/coreapp/cores.txt` has no trx64u group (its groups are files of `coreapp/cores/`; `trx64u_ops.cc` is in `coreapp/src`), so these units fall into the shared suite or `cross`: a trx64u change reruns MultiCore's shared core-app suite |
| Emu | child `emu` (`emu.yml`, needs `images`), suite `coreapp_emu` | `tests/coreapp/emu/run.sh` and `library_n.sh` on `coreapp_trx64u.app` (T228 R25) in ue2emu with `--trx-io on`, job image `u64-build-env:emu-v0.7.0` |

The wish: three stages of our own here (lint, apptests, emu), each keyed on our files; MultiCore's pipeline drops the
trx64u units and does not run them again.

Decided by Alex on 2026-11-23 (translated): "all of it; in principle you should build and manage .ue2, .cfw and .cor on
your own. Only the SDK comes from MultiCore." So every trx64u test moves here, and this repository builds, tests and
delivers the firmware (`.ue2`, `.cfw`) and the core (`.cor`); MultiCore delivers the SDK and nothing of trx64u.
Open, to settle with MultiCore (advisor):

- Which of the 18 sources test trx64u itself (they move here) and which test the app core through trx64u (they stay with
  MultiCore, against another core or a stub library).
- What MultiCore still checks of the `.cor` we deliver (D1): that it loads (T239.3 S8), or nothing.
- What we need from MultiCore for it: the test rig of `tests/coreapp` (stubs, fakes, rigs), the two menu tools, the
  ue2emu image with TRX I/O, versioned with the SDK (R9).

### Open questions for Alex (one at a time)

1. ~~What does our package deliver?~~ Answered: D1.
2. ~~Is trx64u the future firmware of TRX64 Ultimate itself?~~ Answered: D2.
3. ~~Who implements `SvcTrx`, `SvcRing` and `SvcC64`?~~ Answered: D3.
4. ~~One trxmon source here?~~ Answered: D4.
5. ~~One library or two?~~ Answered: D5 (three libraries and a sub-app).

### Requests to MultiCore for T239.3 (read by MultiCore 2026-11-20; R1 in SDK 0.1.0, R2 in work, R3 and R4 decided by Alex 2026-11-22)

- **R1** The SDK release ships the app core image (`coreapp.bin`) with its version, so that a large core can be packed
  outside (§1 item 5).
- **R2** The library poll (T239.3 S3) is served before trx64u moves (P4); the RPC server needs it from the start.
- **R3** The loader runs `.init_array`, or `lib.mk` refuses a library that has one (§1 item 3). **Decided (Alex,
  2026-11-22): the build refuses** a library with constructors or destructors (`.init_array`, `.fini_array`,
  `__cxa_atexit`); asked against "the loader runs them". Ours: `FileSystem_Root` and `mstring` in `ringfile` /
  `ringops` go with the move to `SvcFs`.
- **R4** T239.3 P1 records D3: the TRX service tables leave the app core and come from our libraries through `SvcLib`;
  the base API keeps no `trx` / `c64` implementation for the monolith.
  **Decided (Alex, 2026-11-22): yes**, the tables leave the base API; asked against "they stay in the base API" (base
  3.2 taking our ring layout). The ring library's own table through `SvcLib` (MultiCore's interim rule) is the rule now.
- **R5** The trx64u share of today's app image is measured per library of D5 against `U2A3_IMAGE_MAX` before P4 is cut.
- **R6** The golden ops file, `descrec.py` and the menu simulator are in the SDK, so that our CI tests the menu.
- **R7** An ABI major change is announced before release, so that trx64u is rebuilt before a package ships it.
- **R8** Contract §1, §8 and §10 are rewritten together with this move; ADR-0057 point 6 is replaced.
- **R9** (follows the discussion above) The test rig of `tests/coreapp`, `gen_menu_rec.py`, `check_menu_concept.py` and
  the ue2emu job image with TRX I/O ship with the SDK; MultiCore's lint, apptests and emu drop the trx64u units once ours
  run.

## §4 Out

Nothing decided yet.

## §5 Status and history

- **2026-11-20: opened** (Alex). Review of MultiCore T239 / T239.3 against their code (read only, `main` 7213212a2);
  findings in §1, questions and requests in §3. Preparation that touches nothing in MultiCore, not started:
  1. the trxmon drift, file by file (our `apps/trxmon/src` against their `coreapp/src/trxmon`);
  2. in the files that move: global objects with constructors, and calls outside `LibApi` (FreeRTOS, lwIP, FatFS, MMIO);
  3. the size of the trx64u parts in a map file of `coreapp`, built in a scratch copy;
  4. the build pieces missing here: SDK pin, app core image pin, `descrec.py`, `u2a_pack.py`, `cor_pack.py`, menu
     simulator, and the RISC-V `g++` that `lib.mk` expects.
- **2026-11-20: D1-D5 answered** (Alex, one at a time): the complete `.cor` from here, U3 stands, our libraries implement
  the TRX service tables, one trxmon source here, three libraries (base, ring, monitor) and trex_crt as a sub-app. R4
  and R5 follow D3 and D5.
- **2026-11-20: tests put under discussion** (Alex): lint, apptests and emu tests of trx64u leave MultiCore's pipeline
  and run here as stages of their own. What MultiCore runs today is listed in §3; S6 and R9 added.
- **2026-11-22: started** (Alex, late: *"die MC session wird dir irgendwann das SDK rüber reichen ... Dann baust du um, hier
  und lässt dir von der MC session helfen"*; then *"danach gib Gas. Vor allem der UI Performance muss richtig gut werden"*).
  MultiCore handed over SDK 0.1.0 (from `t239.3-sdk` `3b8e2955`: public headers, `lib.mk`, packers, the app core image
  of R1). Branch `u18`, `software/trx64u`:
  - `sdk.pin` and `fetch_sdk.sh` (the SDK is not in git; the tarball is checked against its sha256); the SDK's example
    builds in `u64-build-env` with `riscv32-unknown-elf-` (a Mac's `riscv64-elf-` has no newlib headers).
  - `base/`, `ring/`, `monitor/`: MultiCore's files copied unchanged from `3b8e2955` (14,465 lines with `menu.toml`).
  - `BOUNDARY.md`, from `tools/boundary.py`: MultiCore's core application built from `3b8e2955`, the moving objects'
    symbols read with `nm`. trx64u 80 symbols from 28 providers, ring 90 from 20, monitor 48 from 8; the core
    application calls back into the moving code from `appsvc`, `c64`, `run`, `screens`, `diag` and five more. The
    preparation's "no constructors" holds for global objects; `ringfile` and `ringops` use `__cxa_atexit`
    (`FileSystem_Root`, `mstring` with destructors).
  - MultiCore's answers: the C64 descriptor `c64.cc` splits, its data into the menu record's new sections (D8,
    branch `t239.3-desc`), its hooks into the base library (`LibRecord` gets the core hooks, D10); `cfg_value` and
    `cfg_text` go into their core application; the symbol mapping comes with base 3.2 / SDK 0.2.0. Interim rule for
    the ring table while R4 is open: the ring library exports its own table (the firmware's `u2p_ring_api_t` with
    `dump`, `save`, `load`, `trace_writes`, `load_entry`, `save_native`, which the SDK's `SvcRing` lacks) through
    `SvcLib`, and trxmon takes it with `lib->find`.
  - Size per library (R5) from MultiCore's own trx64u image: trx64u 397 KB of image and bss (37.8 % of the 1 MiB
    limit), ring 84 KB (8.0 %), monitor 646 KB (61.6 %); mostly static buffers (`BOUNDARY.md`).
  - `build.sh` and a `Makefile` per library. trxmon (D4) builds in the monitor library unchanged against the
    firmware's ABI headers; only `plat_*` / `srv_*` are left, the platform its glue gives. The three libraries stop
    at their first header from MultiCore's core application.
  - Menu speed (T239.3 D4): MultiCore builds a measurement in its menu simulator (`t239.3-speed`), flushed pixels per
    key press, trx64u against the A1200; both sessions exchange its numbers before and after each change.
- **2026-11-22, evening: R3 and R4 decided** (Alex, one at a time). R4: the TRX tables (`SvcTrx`, `SvcRing`, `SvcC64`)
  leave MultiCore's base API and come only from our libraries through `SvcLib`. R3: `lib.mk` refuses a library with
  constructors or destructors; the loader runs none.
  MultiCore recorded them as T239.3 D19 and D20 (integration `5f49fe2f`). Proposed to MultiCore the same evening:
  - `trx` and `ring` are ours: exports "trx" and "ring", major 1, with the firmware's ABI header (`app_api.h`) as their
    one definition (`u2p_trx_api_t` with `detach`, `stop_requested`, `ui_request` and `ring` null in the library;
    `u2p_ring_api_t` whole). Within a major, members are only appended (size first); reorder or remove is a new
    major. MultiCore accepted all three and changed the mechanism: base 3.2 gets named exports (`LibRecord` lists
    {name, major, table, size}; `find` and `LIB_NEEDS` match export names before library names), so one library
    exports "trx" and "c64" and the major is the export's own. We build to `find("trx", 1)`, `find("ring", 1)`,
    `find("c64", 1)`.
  - `c64` gets an SDK header (MultiCore's; other C64-family cores may provide it). Our base library provides it for
    trx64u. Today's `SvcC64` plus `dma_read_frozen`, `cart_preference` and `reset` (from `SvcTrx.reset_c64`).
  - MultiCore's cartridge module (`c64_cart.cc`, D12) drives our bitstream's cartridge engine through its register map,
    not through `SvcC64`; for a second core it needs the module's base address from the core's API TOML (T239.3 D8).
- **2026-11-22, night: MultiCore is the advisor** (Alex, going to bed: *"Die MC Session ist dein advisor, sie darf
  Entscheidungen treffen frag sie also ab jetzt"*). Its answers count as decided and are marked "MultiCore, by Alex's
  delegation"; GitHub, merges into `main` and board steps stay Alex's.
  - **Q1 (MultiCore, by Alex's delegation): port now, against base 3.1.** Within ABI major 3 the API is append-only
    (S5, header lock in their lint), so nothing ported against 3.1 changes shape; 3.2 appends named exports, the
    library budget, the `C64Api` header and the D19 refusal; 3.3 appends the core hooks and the new generic calls
    (sync, settings, board and video services, `cfg_value` / `cfg_text`, `ops_call` / `ops_state`). Now: the monitor's
    `plat_*` onto `SvcSys` / `SvcFs`, `srv_*` onto `SvcNet`; the registrations as `LibOpDecl` / `LibStateDecl` in
    `lib_register`; `ringfile` onto `SvcFs`; every allocation through `api->mem` (the library's own account, S14); no
    objects with constructors or destructors (D19). The ring library is named "ring", so `find("ring", 1)` works on 3.1
    already; trx stays `api->trx` until the cut. What needs calls that do not exist yet is kept as a list.
  - **The boundary map (MultiCore, by Alex's delegation; their T239.3 §5, map `docs/dev/boundary-trx64u.md` on
    `t239.3-boundary`):** `c64.cc` (the TRX2 descriptor) and `c64_go.cc` come to us, data as TOML, hooks from the base
    library; `c64_cart.cc` and `cbm_run.cc` stay theirs as shared libraries. The base API takes a call only when more
    than one core uses it or it belongs to the generic app: UI look, name squeeze and keyboard-joystick become settings
    of the generic app (our code stops calling them); no raw I2C (asked: `u64cfg`'s only I2C is the SID socket
    expander 0x40 on the 1.8 V bus, proposed as one call `board.sid_sockets(ctrl)`). Base 3.3 appends `sync.enter` /
    `leave`, mutexes, `sys.yield`, `sys.ms_timer`; `net.gated` and the settings are read as states
    (`ops->state("net.gated")`, `ops->state("set.<key>")`). S14: everything a library holds is on its own account.
    `trx_io` read32 / write32 / slim are ours (D20), each 4-byte access inside `sync.enter` / `leave`.
  - Three agents port base, ring and monitor against 3.1; calls that do not exist yet go into a `pending_api.h` per
    library under the 3.3 names.
  - **Ring library ported** (`d12abfa9`): compiles; the link is open only on its `pending_api.h` (44 calls). Found:
    `ops->state("set.<key>")` does not route through MultiCore's `LibLoader::svc_state` at `3b8e2955` (theirs, fixed with
    3.3).
  - **The ring's list (MultiCore, by Alex's delegation):**
    - The C64 machine registers (stop, mode, DMA mem-only, cartridge type, REU enable and size, sampler enable, video
      format) are ours: they go into our core's API TOML (D8); the base gives a generic `io` group (register read/write in
      the core's window, `ddr_read` / `ddr_write`), addresses from our TOML, none from base headers.
    - Exports of their shared libraries: "uci" 1 (the command interface slot), "c64cart" 1 (the cartridge calls, the
      GMod2 EEPROM, the cartridge RAM window), "cbm1541" 1 (`slot_mount`, `drive_info(slot, Cbm1541Info *)` with a
      size-first struct; the drive A image window). No raw addresses in our code.
    - Base calls: `board.fpga_caps`, `sys.wall_time`, `sys.app_version`, `settings.override_value` + `settings.apply`,
      `ctx.job_step` / `job_progress` / `job_cancelled`, `ctx.target_path` (was our `trx_target_path`; `diag.cc` uses it).
    - Menu close: the generic app calls a core hook `menu_closed`, which our base library provides and which calls the
      ring; no "ring.unfreeze" by name in generic code (D24). The engine does not start in `lib_register` (job, net and
      trx are null there) but in the core's start hook or its first poll. The poll {"ring.poll", 100 ms} stays.
    - Memory: the S13 budget declares a permanent figure (the ring: about 1.42 MiB) and a peak (a `.c64rering` dump: up
      to about 4.2 MB more); trx64u's heap of 0x950000 holds it.
  - **Monitor library ported** (`2a646fa3`): compiles; the link is open only on `sync_enter` / `sync_leave` /
    `sys_yield`; with those stubbed it packs (image 203,404 B, image + bss 674,898 B, 64.4 % of 1 MiB).
  - **The monitor's list (MultiCore, by Alex's delegation):** `lib.mk` will force-include `hidden.h` for C++ too (our
    Makefile line stays until 3.3); the base honours a poll's `when_frozen` (3.3), so TCP 4312 answers with the menu open;
    `KF_DIRECT` runs on the menu task without the core lock by design, our guard stays; the generic key table's
    KS_MONITOR / KS_TIMELINE rows move into our UI TOML key table (D7); `mon_release` goes into the `menu_closed` hook;
    the F4 screen becomes the monitor's export "textgrid" 1 (SDK header `textgrid.h`: render, cols, rows, line, attr,
    blank_rows, cursor, sig), named by a menu node with layout "terminal"; `save(path, buf, 0)` creates an empty file;
    `plat_firmware_version` / `plat_firmware_git` map to `sys.app_version` / `sys.app_git`; `net.gated` is "yes" while
    the REST token gate is active, null otherwise; `sleep_ms(0)` sleeps one 5 ms tick, `sys.yield` is the yield.
  - `plat_media` (MultiCore, by Alex's delegation): generic, base 3.3 `ctx.media_source(slot, buf, len)` with a
    media-slot name from the core's descriptor; our `plat_media` maps role 0 / 1 / 2 to the slot names our c64
    descriptor TOML gives the cartridge, drive A and drive B.
  - **Base library ported**: compiles; the link is open only on its `pending_api.h` (59 calls). The three 64 KiB static
    buffers are gone (taken from `api->mem` while in use); about 54 KB of bss stay. `api->trx` and `api->c64` are null
    while `lib_register` runs; the library reads them at each use. Play MOD is gone from the menu (T239.3 D5). Found
    while porting: MultiCore's `u64cfg` passed the range index instead of the number for "UltiSID n 6581 Offset/Base"
    (the offset was off by 1024 in `resid8_plan`), and `uci_drive_do` printed an uninitialised message; both fixed here.
  - **Ring library brought to the firmware's state (D6):** 64 sections, the format 2 header parsed whole (jsmn, up to
    4096 bytes), `load_entry` (format 1 with gzip/deflate, picking a `.c64rering` entry), `save_native` (`.trxring`, port
    of `trx_trxring.cc`), `whowrote`'s I/O scan, the REU gate, U9's cartridge search on load, the missing `.c64rering`
    nodes; all 18 members of the exported table are implemented. The header's cartridge type is now the machine's number
    as the firmware writes it (MultiCore's copy wrote the .crt hardware type). Link open only on `pending_api.h` (47:
    `c64_cart_require` / `prohibit` / `writable` added). Not run yet, no file cross-loaded with the firmware. Open: calls
    through the exported table have no machine info (a dump names no media; a load rebuilds no machine); `op_load` loads
    at once where the firmware defers to menu close.
  - **Found in the firmware's frozen ring (D6: fixes go into both):** `trx_trxring.cc` prints the `.trxring` header with
    `%lu` / `%llu`, which the firmware's `small_printf.cc` does not know (no length modifier; it prints `%l` and takes no
    argument), so the firmware writes `.trxring` files its own loader refuses; the library prints the numbers. The format
    1 stage path leaves `want` and `eeprom` uninitialised (`malloc`); the library uses `calloc`. `.c64re` load has no REU
    check; the library refuses. Firmware fixes are the maintainer's call.
  - **Machine info and menu close (MultiCore, by Alex's delegation):** the ring library builds the machine info for a
    dump or load itself (`ctx.media_source` for the slots, c64cart for type / require / prohibit, `cbm1541.drive_info`,
    the video format from our API TOML register through `io`), so calls through the exported table and the menu ops share
    one path; `c64_cart_require` / `prohibit` / `writable` go into c64cart 1. MultiCore's app puts back no machine state
    when the menu closes (the menu draws on its own layer; `close_menu` in their run.cc only releases the monitor, applies
    a pending ring frame and clears C64_STOP), so `op_load` stays immediate and the core hook `menu_closed` stays `void`.
    It replaces those first two steps: it runs on the menu task with the machine still stopped, without the core lock,
    and a `when_frozen` poll (monitor.serve) may run at the same time.
  - **Menu speed (MultiCore T239.3 P7, `t239.3-speed`, menu simulator on the Mac; board milliseconds not measured):**
    trx64u's menu was the slowest because every cursor key evaluated every `show_state` / hide of the whole tree (93-119
    registry lookups per key, linear over about 190 states; our menu has 96 `show_state`), and a scroll step on the long
    F5 list rebuilt the whole pane (355,656 px). Generic fixes: a cursor key asks the registry nothing (states are
    evaluated at open, after an operation, on a tab switch that ran one, and every 2 s; a state changed outside the menu
    shows up to 2 s late), lookups compare a hash first, rows change in place, the header's network addresses are read
    once a second. Measured: cursor move 93 -> 2 state calls, 66 -> 29 us; long F5 run 15,152 -> 4,415 px per move,
    109 -> 51 us; 5 cores x 4 key sequences, 5,490 pictures, 0 differ. Left, the same on every core: a tab switch
    rebuilds the pane (about 400k px); the console line per key blocks about 10 ms at 115,200 baud. Ours: state
    functions must stay cheap (the 2 s tick runs all of them); `state_mp3<N>` makes about 12 uncached DDR2 reads,
    `state_reu_on` and `state_port_taken` read hardware.
  - **Round 2 started** (`74a95323`): MultiCore's 3.3 contract (`t239.3-boundary` `d6afb393`: headers and a handover table
    per library mapping all 106 pending calls) is copied to `software/trx64u/contract/`; `include/base33.h` gives one
    accessor per new `LibApi` group, undefined until an SDK carries 3.3. The engine starts in an operation `ring.init`
    called from our core hook `start`; `menu_closed` calls `monitor.release` and `ring.unfreeze`.
  - **Monitor on the 3.3 names:** `pending_api.h` is gone; the link is open only on `b33_sync`, `b33_sys`, `b33_ctx`.
    The F4 screen is the export "textgrid" (the menu node gets `grid = "textgrid"`); `monitor.release` is an operation;
    `plat_media` reads `ctx.media_source` for the slots "Cartridge" and "Drive A" (MultiCore's `MountSlots`, their
    c64.cc:619; no drive B) and cuts the spec to the path. S13: no permanent heap, peak 50,240 B (one RPC client while
    gated); image 204,040 B, image + bss 675,504 B (64.4 % of 1 MiB).
  - **Ring on the 3.3 names:** all 47 rows mapped, `pending_api.h` gone; the link is open only on six `b33_*` accessors.
    Machine registers through `io` with our offsets in one header (`ring/c64u_regs.h`, the future API TOML's content);
    cartridge RAM and drive A's image through c64cart / cbm1541. One path builds the machine info (`ringmachine.cc`) for
    every dump, save, load and the exported table: `ctx.media_source` for "Cartridge" / "Drive A", c64cart, `cbm1541.
    drive_info`, System Mode from the video format bits. The engine starts in the new operation `ring.init` (the name
    `ring.start` stays the log's start, as in MultiCore's golden list). The dump job keeps its 76,800-byte stack (the
    contract has no rule; MultiCore's job start enforces only a minimum). S13: image 117,864 B, image + bss 136,520 B
    (13 % of 1 MiB); permanent heap 1,486,893 B (the timeline 1,146,880 B), peak during a 64-entry `.c64rering` dump
    4,345,344 B on top. Found: loading a format 1 `.c64rering` holds the file and its unpacked JSON at once (firmware
    design), about 12 MB for a 30 s ring, more than trx64u's heap of 0x950000.
  - **Base library on the 3.3 names (round 2 done):** all 59 rows mapped, `pending_api.h` gone; the link is open only on
    `b33_board`, `b33_ctx`, `b33_io`, `b33_settings`, `b33_sys`. The library exports "corehooks": `start` starts the
    command interface (uci) and calls `ring.init`; `menu_closed` calls `monitor.release`, then `ring.unfreeze`; init,
    release and the media hooks stay null until c64.cc moves here. Nine base-owned settings left `menu.toml` (Joystick
    Input never was in it); the Monitor tab names `grid = "textgrid"`. State functions: the REU MP3 states share one
    look per pass (2 reads, kept 100 ms, dropped by our REU writes), `reu.on` and `c64.port_taken` read one register.
    Settings are read through the settings group with each key's index kept. S13: no permanent heap, peak 66,044 B;
    image 75,444 B, image + bss 129,296 B (12.3 % of 1 MiB). Open: the key table rows (KS_MONITOR, KS_TIMELINE, D7)
    have no TOML format yet; `u2a_pack.py` refuses `--menu` for a core's menu, which our Makefile sets; the base library
    still reads `api->trx` / `api->c64` and must export "trx" and "c64" itself (D20; one export per library in 3.1);
    the U64 block, mixers, palettes and resampler registers have no `io` member and are still written directly.
  - **Round 2's open points (MultiCore, by Alex's delegation; contract `0aff1a3b`):**
    - A ring dump is a stepped operation (the UI node attribute `job`, T228 R21, as "Save frame"): the menu calls it
      again while it answers `more`, each step holds the core lock, the menu stays open and the machine frozen; no
      `api->job` and no 76,800-byte stack for it (`api->job` is for work that never touches the machine). Not a job with
      io calls: `ram_read` / `ram_memonly` depend on a mode other lock holders change, and the base may release the stop
      at menu close.
    - The key table (KS_MONITOR, KS_TIMELINE) is `[[key]]` rows in the UI TOML (layout / node, key, op, pass_key,
      directive, direct; MultiCore's handover, "The key table of the UI TOML").
    - A core's menu is its description, record 6 of the `.cor` (`cor_pack.py pack --desc` with `desc.toml`), not a
      library's menu: our libraries drop `LIB_MENU`; `menu.toml` and `desc.toml` are delivered with them and MultiCore's
      pipeline packs `trx64u.cor`.
    - "trx", "c64" and "corehooks" become named exports of our base library with base 3.2 (SDK 0.2.0, `c64_api.h`).
    - Our register blocks outside the core window (U64 block, mixers, palettes, resampler) are windows declared in our
      API TOML (name, address, size); `io.window(name, &size)` hands out the base address after checking it against the
      core's address map; offsets stay in our constants header. No absolute addresses remain.
    - CoreHooks gets `base_setting_changed(key)` (after the base applies one of its own settings, and once after start;
      lock task): ours sets C64_PADDLE_SWAP from "Joystick Input" bit 0; the base sets the keyboard part.
  - **Base round 3:** `menu.toml` has the key table (14 `[[key]]` rows: the monitor's `terminal` / `any`, 13 Timewarp
    keys on `timeline`, taken from MultiCore's `menukeytab.cc`); the rows need four fields beyond the handover's list
    (`value`, `status`, `status_ok`, `close`) and three key names ("pgup", "pgdn", "runstop"), and descrec does not
    accept `[[key]]` yet (`unknown key 'key'`), so the menu cannot be packed until MultiCore's descrec takes it. The
    library image carries no menu (`LIB_MENU` gone). No absolute address is left: our blocks are `io.window` names
    (`speaker_enable`, `u64`, `audio_mixer`, `speaker_mixer`, `resampler`, `c64_palette`, `hdmi_palette`), the
    resampler's 32-bit coefficients four bytes inside `sync.enter` / `leave`; the API TOML that declares them has no
    format yet (T213.1). `base_setting_changed` sets the paddle swap from "Joystick Input". `u64cfg_palette` now
    ignores a colour index of 16 or more (it overflowed `g_palette`). Image + bss 131,712 B.
  - **Base round 3's points (MultiCore, by Alex's delegation; contract `e3bfcf2d`):** descrec takes `[[key]]` and
    `[[window]]` with base 3.3, so our menu packs only after 3.3. Key rows: `value`, `status`, `status_ok`, `close`
    confirmed, plus `do = "op" | "decline" | "swallow"` and `when = "<state>"` ("!" negates); the monitor's declines and
    Timewarp's swallow (`codes = [32, 126]`) move to our table too, generic code keeps no KS_MONITOR / KS_TIMELINE row.
    `[[window]]` (name, address, size) is compiled into the description record; the base refuses a window over a
    register it drives (power, RESTORE, HDMI PLL reset, keyboard scan, I2C, MDIO, USB hub reset, case LED select), so
    our u64 block is declared as the runs we write; the case LED select is set through `board.case_led_select(v)`; both
    palettes are ours. `bus_wr` / `bus_rd` take the whole bus-side I/O block (0x00-0x2E). After start,
    `base_setting_changed` runs once per base-owned setting with its own key: the swap is written on "Joystick Input".
  - **Ring round 3, stepped dumps:** `tw.dump` (one 16 KB chunk per step), `tw.ringdump` (one entry materialized, then
    one written per step) and `machine.save_c64re` (63 KB hashed or written per step) answer `more` until done; the first
    call builds the state and the machine info once; cancel, a failed step or a menu close removes the partial file. A
    call the menu does not step (REST, the exported table) runs every step inside one call. `api->job` and its stack are
    gone. Nine files (frame, ring and machine dumps x none / cartridge / GMod2) are byte-identical old against new in a
    host harness; the split materialize is read, not run. Image + bss 135,576 B; peak on top of the permanent heap:
    4,349,536 B for a 64-entry ring. Open: "Save .C64RE dump" is a `[[task]]`, which takes no `job` (it runs all steps in
    one call, no progress); the F4 terminal reaches the ring table from `monitor.key`, which runs without the core lock.
    More firmware bugs (D6, frozen copy; fixed in the library): `Gz` writes through a null pointer when its 16 KB buffer
    allocation fails; a failed `.c64rering` write leaves the partial file.
  - **F4 ring commands and the core lock (MultiCore, by Alex's delegation):** the terminal's key handler only queues
    dump / ringdump / ringsave / undump / ringload; the when_frozen poll `monitor.serve` (lock held) runs them one step
    per pass through the ring's stepped operations, so the lock is held one step at a time and the terminal keeps
    answering and shows progress; RPC and F4 share the path. Not without `KF_DIRECT` (typing must stay immediate), and the
    base hands no core lock to libraries (deadlocks from the menu task). "Save .C64RE dump" stays a `[[task]]` in Home:
    `job = true` on a task comes with descrec 3.3.
  - **Base round 3 finished:** 19 key rows in `menu.toml` (14 op, 4 decline, 1 swallow; first match wins, in
    `menukeytab.cc`'s order). The windows are `[[window]]` tables in a new `base/desc.toml`; the u64 block is three runs
    (`u64_userport` 0xA0000405, `u64_hdmi_connectors` 0xA0000408-09, `u64_pwm_duty` 0xA000040D), the case LED select
    goes through `board.case_led_select`, the paddle swap is written only on "Joystick Input". descrec 0.1.0 refuses both
    files until 3.3, as decided.
  - **Windows confirmed (MultiCore, by Alex's delegation):** `menu.toml` (UI TOML: menu, keys) and `desc.toml` (API
    TOML: the core's data, `[[window]]`) stay two files (D7); with 3.3 `cor_pack.py pack` takes `--desc` more than once
    and compiles both into record 6, refusing a table or key in both. The three u64 runs and `speaker_enable` are
    grantable (only u64cfg writes them); `speaker_enable` reads back as the board revision, and our code only writes it.
  - **F4 ring commands in the poll (waits for Alex):** the terminal key only queues dump / snapshot / ringdump /
    ringsave / undump / ringload / loadsnapshot; the poll `monitor.serve` runs the line under the lock, a dump one step
    per pass (progress in the command row, RUN/STOP cancels and removes the partial file, closing the menu drops it),
    loads and `ringsave` in one pass; RPC waits while a terminal job runs. The ring's export carries a second table,
    `RingSteps` (`ring/ring_steps.h`: size, begin, step, pct, bytes, entries, drop), behind `u2p_ring_api_t` at the offset
    of its `size`; the firmware layout and `app_api.h` are unchanged. trxmon registers the step table at run time
    (`ring_set_steps`); the firmware registers none and keeps today's path. Fixed on the way: `mon_tick`'s 250 ms
    redraw check compared after `mon_sig()` had overwritten the hash, so status rows never refreshed between keys. The
    trxmon part is a firmware change: it stays uncommitted in the submodule, saved as
    `software/trx64u/pending/trxmon-ring-steps.patch`, until Alex has read it. The firmware builds with it (about 3 KB
    more, 177,720 B RAM above the image; `app_space.py` not run), trxmon's host tests pass (1131 / 74 / 29), a scratch
    harness compares stepped against whole-call results (100 checks). Board test for Alex: in F4 `dump /usb0/t.c64re`
    and `ringdump /usb0/t.c64rering 4 5`, watch the progress row, RUN/STOP mid-dump removes the file, the same frame
    over RPC (`monitor/exec dump`) is byte-identical. (The open info row and verbs: see the next entry but one.)
  - **More of F4 through the queue, and RingSteps (MultiCore, by Alex's delegation):** the verbs rstep, frame, goto,
    mark and the trace status go through the same queue and run in `monitor.serve` one pass later; the info row draws
    from a snapshot the poll refreshes (at most 5 ms old). One rule: register calls only under the lock, no exception
    list. RingSteps is its own named export "ringsteps" 1 (`LIB_EXPORTS = ring=1 ringsteps=1`); "ring" stays exactly
    the firmware's `u2p_ring_api_t`; a second table inside one export is not an append.
  - **SDK 0.2.0 (base 3.2):** pinned to MultiCore's release `1bb8532d…` (commit `3be66169`), kept read-only in their
    `.ci/sdk/` (a later branch build had been written over the first path; same headers). Named exports: base
    `corehooks=1`, ring `ring=1 ringsteps=1` ("ring" exactly the firmware's `u2p_ring_api_t`), monitor `textgrid=1`;
    every library needs `base>=3.2`; `find` asks for major 1 (3.2's `min_version` is `U2A3_VERSION(major, minor)`, our
    `find(name, 1, ...)` asked for major 0). Budgets from S13: base keep 0 / peak 66,044, ring keep 1,486,893 / peak
    5,836,429, monitor keep 0 / peak 50,240, image figures measured from the packs (exact: re-measure when 3.3 fills the
    accessors). Our hidden.h workaround is gone (lib.mk includes it for C++ now). The constructor check passes; all three
    pack with a stub of the 3.3 accessors (.u2a: base 80,976 B, ring 120,732 B, monitor 209,540 B). Open: a ring dump
    or load run from the monitor (F4, RPC) is charged to the monitor's budget, since 3.2 charges a block to the library
    whose operation runs on the task. MultiCore decided (advisor): from 3.3 what a library's code allocates goes on that
    library's account, also when another library calls it through an export (under 3.2 a ring block kept from a monitor
    call would be freed under the ring when the monitor unloads); our declared figures already fit. 3.3 also matches an
    export on its exact major and logs a find at major 0.
  - **c64.cc comes to us:** the trx64u descriptor's data is in `base/desc.toml` in MultiCore's record-6 data format
    (`[core]` TRX2, `[video]`, `[freeze]`, `[hotkeys]`, `[mount] names = ["Drive A", "Cartridge"]`, `[[report_reg]]`,
    `[mouse]`, four `[[rom]]`; taken from their c64.cc, which served only trx64u), next to the windows. Its hooks are in
    our corehooks (`base/c64core.cc`): init (power-on keys, mode reset, the drive reset through `drive.reset` when "Resets
    when C64 resets" is on, SID cancel, player end, the job and program cleared), release, media_flush / media_pending
    (cbm1541's). All 19 of c64.cc's operations and 3 states are ours (ops 66, states 54), so the ROM puts, the
    associated config load and the SID service are internal calls; c64_go.cc is ported whole (`base/c64_go.*`, on the io
    group), with t64_load and the parts of MultiCore's shared program loader cbm_prg that c64.cc used (a copy: open with
    MultiCore whether they export it). "Run with App" borrows its buffer instead of a static 64 KB. Image + bss 212,500 B
    (a 65,564-byte static program buffer, as in c64.cc). descrec: SDK 0.2.0 does not know the data sections (t239.3's
    does); the file without `[[window]]` compiles there, merged with `menu.toml` without `[[key]]` 65,638 B. Open with
    MultiCore: CoreHooks has no mount-slot functions (their descrec refuses `[mount]` without them, and
    `ctx.media_source` depends on them); no `[core]` key names the library that provides "corehooks"; cbm1541 has no
    drive reset; `menuvlist.cc` names our T64 operations in generic code (D24); the init hook needs io and settings at the
    first boot reset or the power-on keys are lost.
  - **c64.cc's open points (MultiCore, by Alex's delegation; contract `df3f2d69`):** CoreHooks appends `slot_spec`,
    `slot_mount`, `slot_eject` (slot = position in `[mount]`, MountSlots' meaning; lock task); svc_ctx gets the bit
    `CTX_MEDIA_RESTORING` and `ctx.mount_notify(slot)` for a slot our engine changes itself. `[core] hooks = "<library>"`
    names the corehooks library (cor_pack refuses a bundle without it; descrec accepts `[mount]` then). cbm1541 appends
    `reset(msg, n)` (drive A if there is one, never creates one). MultiCore exports its program loader as "cbmprg"
    (`cbmprg_api.h`; five cores use it): our copy goes once it is built. `menuvlist.cc`'s T64 names stay until the cut
    (P4: a container row in the UI TOML). The first boot reset comes after the libraries and groups are loaded.
  - **F4: nothing reaches the machine from the menu task (waits for Alex with the same patch):** trxmon gets
    `ui_set_polled()`; the library sets it, the firmware does not and keeps today's path. In polled mode every terminal
    line (all verbs) is only queued; `ui_poll()` in `monitor.serve` (lock held) runs it, starts a ring file job in the
    same pass, and refreshes a snapshot of the screen's rows (status, registers, the wide info row with ring and trace
    status); render draws only from the snapshot; tick reports what the poll changed. The session start and `diag`'s
    release run in the poll. `monitor.key` waits up to 100 ms for the poll so quick verbs (and `x` / `/quit`, which
    close the menu through the key's answer) never show a queued state; a line still waiting after that shows
    "starting", RUN/STOP cancels it. Only `monitor.release` (the `menu_closed` hook, by the contract) still touches the
    debug block from the menu task. Checked: trxmon host tests 1131 / 74 / 29, screen goldens identical; a scratch
    harness compares queued against immediate for 22 verbs, narrow and wide, plus the three ring files (510 checks);
    the firmware builds with the final sources (exit 0); the monitor library links with a stub of the 3.3 accessors,
    passes D19 and packs (image + bss 682,500 B, .u2a 211,488 B). On base 3.2 (no when_frozen poll) the F4 session
    never starts while the menu is open. `software/trx64u/pending/trxmon-ring-steps.patch` holds both trxmon changes.
  - **Mount-slot hooks, cbmprg, drive reset:** our corehooks implement `slot_spec` / `slot_mount` / `slot_eject`
    (slot 0 "Drive A" through cbm1541, slot 1 "Cartridge" through c64cart run / remove plus forgetting the program,
    as c64.cc's MountSlots did); `desc.toml` names
    `[core] hooks = "trx64u"`; the init hook resets drive A with `cbm1541.reset`. The copy of MultiCore's program loader
    is gone: placing, typing and relinking go through their export "cbmprg"; until a bundle carries it, Run PRG, Load
    PRG, DMA, T64, Run with App and Mount & Run refuse with a reason. Image + bss 213,004 B.
  - **Who tells the base of a slot change (MultiCore, by Alex's delegation; contract `f5fc7128`):** the shared
    libraries, for every change they make to their own slot (cbm1541 "Drive A", c64cart "Cartridge", `for_state` and
    `run_image` included). `ctx.mount_notify` takes the slot's name (a shared library cannot know a core's `[mount]`
    order), from any task (it only marks the slot; the base reads `slot_spec` on its next pass). Both trx64u slots are
    backed by shared libraries, so our code calls it nowhere.
  - **c64cart `require` / `prohibit`:** MultiCore's base keeps no cartridge definition, so both answer 0 ("not known by
    the base", 3.3 contract). Checked: neither the library's ring nor the firmware's reads them on a load (they are
    parsed into the load's want record and never used), so 0 stays and no table by CRT type is built. A `.c64re` the
    library writes then carries 0 where the firmware writes the definition's flags. The comments that claimed a load
    rebuilds the cartridge definition from them are corrected in the library; the firmware's frozen copy keeps the same
    wrong comment (`trx_c64re.cc:1066`, `:1456`).
  - **Base 3.3 (dev SDK 0.3.0, MultiCore `t239.3-33` `79dbc77d`, not released):** serves the groups, net and job for
    libraries, export calls on the provider's account, when_frozen polls, the complete CoreHooks, the shared exports,
    `[[key]]` / `[[window]]` / `[[task]] job`, `--desc` twice. Our switch is running.
  - **How the first run goes (MultiCore, by Alex's delegation):** MultiCore's coreapp tests and the menu simulator are
    native Mac builds and cannot run a RISC-V `.u2a`. The menu simulator takes our UI TOML (`tools/menusim/run.sh c64
    --toml <menu.toml> --ops <op names> --keys ... --stats`) with our states and ops as stubs: menu pixels and figures per
    key against the built-in trx64u. Our code first runs in ue2emu (the emulator of the C64U_emu session) on the shared
    3.3 `coreapp.app` with our `trx64u.cor` in a stick folder of our own (`--usb-dir`); whether the shared image starts
    the trx64u machine at all is the first thing it shows. Then the board.
  - **On base 3.3 (dev SDK 0.3.0, `79dbc77d`):** `contract/` and `include/base33.h` are gone; the code reads the groups
    from its LibApi (65 accessors replaced; members appended to SvcSys checked by size); every library needs
    `base>=3.3`; all three links close (nothing undefined), pass the constructor check and pack (`.u2a`: base 95,624 B,
    ring 121,176 B, monitor 211,192 B; image + bss 212,924 / 136,264 / 682,204 B). `build.sh` checks the unpacked SDK
    against the pin. Record 6 compiles from `menu.toml` + `desc.toml` (66,704 B; `hide = ["Color Scheme"]` added as in
    MultiCore's built-in menu), but descrec needs MultiCore's `basereg.def`, which the SDK does not ship. The window
    check refused `speaker_mixer` (it covered the base's master volume and tone at 0x550/0x551); our bitstream decodes
    only +0x40/41/44/45 there (`ultimate_sys.vhd` `spk_gain`), so the window is 6 bytes and only those four bytes are
    written. A whole `trx64u.cor` needs the bitstream and a trx64u application image the SDK does not carry.
  - **Joystick Input, basereg.def, the shared image (MultiCore, by Alex's delegation):** trx64u does not name the
    capability `kbd_joystick`: it means keyboard joystick lines and a port choice for play in the gateware, and c64_basic
    has neither (its KEYB_JOY port select only feeds the menu's own scan); Alex's T228 R20 already took the row and the
    J hotkey out of trx64u's menu. So `swap_key = "Joystick Input"` and our paddle swap go. c64_basic decodes no paddle
    swap at all (no 0x1A in `ultimate_sys.vhd`'s page-011 write decode, no paddle or pot swap in designs/ or cores/):
    C64_PADDLE_SWAP was only a name in the software, so our writes to it go and nothing is left to decide. The SDK now ships
    `basereg.def` (dev SDK `d6f50a0d`). The shared 3.3 image runs trx64u by the D17 path (no compiled-in descriptor, so
    it builds one from our record 6 and binds our hooks); it has never run. In ue2emu the host's bitstream load is not
    emulated and the core app reads only records 6, 7 and 8 of the `.cor`; the `.cor` is still packed with the real
    bitstream, so the same file later goes to the board.
  - **First run in ue2emu (2026-11-23, dev SDK `d6f50a0d`, shared 3.3 `coreapp.app`, ue2emu 0.7.4 headless):**
    `trx64u.cor` packed with `cor_pack.py pack-large` (the release bitstream of main-d94aa68e, sha256 `c950aa55…` as
    MultiCore's pin; `--app` the SDK's `coreapp.bin`; record 8 the three SID-player programs; 4,173,504 B; `check`
    passes). With the `.cor` on the SD card the base identifies TRX2 TRX64U from our record 6 and loads all three
    libraries ("3 loaded, 0 refused": budgets, needs, exports pass); with a diagnostic record without the 1541 ROM entry
    the C64 reaches READY and the menu opens (F5 our settings, the Monitor screen answers, Timewarp opens). Our code
    showed no fault. Found in MultiCore's base (`d6f50a0d`): M1 the record-described path opens the `.cor` before the USB
    stick has enumerated (no medium wait), so a `.cor` on the stick is never read; M2 a record-described core gets none
    of the base's own operation groups (cbm1541, online, diag), so `drive.put_rom` is missing, the ROM step loops and 27
    menu entries are refused; M3 the init and start hooks run before the libraries get their context and groups
    (`api->io` null, `ops->call` answers "No operations here."), so `ring.init` fails, our windows are not written and
    the SID detection that sets the freeze's stop mode does not run (every menu open: "no PAUSED after 200 ms"); M4 the
    base's own monitor binds TCP 4312 before ours; M5 the shared image's heap cannot take trx64u's 0x950000 (largest that
    fits 0x7cad3c; 3,985,552 B free after our libraries load, while a 64-entry `.c64rering` dump needs 4,349,536 B more
    than the ring's 1,486,893 B). Also: our hotkeys table drops the joystick swap key; nothing writes a paddle swap.
  - **M1-M5 (MultiCore, by Alex's delegation):** all five confirmed in their code; fixes built on t239.3, in test, the
    image changes, the headers and the dev SDK stay. M1 the record path waits for the hand-over medium (USB, at most
    10 s); M2 a record core gets the base's own groups (diag, online always; the 1541 group when the record's caps name
    `drive_a`, which ours must keep); M3 the AppHost and the machine groups are installed before the first reset, so
    init and start get io, ops, c64, trx and net; M4 the base's monitor serves only a core without a hooks library, port
    4312 is ours. M5: our `.c64rering` dump streams entry by entry (materialize, write, free), sized against the largest
    free block; the shared image's heap does not grow; within that, the order fix with the shorter time (a seek-back on
    FAT is cheap, a second pass costs a materialize per entry). The firmware's dump gets the same fix (Alex's, D6).
  - **M5 done in the library: the `.c64rering` dump streams.** The ring stores the head plus undo records, so entries
    come newest first while the file wants them oldest first, and each entry's length depends on its own data (colour RAM
    as decimal numbers). Chosen: a colour-only walk sizes every entry first (about a tenth of the records of a full
    walk), then each entry is materialized newest first and written at its offset (`SvcFs::write` takes an offset; a
    write past the end grows the file on FAT and FTP), the gzip CRC joined from the pieces (`crc32_combine`). One entry
    per step (n + 1 steps instead of 2n + 1). Peak on top of the idle heap for 64 entries: 4,349,536 -> 107,152 B
    (largest block 65,536). 15 of 15 files byte-identical old against new (rings of 1 / 4 / 64 entries x no cartridge
    / cartridge / GMod2, frame and machine dumps) in a host harness, block-edge cases identical, the ring walk run on
    the host over a synthetic log; not run in ue2emu or on the board. Image + bss 138,528 B. Open: a format 1 load
    holds the whole file and its unpacked JSON at once (the next candidate for the same treatment); `heap_peak` keeps
    the earlier ceiling until that is decided. The firmware's dump needs the same change (Alex's, D6).
  - **The format 1 load (MultiCore, by Alex's delegation):** it streams too (gzip and JSON parsed node by node), after
    the ue2emu re-run; the wide `heap_peak` stays until then and drops to keep + 1,444,975 in the same change. api.md
    4.4.12.4 now promises that a write past the end grows the file (the gap undefined until written: FAT leaves old
    cluster data, the network medium zero-fills).
  - **trxmon's change committed (Alex, 2026-11-23: "comitte bitte"):** the polled mode and the stepped ring dumps are
    commit `e67efa83` on branch `u18` of the application submodule (Gitea, not main); the superproject's `u18` points
    the submodule at it, and `software/trx64u/pending/` is gone. The board test (F4 dump / ringdump with progress,
    RUN/STOP removes the file, the same frame over RPC byte-identical) is still open.
  - **Second ue2emu run (2026-11-23, MultiCore's image `7ca3c1b6` with M1-M4 fixed, the real `desc.toml`, the `.cor` on
    the stick):** the base waits for the stick (1.8 s), identifies TRX2 TRX64U, loads our three libraries (no menu entry
    refused), puts the 1541 ROM, runs init and start (SID detect, our monitor serves TCP 4312, the ring logs), BASIC READY.
    Freeze pauses cleanly (menu open 0.7 s after the request, close 51-100 ms); `menu_closed` runs `ring.unfreeze`.
    F1/F3/F5/F7/F8 render; the Monitor screen answers through the poll queue; Timewarp holds frames and scrubs (66 ms a
    step); RPC on 4312 from the host answers (`ping`, `session/read_memory`; `monitor/exec` behind the token gate as
    designed). Save the ring (64 entries) steps with progress: sized 2.9 s, materialized 2.1 s, written 27.2 s (2.9 s of it
    file writes), 6,242,746 B; it parses (gzip CRC, JSON, 64 entries). Save frame (format 2) saves and loads back. Times are
    ue2emu's emulated milliseconds, not the board's: per key F5 cursor update 7-8 ms, render+flush 27-42 ms; F1 5-7 /
    20-24 ms; entering F5 560 / 436 ms. Open: Timewarp shows no picture behind its bar (the base fills the screen with
    the key colour, and c64_basic blanks the C64 picture while the whole machine is paused; the firmware draws the
    captured still instead: MultiCore's base); the `.c64rering` load runs out of memory (the format 1 load holds the file
    and its JSON: the streaming step decided above); our format 1 machine dump loads back as "No cpu node" (to be
    investigated); writing a ring entry costs about 0.4 s of CPU (to be profiled); after an RPC session opens, the
    monitor reports runState "paused" while the C64 runs; trxmon's version reads "unknown" (no `TRXMON_BUILD`).
  - **The Timewarp picture (MultiCore, by Alex's delegation): a core hook.** CoreHooks gets `void (*menu_video)(int
    behind)` appended after `slot_eject` (major 1, append only; a new dev SDK). The base calls it on every change of the
    live key: 1 when a layout shows the machine's picture through the key colour (the timeline), 0 when the menu covers it
    again; on the main loop without the core lock, like `menu_closed` (shared state through `api->sync`); without the
    member the base does nothing. Our library chooses what shows behind: the live picture, or the ring's still at the scrub
    position, which the ring updates itself after each scrub step (the hardware path the firmware uses, no frame drawn in
    software: the still and the video path are our bitstream's, D20). Not chosen: the base drawing the still (a frame drawn
    in software per step, slow on this board), the gateware not blanking while frozen (the live frozen picture, not the
    scrubbed frame). Waits for the header and the dev SDK. Also from MultiCore's review: an op a start hook calls while it
    holds the lock now runs inline.
  - **The frozen firmware ring's bugs fixed (Alex, 2026-11-23: fixes in the firmware are fine, it stays stable, no
    features):** branch `ring-fixes` of the application submodule (Gitea), on `eb3e6be6` (0.3.1), commits `f3a3328f`
    and `bca3894f`: the `.trxring` header through `%u` and `u64_dec()` (small_printf has no length modifier, so 0.3.1
    refuses its own `.trxring` files; the header is now byte-identical to the library's, checked on the host); the
    format 1 stage zeroed (`want`, `eeprom`); `Gz` without its block fails cleanly; a failed `.c64rering` write removes
    the file; a `.c64re` load refused while the REU is on (A08); the require / prohibit comments; and `%.*s` (small_printf
    has no precision either: two refusals sprintf'd a pointer made from the length into fixed buffers) through bounded
    copies. The firmware builds without new warnings; its host tests pass (they do not cover these functions). Not
    done here: the `.c64rering` dump's 4 MB peak (a rework). Board-tested by Alex 2026-11-23 (superproject branch
    `fw-ring-fixes`, pipeline run 470, softload): all five pass. Open: which release takes it (Alex's).
  - **Tests and delivery, MultiCore's answers (T239.3 D21):** MultiCore's trx64u units leave their pipeline only after our
    stages run green (Alex: "Yes, like that"). Their pipeline checks nothing of our `.cor`: S8's first half (the `.cor`
    carries our libraries, the menu record and its tests) is ours; the second half (the shared image has no trx64u
    symbol) stays theirs, built with the cut. `gen_menu_rec.py` has no role for a library core (record 6 comes from
    `descrec.py`; `menu_rec.inc` leaves with the cut). MultiCore sorts the 18 test sources naming trx64u (move / stay and
    rebase / stay / split); what ships with the SDK for them (R9) goes to Alex after that sort. `menu_video`: base side
    in code (t239.3 `01595fea`, not pushed; api.md 9.7.13): called once per change of the layer's key colour, never twice
    with the same value, the first call an on; on for the timeline and for the PAUSED tag with the menu closed.
  - **The format 1 load streams; the cpu node; the write time (2026-11-23, ring library):** `trx_inflate` is a streaming
    inflater (16 KB input, a 32 KB window only for Huffman blocks); pass 1 reads the file once (SHA-256, gzip CRC and
    ISIZE, a JSON scanner that picks the entry by `ring_pick`'s rules), pass 2 re-reads only the chosen checkpoint and
    decodes `ram`, `cartFlash` and `cartRam` straight into the staged buffers. Peak for a 64-entry ring 12,483,561 ->
    107,356 B (largest block 65,536); 168 cases old against new identical in a host harness. `heap_peak` drops to keep +
    1,444,975 (the `.trxring` save) = 2,931,868, as decided; image + bss 148,224 B. A format 1 machine dump has no cpu
    node by design (the 6510's registers cannot be read while paused): the library now loads it coarsely (CPU and CIAs
    stay as the running machine has them), as TRX64's own restore does; frames and rings stay exact. The firmware refuses
    such a file ("No cpu node", `trx_c64re_nodes.cc:281-284`). Write time: a CRC-32 table, inline base64, divisions by
    10 as multiplies; per ring entry 380 -> 110 ms CPU in ue2emu, a 64-entry dump 32.2 -> 11.3 s, the files
    byte-identical. In ue2emu a 6.2 MB ring loads back (pass 1 11.2 s, pass 2 0.3 s), the machine dump and a frame
    load back. Open: pass 1's time; the CRC table's cost on the board's DDR2 is not measured.
  - **menu_video on dev SDK `e4f99648` (2026-11-23):** pinned (headers hash `ae2f02b41643…` as the tarball's VERSION
    says), the hook appended. In ue2emu the base calls it as promised: Timewarp open one 1, scrub steps none, close one 0,
    the PAUSED tag 1 and 0; no pair. **The picture is still black, and the decision's premise was wrong:** the firmware
    does not switch a hardware still in; it copies the captured still (`STILL_STATUS`, the still slot) and draws it as
    RGB565 into the pixel layer in software after every scrub step (`trx_video.cc` `grab_still`, `draw_still`,
    `trx_video_tw_bar`). c64_basic has no surface under the base's layer: `pix_layer.v` is the base's menu layer,
    `vic_scaler` shows black while the whole machine is halted (`top.v` `.blank(c64_halted)`). So the hook only logs
    until it is decided again where the still is drawn. Found on the way and fixed: a scrub drops the oldest entries
    once the ring is full, which shifted every index while Timewarp kept the shown frame by index (LEFT stayed, RIGHT
    jumped 2, "Resume here" resumed one frame late); `go_to` now finds the frame again by its seq. Image + bss: base
    212,860 B, ring 148,360 B.
  - **Where the Timewarp still is drawn (MultiCore, by Alex's delegation): our library, through a pull hook.**
    Superseded on 2026-11-23 by MultiCore's overlay service (T256, the entry after the next); never built. CoreHooks gets `int (*menu_still)(const CoreStillGeom *g, CoreStill *out)` after `menu_video` (major 1, append only).
    The base calls it while `menu_video` is on, when the timeline opens and after every scrub step, on the main loop
    without the core lock; it hands the canvas (960 x 540) and where it lands on the HDMI frame, we answer with an RGB565
    rectangle in our memory, which the base places under the bar as an LVGL image. We draw it as the firmware's
    `draw_still` does: canvas pixel -> frame pixel -> the scaler window (our HDMI table, from our own row and scale
    settings) -> VIC pixel. The buffer (at most 1,036,800 B, about 780 KB at 1080p Fit, plus the ~52 KB still copy) lives
    on our heap from `menu_video(1)` to `menu_video(0)` and fits the ring's declared peak; no block, no picture (answer
    0). The ring copies the still from DDR2 during the scrub op under the lock; `menu_still` only converts that copy.
    Not chosen: the base drawing (our format), a gateware switch (a feature, Alex's), black. Waits for MultiCore's header.
  - **Alex, 2026-11-23 (translated): an overlay over the running picture belongs in the base.** "That's ok. MultiCore
    must allow an app to put an overlay over the running picture. It doesn't do that itself - I understand that, because
    I never wanted it, to keep things simpler. But basically this has to go into the base and so into the SDK, or into a
    library that is then used wherever it is needed." So `menu_still` is not to stay a Timewarp-only hook: it becomes one
    user of a general overlay facility (a library or app puts pixels over the machine's picture, also while it runs), in
    the base and the SDK or in a shared library. Its shape (service or library, push or pull, who owns the layer with the
    menu closed, the still and the PAUSED tag as users) is MultiCore's to design, as advisor.
  - **The overlay is MultiCore's T256, a base service in the SDK (Alex D1-D7, 2026-11-23); what it means for us (a draft
    discussed with MultiCore, on hold, see the next entry).**
    - Alex's decisions there:
      - D1: no new gateware.
      - D2: only the base drives the pixel layer.
      - D3: a library paints RGB565 surfaces, key-colour pixels are transparent.
      - D4: elements only while the menu is closed; the still also lies under the key-coloured menu pages.
      - D5: the base's tag says only "PAUSED".
      - D6/D7: a library can hide and show the menu by a call.
    - **The Timewarp still needs no surface of ours.** After every scrub step the host's still capture already holds the
      frame Timewarp shows: the step runs one frame and waits until the `STILL_STATUS` count moves (`ring/ring.cc`). The
      base re-reads the capture after every op that returns while a key-coloured page is open, and `still(NULL)` asks for
      it. No still buffer and no HDMI table code in the library; the bar stays the base's (`build_timeline`).
    - **The mapping is ours, as data.** The base must not interpret c64_basic's mode registers, so CoreHooks gets
      `int (*still_map)(int row, int scale, OvlStillMap *m)` after `menu_video`. The base calls it before each conversion
      of a capture, on the main loop without the core lock, with the row as 0x4F07 reads and the scale it wrote (raw
      values). We fill `xw0, yw0, hn, hd, vn, vd` and the row's window from `trx_hdmi_geom` / `trx_hdmi_plan`. We answer 0
      for a pair our table does not have, and then there is no still. The lines of a capture come from its own
      `STILL_STATUS` b2 (270 or 240), as the firmware clamps them during a System Mode switch.
    - **PAUSED:** the library that paused (`op_pause_toggle`) shows how to go on with a small key-coloured element of its
      own, shown at pause and hidden at resume. The base's still lies behind it.
    - One buffer per surface: `begin` waits at most one render and never fails, and the base copies the surface into its
      canvas, never scans our memory out.
  - **trx64u does not wait for T256 (Alex via MultiCore, 2026-11-23; MultiCore T239.3 D24, D25: "not everything at
    once").** trx64u and the VIC-20 are the SDK's test cores; T256 follows them as its own step. trx64u is built and
    tested on `t239.3` as it is (base 3.3, dev SDK `e4f99648`). Until T256: Timewarp is the base's timeline page with the
    bar over black, no still; Pause shows the base's PAUSED tag; our pause hint waits. The `ovl` group, `begin`/`end`
    and `still_map` are a draft that will change, and MultiCore's acceptance of `still_map` is on hold: nothing is built
    against them. D25's changes on `t239.3` (other cores' TOML copies, the Amiga's mouse rows) do not touch trx64u.
  - **Two small monitor faults of the second run fixed (2026-11-23).**
    - Before the run control was taken (`mach_own`), trxmon reported runState from `M.running`, which `mach_init` zeroes,
      so "paused" while the C64 ran (the firmware's service the same). `rpc.c` `run_state()` now asks the hardware
      (`ST_SHUT`) until then; trxmon host test 1133 checks, 0 failed.
    - The version said "unknown": `monitor/Makefile` now passes `TRXMON_BUILD` "in-trx64u", as the firmware's makefile
      passes "in-firmware".
    - Monitor image + bss 209,004 + 473,264 B.
  - **First board run of `trx64u.cor` on base 3.3 (Alex, 2026-11-23): the menu is far too slow.** Open from the C64
    5-8 s, category change 3-5 s, tab change 3-5 s; the cursor in a table as fast as in the other cores. Cause: the
    `c64_basic` RISC-V had no data cache (MultiCore's host has one since its T133); the menu's work is the base's LVGL,
    the same as the Amiga's (ue2emu PC sampling: our libraries had no measurable share). Fixed in the bitstream:
    [U19](U19-app-riscv-caches.md), branch `rv-dcache` `54dc9f9e`. `trx64u_u19.cor` (same libraries as `4f3ba9f5`,
    that bitstream) is packed for the board test.
