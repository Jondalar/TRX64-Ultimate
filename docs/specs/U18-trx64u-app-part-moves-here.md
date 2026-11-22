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

### Under discussion: the trx64u tests leave MultiCore's pipeline (Alex, 2026-11-20)

Alex (translated): "Lint, apptests and emu tests must then also run separately in the pipeline, *out* of MultiCore's big
monolithic pipeline and over to you. MultiCore then does not have to do all of that again."

What MultiCore's push pipeline runs for trx64u today (`ci/children.txt`, read in `main` 7213212a2):

| Stage | Where | The trx64u part |
|---|---|---|
| Lint | `ci/lint.sh`, a step of the parent pipeline | "TRX64U menu record" (`tools/gen_menu_rec.py --check`, :80), "TRX64U menu against the UX concept" (`tools/check_menu_concept.py` on `tests/coreapp/golden/menu_c64.txt`, :84), the import `.cor` pack test (`tools/test_pack_import_cor.sh`, :75) |
| Apptests | child `apptests` (`apptests.yml`, `ci/apptests.sh`, `tests/coreapp/run.sh`) | 18 test sources under `tests/coreapp` name trx64u. `tests/coreapp/cores.txt` has no trx64u group (its groups are files of `coreapp/cores/`; `trx64u_ops.cc` is in `coreapp/src`), so these units fall into the shared suite or `cross`: a trx64u change reruns MultiCore's shared core-app suite |
| Emu | child `emu` (`emu.yml`, needs `images`), suite `coreapp_emu` | `tests/coreapp/emu/run.sh` and `library_n.sh` on `coreapp_trx64u.app` (T228 R25) in ue2emu with `--trx-io on`, job image `u64-build-env:emu-v0.7.0` |

The wish: three stages of our own here (lint, apptests, emu), each keyed on our files; MultiCore's pipeline drops the
trx64u units and does not run them again. Open:

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
