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
    against them. D25 on `t239.3` (`4c4d244f`): other cores' TOML copies and the Amiga's own mouse rows; for trx64u only its exported
    operations change: the base registers `mouse.sens` and `mouse.accel` for every core again (MultiCore's golden
    `ops_trx64u.txt` lists them after `led.load`). Nothing in our libraries or our menu changes.
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
  - **Board run of `trx64u_u19.cor` (Alex, 2026-11-23): the menu is acceptable, the U19 bitstream stays.** Alex: the
    transition is good for now once `trx64u.cor` is built this way and the Gideon-based app exists as `.cfw`/`.ue2` and
    `trx64g.cor`; polishing comes step by step after that.
  - **trx64u goes into our CI (Alex, 2026-11-23: *"unser CI anpassen ... und dann bitte auch mitschreiben, wie man einen
    .cor baut"*).** Step 1: `software/trx64u/pack_cor.sh` builds the libraries and the SID player's files (from the
    submodule's 6502 sources), packs `trx64u.cor` and checks it; the icon is in `base/icon.rgb565`; the README's "Packing
    trx64u.cor" is the guide. From the inputs of `trx64u_u19.cor` it makes that file byte for byte. Next: a `cor` stage
    after `package` (as `update`), then lint, apptests and emu. Asked of MultiCore (advisor) the same day: where CI fetches
    the SDK (R10, new: their builds are only in their repository's `.ci/sdk/`), the golden ops list (R6), the heap
    (their pin packs 0x950000, ours the image default 0x7A0000), the test rig (R9) and the icon.
  - **Step 2 (2026-11-23): the SDK from our registry, our golden ops list, the `cor` stage.** MultiCore (advisor): the SDK
    comes from `t239.3` and we keep a copy (their D26); a copy of the pinned tarball is TREX generic `trx64-sdk/0.3.0-e4f99648`
    (`fetch_sdk.sh --download`). The golden ops list is ours (`base/ops_trx64u.txt`, from `e4f99648`; R6), `pack_cor.sh`
    checks record 6 against it. The heap (D15, MultiCore corrected itself twice the same day): sized by trx64u's measured
    need, kept in our core TOML (`heap`, as MultiCore's `core.toml`) and passed on by `pack_cor.sh` as `--heap`; **not measured yet**, so the image's 0x7A0000 applies, as on the board. SDK 0.3.0's
    `coreapp.bin` allows at most 0x7B7878 because it is MultiCore's whole application (other cores' code, ~1.7 MB of their
    buffers, the old `tm_mon`/`tm_rpc`): a defect of the SDK, a base image without core code is announced. `main` (U19) merged into `u18` so that its `.cor` gets the cache bitstream. Stage
    `cor` (T31 decision 16): `ci/cor.sh` packs `trx64u_<commit8>.cor` into every package of a branch with `software/trx64u`.
    Next: lint, apptests and emu from the rig of the pinned `t239.3` commit (MultiCore: `tests/coreapp`, `gen_menu_rec.py`,
    `check_menu_concept.py` with `menu_c64.txt`, `tools/menusim`, `ci/emu/build.sh`); sorting out which of the 18 apptest
    sources are trx64u's is ours.
  - **Stage `cor` green on its first run (u18 `4341ce8e`, 2026-11-23):** every stage of the pipeline green;
    `trx64u_4341ce8e.cor` in package `u18-4341ce8e` (sha256 `374f781e...`, in its `SHA256SUMS`). Its bitstream (`eb8aeaa2`)
    and libraries (`e4e19c2e`) are those of `trx64u_u19.cor`, which Alex accepted on the board; only version and commit
    fields differ. Open for the move: the stages lint, apptests and emu (rig from the pinned `t239.3` commit, sorting of the
    18 apptest sources ours), trx64u's peak heap (D15: measure, keep in our core TOML `heap`, `pack_cor.sh` passes it),
    MultiCore's base image without core code (announced).
- **2026-11-24: P1-7 of MultiCore's T239.3 D28 plan: c64_basic's TOML A, our libraries on its driver table** (Alex's go
  2026-11-24; MultiCore advises; the SDK tools used are 0.4.0's, `c5ca4556`, an unpinned copy, `sdk.pin` stays 0.3.0).
  - `software/trx64u/gateware/c64_basic.toml` (record version 7) is written from what the RTL of `designs/c64_basic`
    decodes, each block with the lines that decode it; one block per register range and driver (D34). 33 blocks. Base:
    `ident`, `resid8` (map `sid`), `c64cart` (map `c64cart`), `cart_regs`, `drive_a`, `cmd_if` (map `uci`), `dma` (map
    `dma`), `gcr_coder` (map `gcr_coder`), `c64_video`, `c64_dma_mode`, `c64_joy`, `c64_debug`, `c64_roms`. Core:
    `trx_gate`, `trx_dbg_run`, `trx_turbo`, `trx_statebus`, `trx_engine`, `trx_debug` (with the drive's half), `trx_pixel`,
    `trx_text`, `trx_slot`, `speaker_enable`, `u64_userport`, `u64_connectors`, `u64_pwm`, `c64_scanlines`,
    `c64_turbo_en`, `c64_phase`, `c64_sid_addr`, `c64_sid_sock`, `c64_sid_cfg`, `c64_bus`. No resampler: nothing decodes
    0x10100580. `cart_regs` and `drive_a` carry their properties themselves, because the SDK's maps differ from this RTL:
    `c64cart_regs` has no `stop_mode` and no `clock_detect` and calls rw registers write-only; `cbm1541` has the WD177x
    registers from +0x1800 (no write reaches them here), `side` (reads 0) and `drivetype` (only with `g_multi_mode`).
  - The libraries: `base/` and `ring/` take every address from `drvtab.h` (`HW_RD`/`HW_WR`; the base's `cart_regs` through
    `io.cart_rd`/`cart_wr` and the ROMs through `io.rom_*`, the offsets from the table); both `c64u_regs.h` are deleted.
    `rtl_check.py lint-src base ring monitor --toml-a gateware/c64_basic.toml --maps <SDK>/maps --allow
    trx_sha256.c:0x106aa070u`: no literal I/O address in 48 files. Fixed on the way: the slot status is read as one 32-bit
    word (the byte reads of +6 and +7 had no byte 0 latch) and ROM_WR_EN written as one (a byte write of +0 never
    committed); the speaker enable and the U64 page's bytes now reach their registers (as windows at 0xA0xxxxxx they went
    to the memory path, bit 28 clear); the resampler code and its tables are gone; ring's SH_CTRL accesses are gone
    (trx_sys.v decodes no word 0x1400: no session register in c64_basic).
  - `base/desc.toml`: record 6's data and four windows; `speaker_enable`, `u64_userport`, `u64_hdmi_connectors` and
    `u64_pwm_duty` became TOML A blocks, `resampler` has none; its wrong comments (addresses "the firmware's", the
    registers "in c64u_regs.h") are fixed.
  - rtl_check of SDK 0.4.0 on the whole file: `an RTL pattern I do not know: designs/c64_basic/trx_sys.v:267: a genvar
    construct`. Per block (each with `ident`): 20 blocks, 66 properties agree (`rtl_check.py check --design
    designs/c64_basic --toml-a <those 20> --maps <SDK>/maps`, 21 s); 13 the tool cannot follow: the seven of `trx_sys.v`
    (genvar), `trx_pixel` and `trx_text` (`pix_layer.v:80`), `drive_a` (`c1541_drive.vhd:272` if generate), `cmd_if`
    (`synchronizer_gzw.vhd:48` attribute), `gcr_coder` (`gcr_codec.vhd:63` for generate), `speaker_enable` (`u2p_io`: "a
    record io_resp used as a value"). The V branch's rtl_check (`t239.3-v`) also says the 13 core blocks in the U2P, U64 and
    C64 I/O pages are "not handed over to the core by the RTL".
  - Built: `base/` and `ring/` compile and pack on SDK 0.3.0's headers with `drvtab.h` made by 0.4.0's `descrec.py drivers`
    beforehand (`trx64u` image 86,260 bytes, `ring` 131,404); `monitor` unchanged.
  - Open, not ours to decide: the four windows `audio_mixer`, `speaker_mixer`, `c64_palette`, `hdmi_palette` (Alex: they lie
    over blocks MultiCore's host TOML A gives the base; `hdmi_palette`, translated to 0x10145000, lands in ultimate_sys's
    overlay region, and the C64 palette decodes RGB writes only); `resid8` is a base block by MultiCore's plan (the SID
    library), but `u64cfg.cc` writes it and the SID library has no call for it (0.3.0 and 0.4.0); the monitor's engine
    (trxmon, submodule, D4) still uses `api->trx->regs` and its own offsets; whether the 13 core blocks in the ultimate_sys
    pages may stay core blocks (the V branch's check).
  - Waits for the pin switch to SDK 0.4.0 (with P2-5, P3-3, P4-1): `sdk.pin`, `build.sh` green on the pin (its `descrec.py
    drivers` and `maps/`), `rtl_check` and `lint-src` in our lint stage (0.3.0 has no `rtl_check.py`; `check` green needs
    the 13 blocks followable), the TOML A in the `.cor` (0.4.0 ships no app image), a CI run.
- **2026-11-24: the window `hdmi_palette` stays; the reSID8 write is interim** (step 2a of the D28 plan).
  - Checked on the code, against the claim that c64_basic has no HDMI palette: the base of SDK 0.4.0 translates the
    window's 0xA0045000 to 0x10145000 (`libsvc.cc` `window_to_toml_a`, our TOML A's `u2p_io_base` 0x10100000);
    `ultimate_sys.vhd` decodes 0x140000-0x147FFF as the overlay (:903-904, `ovl_we` :966, `ovl_addr` :971), and
    `overlay.v` writes +0x5000-0x503F into its 16-colour palette (:105-111, R G B at 4n), the colours of the overlay's
    characters (:288). `u64cfg.cc` `put_colour` writes exactly those bytes. So c64_basic has the HDMI palette and the
    window hits it: not removed. `desc.toml`'s sentence "no HDMI palette" was wrong and is fixed. Its owner stays open
    with `audio_mixer`, `speaker_mixer` and `c64_palette` (Alex).
  - reSID8 (Alex's placement: base, the SID library; MultiCore adds a filter call to that library in a later SDK):
    `u64cfg.cc` keeps its direct write, marked interim. Open: the write moves to the SID library's filter call once the
    SDK has it.
- **2026-11-24: ring's cache comments follow U19's data cache** (step 2a of the D28 plan).
  - `ring.cc` said an engine COPY into the bounce buffer is visible to the CPU as soon as the mover is done because the
    rvlite core has no data cache. c64_basic's rvlite has one since U19 (8 KB, 16-byte lines, write-through, cacheable
    below 0xEA0000). Checked on the RTL, not a gap: every mover write below 0xEA0000 toggles `ext_wr_tgl`
    (`ddr2_port.v:213-214`); 3-4 CPU clocks later the cache stops hitting and clears its 512 tags
    (`rv_dcache.vhd:206-210`, `:242-243`, `:344-351`), long before ENG_CMD's busy bit, which crosses from `ui_clk` into
    the C64 clock and then over the I/O bus, reads 0. Corrected: the head of `ring.cc`, `collect()`'s cost note, the
    fence after the COPY poll (rvlite decodes `fence` as a no-op, `decode_comb.vhd:112-113`; it stays as a compiler
    barrier), `ringfile.cc`'s cache note. U19 §3 gave the tag walk 1,024 clocks; the code walks 512 lines.
- **2026-11-24: step 2a of the D28 plan: SDK 0.4.1 pinned, trx64u's app linked from the SDK (P3-3), heap word 0 (P4-1)**
  (Alex's go 2026-11-24; MultiCore advises).
  - Pin: `sdk.pin` names SDK 0.4.1, MultiCore `t239.3` `8ff28ba9` (tarball sha256 `8b5f2b29...`, headers `d7a12ad8...`,
    `base_archive_sha256` `c7ad6b29...`; `coreapp_sha256` is gone with the app image); `build.sh` checks the new key.
    0.4.0 (`b43857a6`, `c5ca4556`) was pinned first; 0.4.1 carries MultiCore's fixes of the two defects below and the
    new `io.window` rule. Registry package `trx64-sdk/0.4.1-8ff28ba9`: not uploaded by this step; until it is there,
    `fetch_sdk.sh --download`, and with it the lint and cor stages, cannot fetch the SDK. `build.sh` is green on the pin
    (images: trx64u 86,260 B, ring 131,404 B, monitor 209,004 B).
  - TOML B `software/trx64u/app/toml_b.toml` (the SDK example's place, `example/app/`): no descriptor (the core is described
    by its record, `hooks = "trx64u"`), `base_sources = ["portdiag.cc"]` (run.cc calls `portdiag_attach`), no `[sizes]`
    key (the headers' defaults, the shared app's), and 14 shared libraries of the package's catalog: c64cart, cbm1541,
    newfile, uci, sid, tempdisk, cbmprg (our libraries find them by name), dma (runs our `dma.*`), t64 (the `.t64` list),
    mouse, pad, padkeys, fastseek, ftp. Not linked: cbmrun, newdisk, folderhd, resample. Our three libraries stay `.u2a`
    in record 7, as the SDK's example keeps its code library; the sub-apps come in step 2b.
  - `pack_cor.sh`: record 6 compiled against the TOML B's sizes (`descrec.compile_files` from Python: `descrec.py compile`
    takes one TOML, several come with MultiCore's package 1), `app_build.py build` with the TOML A and B into `build/app/`,
    the SDK's symbol check (`libtab.py symcheck`), `cor_pack.py pack-large` with the linked app, `--sizes` and `--heap 0`,
    then the heap figure checked against the image. `ci/cor.sh` runs it unchanged.
  - SDK defects the link found on `b43857a6`, fixed by MultiCore in `563f708e` (in 0.4.1):
    `base/inc/coreapp/ffconf/ffconf.h:5` includes `ffconf.h` through MultiCore's symlink
    `host-app/software/chan_fat`, which the package does not have (`tempdisk.o`, `fastseek.o`, `fastseek_exports.o` do
    not compile); `tools/libtab.py symcheck` imports `check_d24`, shipped only in `menusim/tools/`. Without those two
    libraries the app linked as a diagnostic (text 1,197,124, data 258,728, bss 2,513,684 B) and the symbol check
    passed; nothing else failed.
  - Result on 0.4.1: the app links with all 14 libraries (`tempdisk` and `fastseek` compile; 123 objects; text
    1,199,348, data 259,272, bss 2,513,700 B), `tools/libtab.py symcheck` passes, `cor_pack.py pack-large` (with `--ops`)
    and `cor_pack.py check` pass: `trx64u.cor` 3,757,967 B (bitstream `54dc9f9e`; a local build, not a CI package).
  - P4-1: heap = 0xE70000 - align16(E) (E = `_end`; `__heap_limit` 0xE90000 less the stack reserve 0x20000, from the
    image's heap record; `heapcfg.c` and `cor_pack.py` `app_heap` compute the same). On 0.4.1: E = 0x3F9CE4, heap =
    0xE70000 - 0x3F9CF0 = 0xA76310 (10,969,872 B). That is the region the heap manages, not what is unused: the base's
    own allocations come out of it, and so do our libraries' declared needs (ring keeps 1,486,893 B, peak 2,931,868 B;
    trx64u peak 66,044 B; monitor peak 50,240 B); the 14 libraries of the TOML B declare none. About 336 KB of the image is
    MultiCore's own trx64u monitor (trxmon's `tm_*.o` in `libtrxbase.a`, `monitor.cc`, `monsrv.cc`, `monitor_grid.cc` of
    `base/SIZED.txt`), which never serves a core with a hooks library (M4); it leaves our app with the first SDK from
    `t239.3-v` (V-4 drops D12_TRX64U from the package's lists), and P2-6 deletes the code after our P2-5.
  - Lint: `ci/lint.sh` runs the SDK's `rtl_check.py lint-src` on `base/`, `ring/`, `monitor/` (48 files, none with a
    literal I/O address). `rtl_check.py check` is not in it: the tool cannot follow 13 of the 33 blocks; MultiCore teaches
    it the six patterns and trx_sys's held requests in its step V-1d (goal: all 33 agree) and fixes its `cbm1541` and
    `c64cart_regs` maps there (`_only`); `cart_regs` and `drive_a` keep their own properties until then.
  - `io.window` on 0.4.1: both readers (`tools/descrec.py`, the base's `descrec.cc`, the same `basereg.def`) refuse a
    window that leaves its firmware span and compare it with the base's registers after the translation; `window()`
    (`libsvc.cc`) returns null for a block off the I/O bus. 0.4.1's `descrec.py` (`parse_desc`) on the packed record 6:
    all four windows pass, as on `b43857a6`: `audio_mixer` 0xA0000500 +0x14 -> 0x10100500, `speaker_mixer` 0xA0000540
    +0x6 -> 0x10100540, `c64_palette` 0xA0080800 +0x440 -> 0x10180800, `hdmi_palette` 0xA0045000 +0x40 -> 0x10145000;
    none covers a register of the base (nearest: `master_volume` 0x10100550), all lie on the I/O bus (bit 28).
    `desc.toml`'s window comment named 0.4.0's `window_to_toml_a`, which 0.4.1 no longer has; fixed.
  - The `--ops` check of `pack_cor.sh` stays green: it compares record 6 with `base/ops_trx64u.txt`, a fixed list
    (MultiCore's export at `e4f99648`), not with the app. The golden is unchanged. What the app of SDK 0.4.1 does not
    export of that list: `update.run_record` (the `.cfw` run, P2-1; `menu.toml:890`), the six `online.*` operations and 48
    `online.*` states (the Online sub-app, L-3; the F3 Online tab of `menu.toml`), `reu.playmod` (D5), the states
    `diag.trace` and `reu.sampler`; `prg.run` and `prg.load`, which the base's REST and RPC call (`rest.cc`,
    `rpccontract.cc`), are registered by nobody in our app (MultiCore's `c64.cc` did, R-1); ours keep `c64.run_prg` and
    `c64.load_prg`. What it exports beyond the list: `mouse.sens`, `mouse.accel` (D25), our `ring.init`. These come from
    the code and MultiCore's goldens at both pins; the measured list (REST `/v1/ops`, `/v1/state` of a running app) needs
    ue2emu. P2-5 and the Online sub-app are step 2b. MultiCore's goldens and ops sources are the same at `8ff28ba9`
    (0.4.1) as at `c5ca4556`.
  - Open: a TOML A record in the `.cor`. On SDK 0.4.1 record 6 holds a description (version 6) or a TOML A (version 7),
    never both (`descrec.py` `compile_desc`, `descrec.cc`), and `cor_pack.py` compiles `--desc` without maps; so
    `trx64u.cor` carries no TOML A record, `bundle_check_identity` finds no map and the run-time ID and build check
    (api.md 6.14.13, D30 condition 2) does not run for trx64u. MultiCore builds a record type beside the description,
    `cor_pack.py --maps` and the reader as its own package after this push. Waits for it.
  - Open: ue2emu. Our CI has no ue2emu stage and the tree no script for one; the runs of 2026-11-23 were by hand on the
    Mac (ue2emu 0.7.4). BASIC READY on the app linked here is not shown yet. (Shown in step 2b, below; the gap above was
    taken from the code and is short of what the app exports, 2b has the measured list.)
  - Not done here: a CI run (no push), the registry upload of 0.4.1.
- **2026-11-24: step 2b of the D28 plan: P2-5 (our libraries register what the base dropped; trex_crt and softboot are
  our sub-apps), the Online sub-app in record 7, the operations measured on the running app** (Alex's go 2026-11-24).
  - Operations of `lib_trx64u.cc`: `prg.run` and `prg.load` (R-1: the names the base's REST and RPC call; were
    `c64.run_prg`, `c64.load_prg`; `menu.toml`, `desc.toml` `run_op` and `dma.*` follow) and `update.run_record`, the
    `.cfw` run P2-1 took out of the base: `base/cfwrun.cc`, MultiCore `75da955da^` `coreapp/src/update.cc` on the base
    API (`io.ddr_write`/`ddr_read`, `sys.machine_handover`): pre-header (fingerprint, family, product against the state
    `diag.board`), every record inside 0x03000000-0x03C00000, read back and compared before the jump; refused where the
    state `host.multicore` is set. The family is the constant `U64MK2`: the base API gives none (SvcBoard), asked for P2-6.
    T64: ours (`c64.run_t64`, `c64.dma_t64`) and the base's `menu.t64` (`t64_vlist.cc`), unchanged. The TRX64 runtime:
    the monitor library's TCP 4312 server; the hooks the base calls by name are our libraries' (`ring.init`, `ring.poll`,
    `ring.unfreeze`: `ringops.cc:573`; `monitor.serve`, `monitor.release`: `lib_monitor.cc:396`). trx64u: 67
    operations, 54 states (budget 66 -> 67), image 89,252 B.
  - Sub-apps `apps/trex_crt`, `apps/softboot`: MultiCore's `t239.3` `8ff28ba9` copies (25 files, equal to their blobs
    before the change), built against LibApi 3.3 by `build.sh` with the SDK's `mk/app.mk`, from a clean copy each time
    (app.mk tracks no header). Changed: the Makefiles' include, provenance in both READMEs and two `app.toml` comments,
    and trex_crt comments the code contradicts (the removed firmware files, f4b2d673; the old frame and 21 x 8 view).
    `trex_crt.u2a` 88,909 B `4bc89def...`: 19 operations, 39 states `cart.*`, needs `cart_port>=1.0`; `softboot.u2a`
    10,766 B `cd0bc3c9...`: none, needs `standalone>=1.0`. Both in record 8 under `apps/`. Function table: P2-6.
    `build.sh` also relinks each library every time (lib.mk does not make the image depend on the Makefile's budget).
  - Base rows (P2-4): the SDK's base UI TOML routes them to F5 video/audio and F7 settings, tabs `menu.toml` has; no
    `[[place]]` or `[[group_route]]` needed (`menu.toml` header).
  - Online: record 7 `--lib online.u2a` (`sdk.pin` `online_*`, sha256 `970b0c2c...`; rebuilt here byte for byte from
    `8ff28ba9` `apps/online` with its `coreapp/src/online_api.h`, which the SDK does not ship). `fetch_sdk.sh` takes it
    as its second argument or downloads `trx64-online/1.0-8ff28ba9` (not uploaded). `menu.toml`'s F3 tab `online` and
    its 20 nodes are gone (the sub-app's own record has them); the TOML B names no online library. This tree has no
    `menu_rec.inc`: record 6 is compiled at pack time by the pinned `descrec.py` (contributed-tab entry 28 bytes).
  - The operations check: SDK 0.4.1's link makes no operation table (`app_build.py`: `appsizes.h`, driver table,
    `libtab_table.cc`, `io.ld`), they are registered at run time. `tools/emu_ops.py` runs the packed `.cor` in ue2emu,
    waits for READY from the core the app started and reads REST `/v1/ops`, `/v1/state` (MultiCore's `export.py`
    format); `pack_cor.sh --emu <ue2emu>` compares that both ways with `base/ops_trx64u.txt`. `cor_pack.py --ops` stays:
    it checks every name and binding of record 6.
  - `base/ops_trx64u.txt`, each changed line: `c64.run_prg`, `c64.load_prg` -> `prg.run`, `prg.load` (R-1); + `mouse.sens`,
    `mouse.accel` (the base, `engines.cc:191`, D25); + `ring.init`, `ring.poll`, `ring.unfreeze`, `monitor.serve`,
    `monitor.release` (ours, above); - `monitor.open`, `monitor.tick` (the monitor registers neither since `8f68476d`);
    - `reu.playmod`, state `reu.sampler` (MultiCore `1a820f729`, D5); - state `diag.trace` (MultiCore `570331dc3`,
    P2-1); + state `online.folder` (`online_search.cc:900`). `update.run_record` and `online.*` stay, now registered.
  - Proof (Mac, ue2emu 0.7.4): `pack_cor.sh --bit c64_basic_54dc9f9e.bit --bit-commit 54dc9f9e --version
    0.3.1-dev+u18-2b --emu ~/Development/u64-emulator/target/release/ue2emu`: `trx64u.cor` 3,886,419 B `c3c9a481...`
    (the same bytes twice, the second time on a fresh `fetch_sdk.sh`), records 1 3 4 5 6 7 8; 4 libraries loaded, 0
    refused, no menu refusal; READY 3.6 s after start ("**** COMMODORE 64 BASIC V2 ****", "38911 BASIC BYTES FREE"),
    after `[core] TRX64U released`; ping on 4312 answers `trx64-runtime/2`; 204 operations, 188 states, equal to the
    list (392 lines). A second start on the same stick: `[app] 2 application(s)`, menu nodes 45 -> 48.
  - Found: the base scans `<core folder>/apps` (`run.cc:730`) before it installs record 8 (`run.cc:749`), so a new or
    changed sub-app shows from the second start on (MultiCore). Record 8 never writes over a file unless the record
    names that file as an earlier version (`instfiles.cc`, `cor_pack.py --files-old`): a later trex_crt or softboot has
    to keep the shipped one as `<stem>.v<N>.u2a`, or the card keeps the old. c64_basic's TOML A has no `icap` block,
    so the app is standalone by its link (`board.h` `icap_id`): under the MultiCore host it has no Return to host,
    offers softboot and runs a `.cfw` itself (Alex and MultiCore).
  - A CI emu stage would need: a Linux ue2emu 0.7.4 or later with its default features (`trx64`, `net`; MultiCore's
    `ci/emu` image is built without `net`), a libslirp with `slirp_handle_timer` (Ubuntu 22.04's 4.6 lacks it),
    python3, the four ROMs of 1541ultimate's `roms/` (not in our git), `--net user` with three free local ports; about
    4 s and 52 MB here. Not built.
  - Not done here: a CI run (no push), the registry uploads (0.4.1, `trx64-online/1.0-8ff28ba9`), the emu stage, a
    board test. `8ff28ba9` is no longer on MultiCore's local `t239.3`: it became `2b6d6a56b`, which differs from it only
    in `tests/loader/test_libload.cc` (the SDK, `apps/` and `online_api.h` are the same). `sdk.pin`, the registry package
    names and the provenance lines still name `8ff28ba9`; they follow once MultiCore has pushed.
- **2026-11-24: T-3 of the D28 plan (T256 S6: the ring library shows the Timewarp still and bar through `api->overlay`) is
  not built: SDK 0.4.1 lacks the picture, and the base draws a bar of its own** (Alex's go 2026-11-24). Nothing of the
  code changed.
  - The picture. A library's still is a surface the library paints and owns (api.md 4.17.6, 4.17.8.3). Timewarp's frame
    is what the host's still capture holds after the scrub step's frame (`ring.cc` `scrub_show_locked` waits for the
    `still_status` count). A library cannot read it on 0.4.1: the capture's two slots are the base's own (MultiCore
    `8ff28ba9` `video.cc` `Video::still_start` :346-357 mallocs them and writes STILL_BASE); `still_base` reads nothing
    (`pix_layer.v` `cur` :159-166 reads 0x068 and 0x0A4 only; TOML A `trx_pixel`, `dir = "w"`); no member of
    `svc_overlay.h` or `svc_io.h` and no base export returns the picture; the base takes its own still only when the
    machine stops (`overlay.cc` `settle` :377-383), and a scrub step runs its frame through `trx_gate` without telling
    the base. Slots of ours behind `still_base` would leave the base's capture stale for its own still and the PAUSED tag
    (the base writes the register once, at the core's start).
  - Measured in ue2emu 0.7.4 on `trx64u.cor` `c3c9a481` (step 2b): `10 print ti` / `20 goto 10` runs 6 s, the menu
    opens, Timewarp...: the page shows the base's still (the frame of the menu's stop, `TI` 564 in the last line) behind
    the base's bar ("now", frame 268 / 268). After UP (-1.0 s, 218 / 272), UP (-2.0 s, 168 / 272) and LEFT (-2.1 s,
    167 / 272) everything above the bar is the same pixel for pixel, only the bar changes; the ring logs the three
    scrubs (seq 218, 168, 167, ok). S6 fails today: the picture does not follow the scrub.
  - The bar. The base's timeline page draws a bar of its own on every build of layout `timeline` (`screens.cc`
    `build_timeline` :930-978: 768 x 48 at (96, 476) of 960 x 540, the glyph, the texts, the track); over a still the
    screen is drawn with the person's Overlay Transparency (`ui.cc` `ov_apply` :1430; the default 25 % is opacity 192).
    A bar painted into our still lies under it: the firmware's `tw_layout` (`trx_video.cc` :653-672) gives the same
    rectangle. T256 D3 moves `tw_draw_bar` into the ring library and keeps `build_timeline`; one of the two has to go.
  - What T-3 needs of MultiCore (found here; asked 2026-11-24, answer below): (a) a call that paints the picture the machine drew last
    into a library's surface as the base paints its own still (the person's palette, the place and Scale of the running
    picture); (b) the timeline page without its bar when the library paints it into the still, or the bar stays the
    base's and (a) becomes "take your still again". With both: `menu_video(1)` (the page is up, `lib_trx64u.cc`
    `hook_menu_video`) lets the ring library take its surface, paint the picture and the bar (`tw_draw_bar`'s place and
    look) and `show("still")`; each scrub operation paints again and calls `update`; `menu_video(0)` and `ring.unfreeze`
    call `remove` and free it.
  - Memory (api.md 4.17.6: the surface is the library's): the still element is 960 x 540 RGB565, 1,036,800 B of the ring
    library's `api->mem` while Timewarp shows; with its keep 2,523,693 B, under the declared peak 2,931,868 B, which is
    the keep plus the `.trxring` save (1,444,975); the save and the surface at once would be 3,968,668 B. The base holds
    its own still beside it on its heap while the machine stands (the picture's rectangle, at most 1,036,800 B; in this
    run 960 x 540: ue2emu has no gateware scaler, so the still fills its element).
  - Comments the code contradicts, fixed: `lib_trx64u.cc` `hook_menu_video` (the base calls it for the timeline only,
    the PAUSED tag is an element since T-2; "CPU only" shows nothing through the key while a still lies under the menu),
    `ringops.cc`'s head (the page is F1/timewarp, its keys are `menu.toml`'s `[[key]]` rows). `build.sh trx64u ring`
    makes both images byte for byte as before (`trx64u.u2a` `b70c8f7b...`, `ring.u2a` `0afb3061...`), so the `.cor`
    and its `pack_cor.sh --emu` check of step 2b stand; not run again.
  - For MultiCore beside (a) and (b): `core_hooks.h` and api.md 9.7.13 still say `menu_video` is on for the PAUSED tag;
    SDK 0.4.1's base turns it on for the timeline only.
  - MultiCore's answer (2026-11-24): the line references hold. **S6 is a regression of t239.3 for trx64u**: until T-2
    the timeline page showed the machine through the key colour and a scrub changed that picture (MultiCore
    `core_hooks.h` :74-78); T-2's still is taken once, at the menu stop. So `u18` does not go to `main` before T-3 is
    built. It is a T256 question for Alex (D10's call list has neither call), asked by MultiCore: A) the ring library
    owns the picture, a new helper paints the last drawn frame into a library surface, and the base's timeline bar
    leaves with P2-6 (MultiCore recommends A: D3 as decided); B) the base keeps its bar and its still, and a new call
    makes it take the still again after a scrub step. The `menu_video` text of `core_hooks.h` and api.md 9.7.13 is
    stale (the code is right: the PAUSED tag is an element) and is rewritten with A or B.
- **2026-11-24: no Softboot in trx64u (Alex: "softboot ist eine App um aus einer .cfw die Ultimate-Gideon-App ins sram
  zu laden und starten ... für unser UI nutzlos weil unser bitstream doch gar nicht passt. Softboot muss also raus für den
  SDK Build. Es bleibt für den G Build").** `apps/softboot/` (step 2b's copy of MultiCore's ABI-3 port) is deleted;
  `build.sh` builds trex_crt as the only sub-app, `pack_cor.sh` installs only `apps/trex_crt.u2a` in record 8. Softboot
  registered no operation or state, so the golden is unchanged. It stays in the firmware build
  (`software/ultimate-app/apps/softboot`). The `.cfw` run (`update.run_record`, `base/cfwrun.cc`) stays: it loads a
  `.cfw`'s records into the updaters' window and starts the updater, the "Run Update" of Gideon's file browser. This
  supersedes step 2b's "trex_crt and softboot are sub-apps" and its note that trx64u "offers softboot" off the host.
  Proof: `pack_cor.sh --emu` on SDK 0.4.1 (bitstream `54dc9f9e`, version `0.3.1-dev+u18-nosb`): rc 0, `trx64u.cor`
  3,875,625 B (`f9868d5b...`), READY. from the core 3.5 s after ue2emu's start, 204 operations and 188 states, missing
  from the app 0, not in the golden 0; the first start lists 0 applications (the base scans `apps/` before it installs
  record 8, MultiCore's fix in SDK 0.4.2).
- **2026-11-24: T-3, Alex chose A ("Ok - answer A fits", MultiCore T256 D11, their integration `7a62b75b5`).** The ring
  library owns the Timewarp picture: a helper of the SDK paints the frame the machine drew last into a library surface
  (palette, place, Scale, like D3's text helper), the ring library paints frame and bar as the still (D10), and the base's
  timeline bar leaves with the trx64u code set (P2-6). MultiCore sends the helper's exact shape before T-3 is built on it;
  until that SDK, T-3 waits.
- **2026-11-24: who owns the four windows, Alex: answer C, "sauber trennen".** "Laut/Leise und der Mini Equalizer ->
  base im SDK. UCI Befehle, konkrete SID Einstellungen usw bitte erst mal als lib hierher. Ggfs leihen wir die dann c128
  und c64scpu."
  - The base: master volume and the mini EQ (`master_volume`/`master_tone`, 0xA0000550/551 in basereg.def). c64_basic
    gets no RTL for them (Alex: no; `ultimate_sys.vhd` decodes neither 0x100550 nor 0x100551), so trx64u has no Volume
    and no EQ of the base, a decision with Alex. MultiCore makes the base offer them only where a TOML A has their blocks.
  - Ours: the UCI commands as a library here (MultiCore T239.3 D36: its exports and API are ours alone; trx64u's TOML B
    drops their shared `uci`, and `cmd_if` becomes a core block), the SID settings with reSID8 (`resid8` becomes a core
    block: no filter call of the SID library is needed), the mixer's SID and socket gains (0x10100500-0B) and speaker
    gains (+0x40, +0x41, +0x44, +0x45), both palettes (core blocks; `desc.toml` loses its four windows).
  - With the same TOML A change `trx_pixel` (0x101A6060) becomes owner "base": the base's `video.cc` drives the still
    capture and the pixel layer through it (MultiCore confirmed; P1-7 had it core). The "13 core blocks" of the step
    above are no owner question: rtl_check `--core-in-design` (`t239.3-v` `rtl_check.py` :4219-4221) checks the core
    blocks c64_basic decodes itself like base blocks (our TOML A has 11 of them in the U2P, U64 and C64 pages).
  - Not built yet: the TOML A, `desc.toml`, `u64cfg.cc` and the UCI library.
- **2026-11-24: SDK 0.4.2 and 0.4.3 (MultiCore), not pinned.** 0.4.2 (`307c3382...`, commit 028553da): record 8 is
  installed before the scan of `apps/` (`run.cc`), `include/online_api.h`, windows and core blocks only inside the board's
  I/O span [0x10000000, 0x11000000); our record 6 and `drvtab.h` are byte for byte those of 0.4.1. 0.4.3 (`5b5820f4...`,
  commit e45fbb97) adds T-3's helper, as agreed with MultiCore: `api->overlay->frame(name, surface)` paints the frame
  the machine drew last into a library surface in the caller's task (no wait for the UI task; OVL_E_NOFRAME leaves it
  unchanged), and `still_palette(f)` makes the library the source of the still's colours. Under trx64u the base's still
  paints Colodore today: `Video::set_still_palette` is called only by `u64cfg_register` of MultiCore's c64 core, and
  both are in the discarded sections of our `app.map`. `menu_video` stays T-3's enter and leave signal. The pin waits
  for Alex.
- **2026-11-24: an ICAP loader in c64_basic, the way back to the MultiCore host (Alex: A; MultiCore T239.3 D37).**
  - RTL: a port of MultiCore's host loader. `ultimate_sys.vhd` decodes region `r_icap` 0x060600-0x0606FF (free between
    `r_gcr` and `r_eth`): ID 0x7A at +0x00, data +0x10 (four bytes, first byte in bits 31:24, one word to the ICAPE2),
    enable +0x14 (set from 0: the word counter clears), word counter +0x18..+0x1B; Gideon's +0x04/+0x08 stay inert;
    no decouple +0x20 and no release +0x21 (no partition, no port lock). `top.v`: ICAPE2 X32 on `cpu_clk` (100 MHz, its
    maximum) with the host's byte bit reversal.
  - Bench: `sim/dma_bus` checks the ID, the enable, the words, the counter and a new session (GHDL 5.0.1, PASS).
  - The way back is the base's `hw_return_to_host`: IPROG with WBSTAR 0 after the menu marker of the hand-over page
    (MultiCore T135 D3); the host lies at flash 0x0 (ADR-0055). The contract (`docs/contracts/trx64u-firmware-handover.md`
    §1, §7, §8) is MultiCore's `f3dcf4a78` byte for byte: "started by the host" is the ICAP loader plus a valid hand-over
    page. Under the host trx64u offers "Return to host menu" (the base's row) and our "Power cycle"; standalone (c64_basic
    at flash 0x0, no page) only "Power cycle", with its settings in the flash. "Power off" stays beside them.
  - The `.cfw` run keeps its refusal under the host (`cfwrun.cc`, state `host.multicore`).
  - Open: the TOML A block `icap` (owner base) comes with SDK 0.4.4, which splits the host's region into `icap` and
    `partition` and brings the profile rule; until then the app is linked without `icap`, so the base sees no loader and
    trx64u stays standalone. A CI bitstream run, then the board test (Alex): "Return to host menu" reaches the host's menu.
- **2026-11-24: CI run 475 (dispatched on `u18` 9716a546): green, the ICAP loader in the bitstream.** Lint, app, Bitstream,
  Package, Simulations, Update files and Core (.cor) pass. c64_basic built anew (synth to bitstream 6 min): ICAPE2 1 of 2
  (0 before), Slice LUTs 27,608 of 32,600 (84.69 %; 27,593 before), WNS +0.070 ns; `dma_bus` PASS in CI as well. The registry
  uploads it needed (SDK 0.4.1 and `online.u2a`, then 0.4.4) are mine (Alex: "Uploads machst du doch selbst"), checked by an
  anonymous download and its sha256.
- **2026-11-24: our `trx64u.cor` for MultiCore's P1-8 (Alex: "Dann bau die .cor hier ... die muss nicht perfekt sein. Die
  Fehler schleifen wir hier dann aus").** The Core stage packs it on every run: run 475's
  `trx64-ultimate/u18-9716a546/trx64u_9716a546.cor`, 3,979,287 B, sha256 `513bd1aa...` (`cor_pack.py check` of 0.4.4 OK;
  bitstream 9716a546 with the loader, app on SDK 0.4.1, records 7 and 8 as in step 2b). MultiCore has URL and sha256; its
  package takes our `.cor` by sha256 and deletes its own trx64u copy (P1-8). Each later build has its own sha256.
- **2026-11-24: SDK 0.4.4 pinned, the TOML A block `icap`.** 0.4.4 (`29144432...`, commit 9cd216eb, headers `1065b4ac`, the
  same as 0.4.3's) brings D11 (`frame()`, `still_palette`), D37 (the host's ICAP region as the blocks `icap`, 0x1C bytes, and
  `partition`; the board is the host's when the loader answers 0x7A and `partition` is linked or the hand-over page is valid)
  and D36 (Volume and Tone rows only with the base block `audio_master`; the operations `volume.*` and `tone.apply` stay
  registered, so `base/ops_trx64u.txt` is unchanged). `gateware/c64_basic.toml` gets block `icap` (owner base, map `icap`,
  34 blocks) and no `partition`: under the host the base offers "Return to host menu" through the page; standalone, at flash
  0x0 without a page, it does not, and keeps the settings in the flash. Record 6 and the driver table of the other 33 blocks
  are byte for byte those of 0.4.1. The ICAP insert moved the lines of `ultimate_sys.vhd` (+5 from 354, +10 from 428, +12 from
  885, +44 from 1363, +54 from 1366): the references in the TOML A, `desc.toml` and `u64cfg.cc` follow. Not run here: the
  emulator check (`pack_cor.sh --emu`) on 0.4.4; it comes with T-3, which is built on 0.4.3 meanwhile.
- **2026-11-24: T-3 built (branch `u18-t3`, local; on SDK 0.4.3 through a TEMP pin commit that is not pushed, the T-3
  commit goes onto the 0.4.4 pin).** Alex's A (MultiCore T256 D11) and his word, relayed by MultiCore: the bar and its
  texts come from trx64u, the base only makes it possible.
  - Ring library: `twstill.cc` takes the page's coming and going from trx64u's hook `menu_video` through a new export
    `ringstill` (major 1, `ring/ring_still.h`; no operation, so nothing for REST and no change of the ops golden). On
    coming: a 960 x 540 RGB565 surface from the ring's heap (1,036,800 B, cleared once), `api->overlay->frame("still")`
    into it, the bar over it, `show("still")`; after every move of tw.step, tw.seconds, tw.oldest, tw.live and
    tw.next_mark: frame(), bar, `update`; on going: `remove`, the surface freed. A mutex orders the menu task and the
    executor (REST runs the same operations). tw.cancel does not paint: its key closes the menu.
  - The bar, `twbar.cc` `tw_bar_paint(surface, canvas, palette, TwBar)`: the firmware's tw_draw_bar (`trx_video.cc`)
    at the place of the base's copy (x 96, y 476, 768 x 48 on 960 x 540); per pixel in RGB565 1/4 frame and 3/4
    0x101018 (barmix), glyph, position, info, hint and track in the person's palette (the hook passes
    `u64cfg_palette`). The texts use c64_basic's overlay font (`ovl_font8.mem`, made into `twfont.h` at build time by
    `twfont.py`), not `api->overlay->text()`: the UI task writes those at its next frame, outside the paint. The states
    tw.pos, tw.info, tw.glyph, tw.track and the page's hint are unchanged; the base of 0.4.3 still draws its bar over
    ours until P2-6.
  - trx64u: `u64cfg_still_palette()` makes the palette in force the source of the still's colours (`still_palette`),
    called by the core's service poll (the start hook runs before the overlay service: OVL_E_NOSERVICE) and the hook.
  - Budget (ring/Makefile): image 136,160 + 16,852 B; heap_peak 2,931,868 -> 3,968,668 B = idle 1,486,893 + the still
    1,036,800 + the .trxring save 1,444,975 (the monitor's RPC can save while the page shows); heap_keep, ops and states
    unchanged. trx64u 89,760 + 120,684 B, within its budget.
  - Proof, all on the 0.4.3 base: `pack_cor.sh --emu` rc 0, `trx64u.cor` 3,883,643 B (`283c93dc...`), READY 3.6 s,
    204 operations, 188 states, missing 0, not in the golden 0. ue2emu, `10 print ti` / `20 goto 10`, Timewarp, UP, UP,
    LEFT, against the u18 libraries without T-3 on the same base: with T-3 the picture's lines read 506..567,
    443..506, 382..446, 382..443, each its step's C64 screen (at least 99 % of the 64,000 C64 pixels, UP 1 all of them;
    the rest is the frame drawn last lagging the screen RAM), without T-3 511..572 at every step. Colours: u64cfg's
    default palette, then a `.vpl` applied by `u64.apply_palette` (screen 00A200, text FF8200), without T-3 Colodore in
    both; the base's own still under the menu takes the `.vpl` too. Host: `tw_bar_paint` compiled on the Mac against a
    model of tw_draw_bar, 6 states, no pixel different; box, glyph, track marker, texts and the mix checked. Not shown:
    our bar in ue2emu (the base's lies over it), the board.
  - Found for MultiCore: frame() of the still takes 1.2 s in ue2emu (timed around the call: frame() 1,193-1,205 ms,
    the bar 45 ms; inside the base's `Video::grab`, not timed further), so the menu task needs 1,477 ms for the page's
    key and each step's picture follows its move by 1.24 s. The close from the page takes 1,248 ms instead of 49-64 ms:
    `remove()` sets `retake_` and settles on the UI task, and settle takes the base's still again while the machine
    stands (`overlay.cc` 389-400, 615-633), though the menu is closing; the time is that of one grab. The bar is laid
    out on 960 x 540: a library learns no canvas, so on 480p, 576p and 800 x 600 it lies partly outside.
  - Found here: `build.sh` and `pack_cor.sh` keep objects of the old SDK after a pin change (their `.d` files name the
    old SDK's files, which stay), so the first runs linked 0.4.1's base; `build/app/obj` had to be emptied by hand.
    `fetch_sdk.sh` given a relative path to `online.u2a` deletes the file before it copies it.
- **2026-11-24: T-3 on `u18` with SDK 0.4.4; CI run 476.**
  - Run 476 (3e96c8cb, the 0.4.4 pin with block `icap`) is green. Its `trx64u.cor` (3,981,511 B, `1f45b8bf...`) went to
    MultiCore, whose P1-8 started with run 475's. Read from the app's own bytes: the base reads the loader's ID at
    0x10060600, and the way back writes enable 1 at 0x10060614, the 36 bytes FFFFFFFF AA995566 20000000 30020001 00000000
    30008001 0000000F 20000000 20000000 (WBSTAR 0, IPROG) to 0x10060610, then enable 0. Run 475's app has none of these.
  - T-3 (b1a92d07 of `u18-t3`, without its TEMP pin) is cherry-picked as 5c9ff24a. On 0.4.4 the libraries link at the
    sizes of 0.4.3 (trx64u 89,760 + 120,684, ring 136,160 + 16,852; monitor 209,004, trex_crt 80,016), so the budgets
    stand. `pack_cor.sh --emu`: rc 0, `trx64u.cor` 3,986,811 B (`5b2c9970...`, the same from a clean rebuild), READY
    3.6 s, 4 libraries loaded, 0 refused; the app's heap 10,967,632 B, free 7,986,912 before the libraries and 6,703,968
    after them; operations and states: missing 0, not in the golden 0.
  - The ue2emu check of T-3 (the run above: `10 print ti`, Timewarp, UP, UP, LEFT, then a `.vpl`) passes on 0.4.4 as on
    0.4.3: the picture's lines 514..578, 457..517, 396..459, 396..457, each its own step's screen; the colours 2928B5 /
    6B69EF, then 00A200 / FF8200; frame() 1,193-1,206 ms, the bar 45 ms. 0.4.4 leaves overlay.cc, screens.cc and
    trx_lv_port.cc as 0.4.3 has them.
  - Fixed with it (the two traps above): a build folder keeps the sha256 of its SDK tarball in `.sdk` and starts clean
    under another (`build.sh`, `pack_cor.sh`); `fetch_sdk.sh` given `.sdk/online.u2a` by a relative path keeps it.
- **2026-11-24: SDK 0.4.5 pinned; the bar on the canvas of the mode, and the only one.**
  - 0.4.5 (t239.3 e8b76719; tarball `e87e2039...`, headers `00b60e80`, base archive `1cd49804`) is MultiCore's answer to
    our three findings (T256). It brings `canvas()` (api.md 4.17.16) and the grab's column table, and the closing menu no
    longer grabs when a library's still goes. D12: the base draws no Timewarp bar. D14 (Alex's suggestion): the base frees
    its own still while a library's still shows. In our registry as `trx64-sdk/0.4.5-e8b76719` (201, download checked).
  - `twstill.cc`: the bar lies at the bottom of the part of the still the canvas shows, asked at every paint. That part
    is the canvas cut to 960 x 540: the still is the base's element "still", 960 x 540 at the top left (base_ui.toml),
    and an element keeps its size while the canvas cuts it (overlay.cc `ovl_place_on`). A changed canvas clears the
    surface first.
  - Host check (`twbar_host` on `twbar.cc`): on 640 x 480, 720 x 540 (576p), 800 x 540 (800 x 600) and 960 x 540, every
    changed pixel lies inside the bar and inside the shown part, the whole bar is mixed, and info and hint stay apart.
    The test used the longer info text "the ring holds no frame yet"; at 480p the gap is 32 pixels.
  - On the narrower canvases the bar keeps the 1080p scale (48 rows, the position at 2x). Below 720 wide, the firmware's
    layer used its small scale (32 rows).
  - The ring's image is 136,352 + 16,860 = 153,212 B (+200 for the canvas), and its budget follows. trx64u and the
    monitor are unchanged.
  - `pack_cor.sh --emu`: rc 0. `trx64u.cor` 3,985,875 B (`ec30395d...`), READY 3.6 s, 4 libraries loaded, operations
    and states equal to the golden. The build folders of 0.4.4 started clean by their stamp.
  - The ue2emu check passes as before, and our bar is now the only one (glyph, position, frame n / m, hint, track).
    frame() takes 217-220 ms (0.4.4: 1,193-1,206), the bar 45 ms. The close from the page takes 48-63 ms (0.4.4:
    1,249-1,252).
  - Open: the ring's heap_peak (Alex: as it is, or the still taken away for a save, with a progress element), and the
    board.
- **2026-11-24: P1-8 at MultiCore: their package takes our `trx64u.cor`; the contract follows (D21, D44, D45).**
  - MultiCore's t239.3 c26aab2aa pins our CI run 478: `trx64-ultimate/u18-a294af0f/trx64u_a294af0f.cor`, 3,985,867 B,
    sha256 `18c16a1e...`. MultiCore deleted its own trx64u image, `c64_desc`, `coreapp/cores/trx64u/`, `c64.cc`,
    `hk_c64`, its two trx64u lint checks and the trx64u tests by the D21 sort.
  - Our link does not use any of it: `app.elf` has no `c64_register`, `hk_c64`, `c64_desc`, `c64_go` or `cbmrun`. The
    descriptor's code is ours (`base/c64core.cc`), and the record's id "TRX2" (`base/desc.toml`) keeps `appsvc.cc`'s
    trx/c64 groups.
  - The delivery (MultiCore T239.3 D45; Alex: "Findet mal einen Weg mit dem builder Lokal", no GitHub for now):
    MultiCore's builder poller fetches the pinned file from our Gitea generic registry over the LAN, without a token
    (checked from the builder: HTTP 200). It keeps the file only when its size and sha256 match the pin, and publishes
    it as `bit-imp-trx64u-<key>` in trx64mc-builds. For each new build we send PACKAGE, VERSION, COR_FILE, sha256 and
    size, and MultiCore moves the pin.
  - The contract (`docs/contracts/trx64u-firmware-handover.md`) is MultiCore's 8058b7617 (integration), byte for byte
    the same as cfa7872a7 (t239.3), sha256 `a643e632...`:
    - §1: `trx64u.cor` is built, tested and delivered here; the coreapp and the SDK are MultiCore's.
    - §2: the pin fields.
    - §5: the `.cfw` note.
    - §10: the menu is ours.
    - §7 and §8 (D37) are unchanged.
    - Their first P1-8 text (9de718305) was based on the text before D37 and dropped §7's way back and §8's loader; it
      was merged again on our note.
  - Open (MultiCore D22, ours to rewrite; no test rig in the SDK):
    - test_boot_c64 and test_c64_prg with c64_rig.h;
    - the C64 cases of test_prompt, test_cut (trx64u key mapping, monitor_grid adapter) and test_desc (caps, PRG
      actions, machine_info);
    - the TRX64U lines of the keywalk/ids goldens and menu_c64.txt (`ops_trx64u.txt` is ours already);
    - media.py, crtmedia.py and the coreapp_emu run (tests/coreapp/emu/run.sh);
    - the render's C64 yardstick (c64-home, c64-settings);
    - the UI features of our menu (export, home, letters, library, media_level, navigation, overlay, overlay_trx64u,
      rows_shots, settings).
    - MultiCore keeps c64_model.h; copy it with its provenance when needed.
  - MultiCore's next SDK drops `coreapp/cores/c64.cc`; 0.4.5 stays as it is.
- **2026-11-24: the ring's heap_peak, Alex: "Mach erst mal alles so wie in der gideon based app."** heap_peak stays
  3,968,668 B: the Timewarp still and a `.trxring` save may be held at once. The firmware does the same: its
  `trx_trxring_save` (`software/ultimate-app/software/io/c64/trx_trxring.cc:260-279`) mallocs its buffers on top of the
  Timewarp layer's and never touches the video layer or the still.
- **2026-11-24: the owner split built (Alex's C; branch `u18-split` from `u18` a294af0f).**
  - TOML A (`gateware/c64_basic.toml`), core blocks with their properties read from the RTL: `audio_mixer` 0x10100500
    (12 gains, writes only; `ultimate_sys.vhd:1257-1274` decodes 0x00-0x0B), `speaker_mixer` 0x10100540 and
    `speaker_mixer_b` 0x10100544 (two blocks, because +0x02 and +0x03 decode nothing and rtl_check refuses a block over
    them), `c64_palette` 0x10180800 (RGB, 0x40 bytes; `pal_we` only with address bits 10:6 = 0, so no YUV at +0x400),
    `hdmi_palette` 0x10145000 (`overlay.v` block 5). `resid8`, `cmd_if` and `gcr_coder` are core blocks with inline
    properties under the names of the SDK's maps `sid`, `uci` and `gcr_coder`; `trx_pixel` is the base's. 0x10100550 and
    0x10100551 stay out: no Volume and no Tone of the base on trx64u (decided gap, Alex).
  - `desc.toml` has no `[[window]]`, and `u64cfg.cc` writes through `drvtab.h`; `c64u_win` and `WIN_*` are gone. The
    writes c64_basic never decoded are dropped: mixer bytes 0x0C-0x13 (drive 1, drive 2, tape; the hidden settings "Vol
    Drive 1" and "Pan Drive 1" are no longer read) and the C64 palette's YUV copy. The two hidden rows stay in the menu
    as the firmware has them (`u64_config.cc` :509, :519, `CFG_MIXER6_*`, bytes c64_basic does not decode either), so
    record 6 is unchanged (Alex: "Mach erst mal alles so wie in der gideon based app").
  - Address check (host, in the scratchpad): `u64cfg.cc` of a294af0f and the new one, each compiled as it is with its own
    `drvtab.h`. Every register access faults in macOS's `__PAGEZERO` and is logged at its board address; the old windows
    come from the base's own code (`libsvc.cc` `window()`, `descrec.cc` lookup and translation, verbatim) over
    `desc.toml`'s rows. 34 scenarios with 8 settings seeds: the new build writes 8,131 register bytes, each at the old
    board address with the same values per address; the old one's 3,264 more bytes are all undecoded ones (192 mixer,
    3,072 YUV); every other call into the base is the same. A wrong offset fails the check.
  - rtl_check of SDK 0.4.5 (it has no `--core-in-design`): `ident`, `audio_mixer`, `speaker_mixer`, `speaker_mixer_b`
    and `resid8` agree (5 blocks, 15 properties). For `c64_palette`, `hdmi_palette`, `cmd_if`, `gcr_coder` and
    `trx_pixel` the checker stops at an RTL pattern it does not know (`vic_scaler.v:52`, `overlay.v:40`,
    `synchronizer_gzw.vhd:48`, `gcr_codec.vhd:63`, `pix_layer.v:80`; the last two also at a294af0f). Their decode was
    read by hand, and the TOML A names the lines. Over the whole TOML A, 23 of 39 blocks agree; the tool stops at an
    unknown pattern for 15 and finds a byte of `icap` (+0x1) that nothing decodes. `ci/lint.sh`'s reverse lint now
    covers `uci/` too (59 files, no literal I/O address).
  - UCI: form (b), our own sources linked by our TOML B. Form (a) is out on SDK 0.4.5: the library is C++ on the base's
    own objects (the server `cmd_if`, its targets' base class, the FileManager, the RTC), which a record-7 image cannot
    link, and its freeze request is an entry of the app's library table that LibApi does not offer. `uci/` holds
    MultiCore's `uci.cc`, `uci_slot.cc`, `uci_exports.cc` and their headers (t239.3 e8b76719, byte for byte SDK 0.4.5) as
    `trxuci*`, with a PROVENANCE header each. The names differ because a library's include path has the SDK's
    `include/` first. The TOML B's library `uci` takes them (`pack_cor.sh --src uci`), and the export is still "uci".
    `pack_cor.sh` now drops objects the link did not load before the symbol check (the old `uci*.o`).
  - Found on the way: the base's server writes `slot_base` (0x47) itself at its start (`CommandInterface::start`); the
    base's `video.cc` drives `trx_pixel` by its own offsets; the ring reads `trx_pixel`'s still count through the
    driver table.
  - Budgets: trx64u 88,924 + 120,672 = 209,596 B (a294af0f, measured: 89,760 + 120,684), image budget 209,596. The ring is
    unchanged at 136,352 + 16,860 = 153,212.
  - `pack_cor.sh --emu`: rc 0, READY 3.6 s, 4 libraries loaded, 0 refused, operations and states equal to the golden.
    The T-3 check passes (`check_emu.py`, the palette through `u64.apply_palette`). UCI in ue2emu: with "Command
    Interface (UCI)" set over REST, ue2emu's monitor `dir` (DOS target 2 over the UCI block) lists `/`, `/Usb0` and
    `/Usb0/cores`, answered by our library.
  - Not exercised: the board, the UCI from the C64's side, and CONTROL's palette and DECODE_TRACK commands. Their
    registers are covered by the address check and by the unchanged driver table names.
  - MultiCore's answers to the split's six questions (2026-11-24; 1, 2 and 4 Alex's decisions T239.3 D48-D50 on their
    integration 527671089, 3, 5 and 6 MultiCore's from the code):
    - D48 (Alex: "the advantage of libraries is that we can 'share' them when needed"): the UCI server
      (`command_intf.cc`) leaves MultiCore's base and becomes a source of the library `uci`, taking `cmd_if`'s address
      from the driver table. SDK 0.4.6 hands it to us with provenance; we add it to `uci/`. `uci` leaves MultiCore's
      shared app and the SDK; no core of theirs runs UCI.
    - D49 (Alex: "UCI is relevant only for the trx64 core ... it must always run"): UCI is a linked library of trx64u
      only, form (b). There is no `.u2a` form, and LibApi gets none of the three things it would need.
    - rtl_check: `--core-in-design` and the patterns it stops at here (VHDL for-generate, ascending vectors, a genvar
      select as a constant, the debugger behind trx_sys's handshake) are on MultiCore's t239.3-v. They reach us with SDK
      0.4.6. Before cutting 0.4.6, MultiCore runs it on u18's TOML A and fixes what still stops (P1-3's proof). `icap`
      +0x1 decoded by nothing: agreed.
    - D50 (Alex: "Ok. 1"): a core's own library reads base blocks through the driver table and writes only blocks its
      core owns; 0.4.6 gives it only read macros for base blocks. The ring's `trx_pixel.still_status` read stays. Our
      one direct write to a base block is the ring's `c64cart.cmd` = inval (`ring.cc` :1132, :1396, :2386, after
      `cart_ram_put`). MultiCore: the owner does it. In 0.4.6 the shared c64cart driver drops the cached lines itself
      after every `ram_write` and every `image_write`, which covers all three: :1132 and :1396 follow `ram_write`, :2386
      follows `ram_write` or the image a ring-file load puts back (`rom_put`, `ringfile.cc`, through `cart_rom_write`,
      `ringmachine.cc`, to `image_write`). The three writes go with the 0.4.6 pin, not before: 0.4.5's driver does not
      drop the lines. 0.4.6 leaves out only the base blocks' `_WR` macros; `_ADDR`, `_RD`, masks and values stay, so
      `DRV_OFF` and the io group's offset-taking calls (`cart_wr`/`cart_rd` on `cart_regs`, `rom_write`/`rom_read` on
      `c64_roms`) stay, and those writes are the base's.
    - D51 (Alex, raised by the D50 question: "The core must be able to persist from libs, apps. And triggered over the
      API too."): LibApi gets the base's write-back service for libraries in 0.4.6 (register a writable medium, report
      changes, write now); apps and the API ask for the write. MultiCore's first text had c64cart's `ram_write` and
      `image_write` report their writes, so a ring restore would have counted as a cartridge change. We showed from the
      firmware that neither a Timewarp rewind nor a state load is one (`trx_ring.cc` `restore_bus` and :2264-2287 write
      the RAM window and the flash image, then CART_INVAL; only the C64's own flash or EEPROM programming marks a change,
      `trx_cart.cc` `note_change_isr`), and reporting them would make Ask ask after a mere rewind and Auto overwrite the
      user's .crt with an older flash image. Corrected on MultiCore's integration 9a402fb21: in 0.4.6 both calls only
      add the inval and report nothing; a library that changes cartridge content on purpose reports it itself. Our ring
      needs no change for it beyond D50's.
    - The SDK's generic `trx64u_ops.cc` leaves the SDK with `uci` in 0.4.6; the rest of trx64u's code set goes with P2-6.
    - `PX_OSIZE`: intended. The base's `video.cc` writes it only when the VIC scaler gateware answers, with `PX_CTRL`'s
      scale bits for a layer without it; on c64_basic the write lands on an undecoded address.
- **2026-11-24: `trx64g.cor` is packed here too (MultiCore T239.3 D46; branch `u18-g` from `u18` e8927a74).**
  - Alex's decision, as MultiCore recorded it (integration dfea55cbd): "Yes, of course, please. If they pack the trx64u cor, the
    trx64g cor is not much harder. They can take the scripts from you or have them given." Both finished files lie in one version
    of our package, and MultiCore's builder takes each by its sha256 (D45). Until our first file exists, their pin stays and their
    builder packs trx64g itself.
  - `software/trx64g/` holds MultiCore's packer as it is (Alex: take their scripts as they are), copied from t239.3 cfa7872a:
    `pack_import_cor.sh` and `pack_large_cor.sh`, what they call (`core_toml.py`, `version.sh`, `pkg.env`), the pin
    `trx64g.pin` and the icon. Only what this tree forces changed: paths, `cor_pack.py` taken from the pinned SDK (`sdk.pin`, as
    for trx64u), and the pin values, which each run brings. Each file has a PROVENANCE header, and a `diff` against MultiCore's
    file shows only those lines.
  - `ci/cor.sh`:
    - It fetches `ultimate.app` with `c64_basic.bit` and checks both against the manifest.
    - It writes the pin of the run (`SEMVER` is the manifest's "version") and runs the copied `pack_import_cor.sh`.
    - It publishes `trx64g_<commit8>.cor` beside `trx64u_<commit8>.cor`, both with their `SHA256SUMS` lines.
    - It removes the older `.cor` files of either core from the version and from `-latest`.
    - trx64u's part and `release_reuse_skip` are unchanged.
  - The version inside is `x.y.z-dev+<commit8>`, or `x.y.z` on a tag build. MultiCore's `version.sh with` makes it from the
    manifest's "version" (`APPL_VERSION_NUMBER`), which is the contract's §2 form. `bit_commit` is the commit8. `app_commit` is
    the submodule commit; MultiCore's builder passed none.
  - Proof with run 479's inputs (`u18-e8927a74`; both files equal the manifest's):
    - `trx64g.cor` is 3,568,073 B, sha256 `d9e8c537...`, and `cor_pack.py check` passes: kind large, id trx64g, name "TRX64
      Ultimate (Gideon app)", version 0.3.1-dev+e8927a74, records 1, 3, 4 and 5 only.
    - The app is 1,688,216 B, sha256 `d2a9b237...`: `ultimate.app` without its 12 bytes. The bitstream is 1,877,221 B, sha256
      `4d2a74f6...`, the manifest's. The icon is `d521f548...`.
    - MultiCore's own scripts, exported from t239.3 into the scratchpad, pack the same bytes from the same pin. Both forms of
      `SEMVER` give them, and a wrong `APP_SHA256` is refused.
  - Dry run of `ci/cor.sh` (HEAD e8927a74) against a stand-in registry on 127.0.0.1, holding run 479's files and three planted
    stale `.cor` files. trx64u's container build was a stand-in that gave run 479's trx64u file. The run was green:
    - both inputs fetched and checked, and the three stale files deleted;
    - `SHA256SUMS` rewritten with both `.cor` lines; the lines of both `.cor` files and both inputs verify (`shasum -c`);
    - both `.cor` files and `SHA256SUMS` put to `u18-e8927a74` and `u18-latest`.
    - shellcheck is clean at every level on `ci/cor.sh` and on the copies.
  - ue2emu 0.7.4, the packed `trx64g.cor` with `--handover` (its path, id, name and version):
    - From `/SD/cores`, where MultiCore's package puts its cores, on a first start and on a second one:
      - the banner `*** C64 Ultimate (V1.01) 0.3.1 ***`, then "All linked modules" after 1.0 s emulated;
      - the C64 at READY, and `GET /v1/info` 200 (firmware_version 0.3.1, git_commit_hash 0103c58);
      - the menu button opens Gideon's menu.
    - The first start from the card asks nothing. The init of U64 Config writes `/SD/cores/trx64g/trx64g.cfg`, and
      `save_core_cfg` closes the question (`cfg_import_open = false`). So the flash's settings are kept without the user
      deciding, which contract §4 asks for.
    - From `/Usb0/cores`, the firmware looks for the `.cfg` before the stick is mounted:
      - it reports "does not exist yet" even when the `.cfg` is there, and the writes of the init fail ("cannot write", 10
        times);
      - the question goes to `c64UserInterface->popup()` while the C64 is not frozen, so it does not show, and Y is not read
        (`C64::keyboard_scan_allowed` needs `isFrozen`);
      - the start goes on only after two pushes of the menu button. The first push answers No, and the 16-byte `.cfg` of
        defaults is written. The second push closes the note about factory settings, and the menu stays open.
      - With the `.cfg` already on the stick, the next start reports "does not exist yet" again and stops at the question.
  - Licence: of the Commodore ROMs, `trx64g.cor` holds what `ultimate.app` holds (`PROVENANCE.md`, `software/trx64g/README.md`):
    - the BASIC ROM 901226-01, whole;
    - `roms/chars.bin`, 225 of whose 256 glyphs are glyphs of 901225-01;
    - `software/u64/default_chars.bin`, 252 of whose 512 glyphs are;
    - `module.bin`.
    There is no files record.
  - Not exercised: a CI run and a real upload (nothing is pushed or uploaded), and the board.
  - The contract's §1, §2 and §5 for D46 (both sides on Alex's decision): MultiCore's 8ca00d17d (integration), taken byte for
    byte (sha256 `021ea3ff...`): `trx64u.cor` and `trx64g.cor` are built, tested and delivered here as files of one version of
    `trx64-ultimate`; MultiCore's package takes each by its SHA-256.
  - `default_chars.bin` is the part of the character ROM that Gideon's application holds, which MultiCore's licence rule for
    `trx64g.cor` allows (the BASIC ROM, that part of the character ROM, `module.bin`).
  - Run 480 (975aa59b), the first CI-built `trx64g.cor`, delivered: `trx64g_975aa59b.cor`, 3,568,073 B, sha256
    `00368bed...` (records 1, 3, 4 and 5); `trx64u_975aa59b.cor`, 3,984,896 B, sha256 `03239331...` (the same app, libraries,
    record 6, files and bitstream as run 479; only the version and `bit_commit` differ). MultiCore's t239.3 615bb6922 pins
    both, `trx64g.pin` now in the COR form; their ROM check (`check_public_tree --allow-trx`) is clean for both.
    Their 0242d0ca5 then corrected `pack_import_cor.sh`'s header comment (who packs `trx64g.cor` since D46); our copy took it,
    its PROVENANCE names 0242d0ca5.
    `app_commit` stays in trx64g's manifest (MultiCore: it names what the file holds, and their side reads no field of it).
  - Open:
    - a release reuses its branch package, so its `trx64g.cor` shows `x.y.z-dev+<commit8>`, not the `x.y.z` that §2 gives a
      release;
    - the firmware's one-time question "import the settings from /Flash?" (`ultimate.cc`, after the UI is up) fails under
      MultiCore's base, seen in ue2emu and followed in the code:
      - from the card: an init write of a page (`ConfigPage::write`, `config.cc`) calls `save_core_cfg(false)`, which writes
        the whole `.cfg` and clears `cfg_import_open`, so the flash settings are taken before the question is asked;
      - from the stick: the `.cfg` is looked for before the stick is mounted and every write fails; the question goes to
        the C64 menu while the machine is not frozen, so it is not shown and no key is read (`C64::keyboard_scan_allowed`
        needs `isFrozen`); the first menu-button push answers "no" and writes the defaults `.cfg`.
      MultiCore's coreapp waits for the `.cor`'s medium (`mediumwait.h`); Gideon's application does not. Ours to fix
      (MultiCore: noted as ours); `software/ultimate-app` is not changed yet.
- **2026-11-24: MultiCore's core app guide reworked, in their repository (Alex).**
  - Alex: "Build an md page on how a core gets its service and its app with the SDK, as a help for MultiCore", then, on
    MultiCore's `docs/dev/core-app-guide.md`: "basically that needs to be reworked", and "agree that you may store it
    there directly".
  - Agreed with MultiCore: our own worktree of their repository, branch `fw/core-app-guide` from their t239.3; only the
    guide; their commit rules (no Claude trailers, `tools/check_todo.py`, a public text: no hosts, IPs, registry URLs,
    logins or private paths); they merge it into t239.3, and integration gets it with Alex's PR.
  - Committed there as c765b8f3f on t239.3 e7bed3e93: 814 lines, +507/-107; `check_todo` ok, `check_public_tree`
    (`--commits`, `--messages --trailers`) clean. The guide is read off their t239.3 (0.4.6 parts B, C, D) and off
    trx64u. Sections 1-12 are theirs, corrected from the code; sections 13-26 are new: the SDK core path from pin to
    delivery, with trx64u and trx64g as the worked example.
  - Sent to MultiCore with eight findings for their side: two lines of the SDK README, `lib.mk`'s `RV_PREFIX` default,
    `DRV_BASE_READ_ONLY` not set by `lib.mk`, contract §10's stale "today in MultiCore's coreapp/src", `SVC_HAS` and
    `drv_fake.h` in the SDK headers, TOML A names in record 6's register numbers, and P2-6's replacement of
    `lib->find`.
  - Ours, open: trx64u's `.cor` carries the version `<branch>-<sha8>`, while contract §2's shown form is
    `x.y.z-dev+<sha8>`, as `trx64g.cor` has it.
  - `software/trx64u/README.md`'s record 8 note was stale (the base installs record 8 before it scans `apps/`):
    corrected.
  - MultiCore's fact check of it followed (21 wrong statements, mostly of their sections 1-12, and the 0.4.6 state on
    t239.3 e61ed6b88). Applied on the branch as 21faec901, checked in the code; where the code said otherwise than their
    list, the guide follows the code (the host app checks the contract, `all`, the volume keys, the controls), and six
    more differences were found. `rtl_check` of e61ed6b88 passes c64_basic's whole TOML A (39 blocks, 168
    properties, re-run here). MultiCore merged the branch into t239.3 (6081237dd) and fixed the comments the guide's
    §12 named in their code. Our worktree there is removed.
- **2026-11-24: contract §10 rewritten by both sides on Alex's go ("Yes, you may!", translated, given to MultiCore).**
  - Found while reworking MultiCore's core app guide: §10 still named T230's target split, in which MultiCore would build
    trx64u's features as generic modules ("today in MultiCore's `coreapp/src`").
  - The new §10 is the split of T239.3 D21/D44: we own the bitstream, TOML A and B, the menu and the code of the
    features (Timewarp ring, monitor, U64 registers, UCI), built with MultiCore's SDK. MultiCore owns the host, the base
    and the SDK; its package takes `trx64u.cor` and `trx64g.cor` by their SHA-256.
  - Our two changes to MultiCore's draft:
    - "the streams" is out: the data streams are the base's code (MultiCore's `coreapp/src/netops.cc`), and our
      libraries have no stream code;
    - both `.cor` files are named.
  - The contract is MultiCore's 2f1d1ccd4 (integration), byte for byte, sha256 `4cd4dc89...`.
- **2026-11-24: SDK 0.4.6 pinned (MultiCore t239.3 6e280710, base 3.4, a dev build).**
  - The tarball `trx64-sdk-0.4.6.tar.gz` (sha256 `fd3175de...`) with the VERSION MultiCore named (API headers `22fd021d...`, base archive
    `65dc7f11...`). The online sub-app is the build of the same commit (`online-6e280710.u2a`, `061c7394...`): 0.4.6's `OnlineApi` has
    `loading` and `loading_for` (D41 L-3c), and the base takes the table only at that size (`netfs.cc` :83-88), so the 8ff28ba9 build would
    give no folder `/Online` on 0.4.6. Both are in our registry (`trx64-sdk/0.4.6-6e280710`, `trx64-online/1.0-6e280710`), checked by an
    anonymous download.
  - What broke, and what changed:
    - The ring, at its three `c64cart.cmd` writes (no `_WR` for a base block), as D50 said. They are gone: in 0.4.6, c64cart's
      `cart_ram_write` (`c64cart_exports.cc` :45-51) drops the cached lines after each write in range, and `c64_cart_image_write`
      (`c64_cart.cc` :809-815) after each write while a cartridge is in. The ring is 136,232 + 16,860 = 153,092 bytes, its budget lowered to it.
    - UCI (D48): the base has neither the server (`cmd_if`, `command_targets`; `riscv_main.o` keeps weak `command_interface_irq` and
      `ResetInterruptHandlerCmdIf`) nor the headers `command_intf.h`, `rtc.h`, `subsys.h`, `endianness.h`, `palette_command.h` and `http_codes.h`.
      `uci/` takes the hand-over (`command_intf.cc` and `.h`, `command_target.h`, `network_target.cc` and `.h`) and the six headers from our
      firmware submodule, byte for byte MultiCore's at 6e280710, each with a PROVENANCE header. The TOML B's library `uci` names the two new
      sources.
    - The server's registers come from the driver table. The TOML A's `cmd_if` lists its own properties (MultiCore's map `uci` has the two
      slot registers; MultiCore: the block is ours): the 16 registers of `command_protocol.vhd` with the directions of the RTL (+4 and +5 are
      `command_start` and `command_end` read, `irq_mask_set` and `irq_mask_clear` written) and the run `buffer` (+0x800, 2,048 bytes); its size
      is 0x1000 (was 0x10). `uci/command_intf.h` maps the firmware's names to `DRV_cmd_if_*`.
    - `base/ops_trx64u.txt` gets `writeback.flush` (D51), 393 lines. trx64u, monitor and trex_crt build unchanged; trx64u's image is the same.
  - Checks, local (the Mac and the build container):
    - `pack_cor.sh --emu`: rc 0, `trx64u.cor` 3,987,047 bytes, READY 3.6 s, 4 libraries loaded, 0 refused, the operations and states equal
      to the golden.
    - UCI in ue2emu: `settings.set` "Command Interface (UCI)" = Enabled over REST, then ue2emu's `monitor dir` of `/`, `/Usb0` and
      `/Usb0/cores` (the DOS target over the UCI block) is answered by our server.
    - `rtl_check lint-src base ring monitor uci`: no literal I/O address in 70 files.
    - `rtl_check check` of 0.4.6 on the whole TOML A: every block holds but one property, "block cmd_if, property command_start (dir r):
      byte +0x4 (0x10044004) has no read in the RTL". `command_protocol.vhd`:263 decodes that read and drives
      `c_cmd_if_command_buffer_addr(10 downto 3)`, the constant 0 (`command_if_pkg.vhd`:33); the same pattern with constants other than 0
      (+5 to +9) passes. MultiCore: the tool's fault (`rtl_check.py`:3784 counts a read only when its answer is not the constant 0, which
      keeps an undecoded address's default answer out and loses a decoded arm that answers 0); the fix is open in their T239.3 §5 and comes
      with a later SDK. Until then `check` stays out of our lint.
  - D52 (Alex, 2026-11-24, as MultiCore recorded it on integration c7e5ce8a7; translated; replaced by D53 below): "The ITU registers must be writable." The
    handed-over server writes the base's ITU itself through `itu.h` (`ITU_IRQ_CLEAR` and `ITU_IRQ_ENABLE` for the C64 reset, `ITU_IRQ_ENABLE`
    for the command interface, `ITU_IRQ_DISABLE` in its destructor), which until 0.4.5 was the base's own code, and the base has no call for
    an interrupt. A core's own library writes the registers of `itu`, an exception to D50; the server stays as it is. The code behind it:
    the enable and disable registers set and clear only the bits written (`itu.vhd`:136-139); the base enables the timer alone, "other
    modules shall enable their own interrupt" (`riscv_main.c`:185, MultiCore 6e280710). Open for the next SDK: the driver table gives a
    core's library `itu`'s `_WR` macros too (0.4.6 puts them inside `#ifndef DRV_BASE_READ_ONLY`).
  - CI run 481 (82fe1b90) green in every stage; delivered to MultiCore (D45): `trx64u_82fe1b90.cor`, 3,987,048 bytes, sha256
    `b4e184bd...`, and `trx64g_82fe1b90.cor`, 3,568,073 bytes, sha256 `ffcd38c2...`, both checked against the package's SHA256SUMS.
    MultiCore's pins take them (t239.3 bff5d6730, the values checked there).
  - Not exercised: the board, a UCI command from a C64 program, the NETWORK target and CONTROL.
- **2026-11-24: T239.3 D53 (Alex) replaces D50 and D52: every source writes every block.**
  - As MultiCore recorded it (integration ccad7aa43; translated): "In the TOML I define: my core supports UCI on these registers etc. That
    must then of course be writable. ... At build time this becomes one app. So why protection etc. Out with it. Everything is open."
  - On MultiCore's t239.3 84acf5130 (pushed; SDK_VERSION 0.4.7, base 3.4, no package yet): `descrec.py` and `lib.mk` no longer have
    `DRV_BASE_READ_ONLY` (6e280710's `descrec.py` had it three times), so `drvtab.h` gives every source the `_WR` macros of every block,
    the base's too (`itu`, `c64cart`).
  - Our code needs no change. The ring keeps its three `c64cart.cmd` writes out: c64cart still drops the cached lines after `ram_write` and
    `image_write`. The UCI server's ITU writes are allowed. With the 0.4.7 pin, `sdk.pin`'s note on the read-only macros goes.
