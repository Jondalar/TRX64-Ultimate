# U18 — trx64u's application part moves here: a library on MultiCore's app core

**Status:** PROPOSED (2026-11-20). Nothing built; the questions in §3 are open, the requests to MultiCore in §3 are not
sent.
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
| S1 | P2 | `trx64u.u2a` builds here from the SDK alone, within the loader's limits, and the app core image built for the other cores holds no trx64u symbol | our CI; MultiCore T239.3 S8 |
| S2 | P2 | On the board trx64u starts, lists its menu, answers RPC on TCP 4312 from the start, runs a TREX CRT dump, a drive mount and Timewarp scrub | board step with Alex (the T228 §5 checklist plus RPC and Timewarp) |
| S3 | P3 | One trxmon source: our firmware's trxmon and the trx64u library build from the same files | build of both; no copy left in MultiCore |
| S4 | P4 | Our CI runs the trx64u menu and ops tests (golden ops, menu simulator) without a board | CI stage |
| S5 | P5 | Contract, ADR-0057 and U3 say the same as T239 | `diff` of the two contract copies; review by Alex |

## §3 Decision

Nothing decided here yet. Applies from MultiCore: T239 D9, D10, D13; T239.3 D1 (libraries declare their budget and load
with the app core; sub-apps load on demand), D2, D5.

### Open questions for Alex (one at a time)

1. **Until the registry exists, what does our package deliver?** (a) the complete `trx64u.cor`, built here with a pinned
   app core image, or (b) the pieces (`trx64u.u2a`, menu record, files), packed by MultiCore as today. Proposed: (a); it
   is already the shape the registry needs (T239 D5, D9), and MultiCore pins one file.
2. **Is trx64u the future firmware of TRX64 Ultimate itself (U3, ADR-0057), or only the MultiCore core?** If U3 stands,
   our standalone firmware becomes app core + trx64u library, and Gideon's app stays only as `trx64g`. If not, we keep
   two UIs here (Gideon's app and the trx64u menu) and every feature goes into both.
3. **Who implements `SvcTrx`, `SvcRing` and `SvcC64` after the move?** (a) the trx64u library exports them
   (`SvcLib`), (b) the app core keeps them. (a) follows D24, but it changes how trex_crt and trxmon find them.
4. **One trxmon source here:** does MultiCore's copy go away, so that the two drifted headers are merged once, here?
5. **One library or two** (trx64u and the monitor engine): decided after the size measurement (§5, prep 3).

### Requests to MultiCore for T239.3 (not sent; with Alex's go)

- **R1** The SDK release ships the app core image (`coreapp.bin`) with its version, so that a large core can be packed
  outside (§1 item 5).
- **R2** The library poll (T239.3 S3) is served before trx64u moves (P4); the RPC server needs it from the start.
- **R3** The loader runs `.init_array`, or `lib.mk` refuses a library that has one (§1 item 3).
- **R4** T239.3 P1 states who implements the TRX service tables after D2 (question 3).
- **R5** The trx64u share of today's app image is measured against `U2A3_IMAGE_MAX` before P4 is cut.
- **R6** The golden ops file, `descrec.py` and the menu simulator are in the SDK, so that our CI tests the menu.
- **R7** An ABI major change is announced before release, so that trx64u is rebuilt before a package ships it.
- **R8** Contract §1, §8 and §10 are rewritten together with this move; ADR-0057 point 6 is replaced.

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
