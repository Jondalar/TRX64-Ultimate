# TODO

## Overview

The one list. IDs never change; Alex sets the order and drops items, Claude
only changes the status. The table is an overview, the details are in the specs
(docs/specs/), in docs/todo/<ID>.md and in docs/.

**How this file is kept (since 2026-10-07):**
- The tables are an overview only: ID, status, a short title, one line saying what is still missing ("Open"), and a link to the details. No histories, board results, CI numbers or decision texts in a row.
- Details live in the row's spec (section "Status and history", dated entries, newest first); a row without a spec has `docs/todo/<ID>.md` (title, description, status and history). Create the file when the first detail comes up.
- Decisions live in [docs/adr/](docs/adr/README.md), one file per decision (`NNNN-short-title.md`, Michael Nygard format; Alex's words quoted). A changed decision gets a new ADR and the old one is set to "Superseded by NNNN". Rows and specs link to the ADR instead of repeating it. Old "decision N" references resolve through the "Formerly" line of the ADR.
- **Numbering (Alex, 2026-10-22):** TRX64U-MultiCore counts T numbers on its own, so the two lists collided (T112 here and there, T116). T numbers up to T116 stay as they are; T112 here is the ESP32 Wi-Fi recovery, not MC's T112. New items in this repo are U1, U2, ...
- Every result, board test or decision updates the detail file (and the ADR if it is a decision); only when the one-line "Open" changes does the row change. Status changes stay with Alex's calls as before.

Status: 🔨 in progress · ⏭ next · ⏸ later · ✖ dropped · ✅ done · ❌ won't do · 📅 planned

Order set by Alex (2026-10-02): T20 -> T21 -> T28 -> T33. **Done: all four are built and in main (2026-10-06); what is still open sits in their rows (T20, T21).**

**No milestones any more (Alex, 2026-11-07; [ADR-0066](docs/adr/0066-milestones-end-with-the-multicore-split.md)):**
M1, M2 and M3 ([ADR-0003](docs/adr/0003-milestones-m1-m2-m3.md)) are retired. M2's content and M3's multi-core
work went to `TREX/TRX64U-MultiCore` ([ADR-0056](docs/adr/0056-trx64-ultimate-is-the-monolith.md),
[ADR-0057](docs/adr/0057-generic-core-app-replaces-gideons-app.md)), M1 closed with RC1, and the M2 exit
criterion (the e2e tests on Robot Framework, T71, [ADR-0052](docs/adr/0052-robot-framework-bdd-tests.md)) went
with it: that migration is T71's goal, not a gate. The sections below are named after what they hold, the
status icons carry the order, and an **M-word in an older ADR, spec or history entry reads as "deferred, not
now"**.

Where we are (2026-10-09; older entries in [docs/todo/overview-history.md](docs/todo/overview-history.md)):
- **Goal:** drop-in C64U replacement + TRX64 monitor + Timewarp ([ADR-0016](docs/adr/0016-back-to-the-drawing-board.md)); no 100T, no public release ([ADR-0015](docs/adr/0015-no-100t-no-public-release.md)).
- **The drop-in line:** all its rows closed 2026-10-10; RC1 (tag `m1-rc1`, integ-m1 692f643d, release `trx64-integ-m1-692f643d`) installed on the board and handed to the testers. Next: T77/T78 (monitor hang, WiFi RPC timeout), then T75, T76 and the ⏸ rows.
- **Board session 2026-10-10 (Alex):** done: T62 human checks, T16 EF1/MegaByter/GMod2/EF3, T57 B1-B11, T74 (MegaByter + Gideon's core measured), T49 restore (settings NO and YES). RC1 installed. Checklist: [docs/AlexTest101026.md](docs/AlexTest101026.md).
- **Board:** the flash holds RC1 (integ-m1 692f643d, app 3c8e4aa); EF3 in the port. Board tests: [docs/board-tests-pending.md](docs/board-tests-pending.md).

## Done: the drop-in line and the TRX64 functions

| ID | Status | Title | Open | Details |
|---|---|---|---|---|
| T01 | ✅ | Flash read-only for the app (settings, real MAC, ROMs) | – | – |
| T02 | ✅ | Overlay: Gideon's menu on HDMI | – | – |
| T03 | ✅ | SD card | – | – |
| T04 | ✅ | Test the rocker (menu/reset/freeze), needs T02 | – | – |
| T05 | ✅ | IEC, 1541 emulation, start D64 (directory and LOAD work on the board) | – | – |
| T06 | ✅ | Start CRT: cartridge types, ROM from DDR2 | Freezer buttons, GMod2 EEPROM etc. | [T06](docs/todo/T06.md) |
| T07 | ✅ | USB (hub and USB stick work on the board) | – | – |
| T08 | ✅ | Case and board LEDs | – | [OPT-bling](docs/specs/OPT-bling.md#t08) |
| T09 | ✅ | Keyboard LEDs (Bling), SHIFT LOCK state and its LED | – | [OPT-bling](docs/specs/OPT-bling.md#t09) |
| T10 | ✅ | Lorenz failures (CPUPORT, TRAP, CIA timers) | – | [T10](docs/todo/T10.md) |
| T11 | ✅ | Fast DDR2 path; 1541 and cart ROM out of block RAM | – | [T11](docs/todo/T11.md) |
| T12 | ✅ | Freeze: full-core pause, hardware menu text, cartridge peek | Freeze without HDMI -> T35 | [T12-freeze-overlay](docs/specs/T12-freeze-overlay.md#status-and-history) |
| T13 | ✅ | Audio and debug stream | Debug stream (2/3) not built (gap G15) | [T13](docs/todo/T13.md) |
| T14 | ✅ | HW I2C for the app | – | – |
| T15 | ✅ | Boot from flash without JTAG | – | [T15-flash-boot](docs/specs/T15-flash-boot.md#status-and-history) |
| T16 | ✅ | Cartridge port with Alex's real carts | – | [T16-cartridge-port](docs/specs/T16-cartridge-port.md#status-and-history) |
| T16.2 | ✅ | Ultimate app side for writable cartridges | – | [T16.2-app-cartridge-writeback](docs/specs/T16.2-app-cartridge-writeback.md#status-and-history) |
| T17 | ✅ | Test the built-in speaker | – | – |
| T20 | ✅ | Delta ring, rewind/scrub (Timewarp) | Done 2026-10-09 ($01F2 fix and `snap` bench passed on the board 2026-10-08, in main); UI extras -> T54 | [T20-delta-ring](docs/specs/T20-delta-ring.md#t20) |
| T21 | ✅ | Monitor at TRX64 level (trxmon) | – (rest moved to T55) | [T21-monitor](docs/specs/T21-monitor.md#status-and-history) |
| SLIM | ✅ | Slim state path (replaces the freeze session) | freeze_raster and monitor golden not individually reported | [SLIM-state-path](docs/specs/SLIM-state-path.md#status-and-history) |
| OPT-ddr2 | ✅ | Own lean DDR2 controller instead of MIG | – | [OPT-ddr2](docs/specs/OPT-ddr2.md#status-and-history) |
| OPT-pix | ✅ | Slimmer pix_layer | – | [OPT-pix](docs/specs/OPT-pix.md#status-and-history) |
| OPT-mig | ✅ | MIG without XADC, 2 bank machines | – | [OPT-mig](docs/specs/OPT-mig.md#status-and-history) |
| OPT-kb | ✅ | c64u_keyboard 721 -> 294 LUT, formally equivalent | – | [OPT-kb](docs/specs/OPT-kb.md#status-and-history) |
| OPT-rvmul | ✅ | rvlite register file -> multiplier path pipelined | – | [OPT-rvmul](docs/specs/OPT-rvmul.md#status-and-history) |
| T25 | ✅ | SID: Dag Lem's reDIP at UltiSID level (8 SIDs), mixer, sampler | T25.6: the status mask (ADR-0069) built 2026-11-14, board test `U12-UA972` open | [T25](docs/todo/T25.md) |
| T28 | ✅ | UCI (Ultimate command interface at $DF1C-$DF1F) | – | [T28-uci](docs/specs/T28-uci.md#status-and-history) |
| T29 | ✅ | Turbo: CPU up to 63 cycles per C64 cycle | – | [T29-turbo](docs/specs/T29-turbo.md#status-and-history) |
| T33 | ✅ | Generic cartridge decoder per Alex's mapping descriptor | – | [T33-generic-cart-decoder](docs/specs/T33-generic-cart-decoder.md#status-and-history) |
| T59 | ✅ | Custom palettes as in Gideon's core | – | [T59-palette](docs/specs/T59-palette.md#7-status-and-history) |
| T60 | ✅ | REU as in Gideon's core, up to 16 MB | Gideon's firmware with a cartridge on our bitstream (ROM_SEL 0) not reported; EF3 needs the window widened (T51) | [T60-reu-16mb](docs/specs/T60-reu-16mb.md#status-and-history) |
| T62 | ✅ | Small drop-in gaps from the gap audit (one RTL bundle) | – | [T62](docs/todo/T62.md) |
| T61 | ✅ | Wall clock in software (gap audit G05) | – | [T61](docs/todo/T61.md) |
| T63 | ✅ | App DMA into I/O ($D000-$DFFF) while the C64 runs (G06) | – | [G06-dma-cycle-steal](docs/specs/G06-dma-cycle-steal.md#status-and-history) |
| T67 | ✅ | App registry keeps the old path when a .u2a file is replaced | – | [T67](docs/todo/T67.md) |
| T38 | ✅ | Keyboard matrix and joystick injection from the app | Paddles/mouse, WASD-to-joystick, port swap -> T47 | [T38-T39-input-reset](docs/specs/T38-T39-input-reset.md#t38) |
| T39 | ✅ | C64 reset sense interrupt for the app | Port's P20 reset sense ORed in with T16 | [T38-T39-input-reset](docs/specs/T38-T39-input-reset.md#t39) |
| T40 | ✅ | NTSC: PAL/NTSC/PAL-60/NTSC-50 from the app's System Mode | Lorenz in NTSC on the board (B6); M1-M7 against Gideon's core; Till's monitor | [T40-ntsc](docs/specs/T40-ntsc.md#status-and-history) |
| T49 | ✅ | Update packages (.cfw/.ue2 + the apps) for internal testers | – | [T49-update-packages](docs/specs/T49-update-packages.md#status-and-history) |
| T53 | ✖ | UE2 build (XC7A100T) as a second CI bitstream | – (dropped) | [T53-ue2-build](docs/specs/T53-ue2-build.md#status-and-history) |
| T57 | ✅ | trex_crt (dump / write / inspect) 100 % on both cores | – | [T57-trex-crt-both-cores](docs/specs/T57-trex-crt-both-cores.md#status-and-history) |
| T72 | ✅ | Quick fix: REU, Ultimate Audio and UCI off while a cartridge runs (physical or .crt) ([ADR-0053](docs/adr/0053-no-internal-io-devices-while-a-cart-is-active.md)) | – | [T72](docs/todo/T72.md) |
| T74 | ✅ | Unmapped I/O reads return the VIC's phi1 byte (open bus as on a real C64) | – | [T74](docs/todo/T74.md) |
| T31 | ✅ | Multi-stage CI (Gitea job graph) | `ULTSYS_NEGATIVE=1` self-check prints PASS; lorenz/sid/turbo builds still use --public-flat-rw; `release.yml` input `ref` collides with the dispatch form's branch field; tier 2 moves out of the push into a nightly as at MultiCore (Alex 2026-11-16, not built) | [T31-ci-pipeline](docs/specs/T31-ci-pipeline.md#status-and-history) |

## The monolith: the open work

**Decoupling moved (2026-10-16, Alex: "the decoupling, softboot and so on, that is MultiCore"):** T24, T32, T36, T48, T52 live in
`TREX/TRX64U-MultiCore` ([ADR-0056](docs/adr/0056-trx64-ultimate-is-the-monolith.md)). This repository is the monolith: Gideon's core plus
ring, Timewarp, monitor and SID sockets, with Gideon's app until the generic core app replaces it ([ADR-0057](docs/adr/0057-generic-core-app-replaces-gideons-app.md),
2026-10-18: built in MultiCore `app/`/`coreapp/` as T104, driven from here; day-1 list in the ADR).

| ID | Status | Title | Open | Details |
|---|---|---|---|---|
| T30 | ⏸ | Design document (C4, sequence diagrams, Mermaid) | Not written | [T30](docs/todo/T30.md) |
| T112 | 📅 | ESP32 Wi-Fi recovery (Gideon's module 1.18) as our safety net: analysis | DECIDED: back to Gideon; wait for his 1.18 release, then S1/S2 | [T112](docs/specs/T112-esp32-wifi-recovery.md) |
| T35 | ⏸ | Test session at Alex's: tape port, tapecart, user port, freeze without HDMI | Not built (ADR-0047) | [T35](docs/todo/T35.md) |
| T37 | 🔨 | Real SID chips in the C64U's SID sockets | On main since 2026-10-18, all questions answered; not yet on the board; ADC slot per socket needs a real SID | [T37-sid-sockets](docs/specs/T37-sid-sockets.md#status-and-history) |
| T41 | ⏸ | Faster app CPU memory path (line buffer) | Re-measure the latency first (MIG is gone) | [T41](docs/todo/T41.md) |
| T54 | ⏸ | Timewarp UI extras (from T20) | Not built | [T54](docs/todo/T54.md) |
| T55 | ✅ | Monitor extras (from T21) | Done 2026-10-09 (script + Alex at the keyboard); counters still decimal (question); full 1541 VIAs out (budget) | [T55](docs/todo/T55.md) |
| T56 | ⏸ | Drives as in Gideon's core (mm_drive, drive B, 1571/1581) | Spec first; trial synthesis for LUT/BRAM numbers | [T56](docs/todo/T56.md) |
| T58 | ⏸ | Tape port in our core (Datasette and tapecart) | Not built; spec first | [T58](docs/todo/T58.md) |
| T66 | 🔨 | Timewarp ring and .c64re snapshot save/load, done properly | all nine parts built and compiling (app CI 244); the board test is Alex's, in docs/board-tests-pending.md | [T20-delta-ring](docs/specs/T20-delta-ring.md#t66) |
| T73 | 📅 | Internal IO devices and cartridge IO usage: spec ([ADR-0053](docs/adr/0053-no-internal-io-devices-while-a-cart-is-active.md)) | Spec first | [T73](docs/todo/T73.md) |
| T75 | 📅 | REU register window and turbo colour RAM read as on Gideon's core | Not built | [T75](docs/todo/T75.md) |
| T76 | 📅 | R21 (`CIA_RSTn`): what Gideon's design resets with it | Not measured | [T76](docs/todo/T76.md) |
| T77 | 🔨 | trxmon over RPC: `bk` + `g` hangs the app (RC1) | WiFi part in T78 | [T77](docs/todo/T77.md) |
| T78 | 📅 | WiFi RPC replies with a timeout (`wifi_cmd.h` `TRANSMIT`) | Spec written, not built | [T78-wifi-rpc-timeout](docs/specs/T78-wifi-rpc-timeout.md#status-and-history) |
| T80 | 📅 | Lean ring engine: core passes data through, the app works on the ring (also for the drop-in line) | S1 (APPLY switch) decided, being built; S2-S4 open | [T80-lean-ring](docs/specs/T80-lean-ring.md) |
| T82 | 📅 | VIC-II: name the cause of the raster flicker against Gideon's core, then slim kawari (2,117 LUT) | Spec written, not started | [T82-vic-check-and-slim](docs/specs/T82-vic-check-and-slim.md) |
| T95 | 🔄 | Descriptor-driven cartridge tool: new cartridge = one JSON, UI cut from the logic (TwoMegabyter is the first user) | D1+D2+D5 done: 134 geom checks, harness 34541/0 with the traffic unchanged (S1), cores link and run without a view (S5, 12 checks); next D3/D4, then D6. Q1-Q3 open | [T95-descriptor-driven-cart-tool](docs/specs/T95-descriptor-driven-cart-tool.md) |
| T71.1 | 📅 | Robot Framework tests: PoC (same scenario on ue2emu and board) | Not built; register list (Q1) open | [T71-robot-tests](docs/specs/T71-robot-tests.md#71-poc--t711-in-m2) |
| T71 | 📅 | Robot Framework tests: MVP in the pipeline, then full migration of the e2e suites ([ADR-0052](docs/adr/0052-robot-framework-bdd-tests.md); no longer a gate, [ADR-0066](docs/adr/0066-milestones-end-with-the-multicore-split.md)) | Not built; after T71.1 | [T71-robot-tests](docs/specs/T71-robot-tests.md#7-plan-and-acceptance-criteria) |
| U1 | 📅 | trex_crt: Erase Cartridge in the F5 menu; the writer stages the CRT on our core (USB stick drop-off) | Scope decided, not built | [U1](docs/todo/U1.md) |
| U2 | 🔨 | User manual (docs/manual): every menu, function and app of the release | Written from code + emulator; board pictures open | [U2](docs/todo/U2.md) |
| U3 | 📅 | Generic core app replaces Gideon's app (ADR-0057): gap analysis, order, decisions | Spec written; 10 decisions open; nothing built here | [U3](docs/specs/U3-generic-app-replaces-gideons-app.md) |
| U4 | 🔨 | From Timewarp into the monitor (C= + O) at the frame you are on | board test open; the way back (monitor -> Timewarp) is out of scope | [U4-timewarp-to-monitor](docs/specs/U4-timewarp-to-monitor.md#status-and-history) |
| U5 | 🔨 | The stops move into the firmware: observer service, RPC server and monitor | built; board tests 1 and 3 of §7 passed 2026-11-04, four open; the dbgsess case runs first in CI | [U5-observer-service](docs/specs/U5-observer-service.md#status-and-history) |
| U6 | 📅 | The gateware carries only what is built in: our unbuilt SID engine, the unbuilt reDIP variant and whatever else the build reads but never instantiates | Not started; costs one Vivado run, so it rides with the next gateware change | [U6](docs/todo/U6.md) |
| U7 | 🔨 | The SID filter keeps the bits it needs: fractional state and rounded shifts (reDIP loses integrator steps at a low cutoff) | Built, measured (-32.6 -> -75.4 dBFS worst case, no new DSP) and the reDIP suite is green; Vivado numbers and board hear test open | [U7-sid-filter-precision](docs/specs/U7-sid-filter-precision.md#9-status-and-history) |
| U8 | 🔨 | `.c64re` format 2: state and medium in one binary file, the cartridge once, read and written streaming (the old file was 93 % cartridge and could not be loaded back: "Out of memory") | Board 2026-11-07: the write is right (1.18 MB beside the image, cartridge once), the load refused it - `%lu` in the header, which this printf does not expand, shifted `blockWords` onto `caps2`; fixed, 13 format tests green, firmware `u8dev2` on the board, U8-LOAD to repeat | [U8-c64re-v2-state-and-medium](docs/specs/U8-c64re-v2-state-and-medium.md#9-status-and-history) |
| U9 | 🔨 | Loading a state puts the medium back in: the cartridge is inserted, the disk mounted, the settings it needs set for as long as it runs | Built 2026-11-08: D0 gate at save time, GMod2 + its EEPROM section, machineState in the header, insert without reset, mount, settings override that survives Reset and ends at Reboot, type check back, 17 format tests green. D3b narrowed while building: without its .crt the load refuses (Q5). D3c 2026-11-08: a medium whose own path is not there is taken from beside the .c64re by file name. D3d 2026-11-08: when it is in neither place the load asks and a modal picker (PR 662) takes the answer. Board tests U9-* open | [U9-state-puts-the-medium-back](docs/specs/U9-state-puts-the-medium-back.md) |
| U10 | 📋 | The ring in a file: `Save Ring` / `Load Ring` in the firmware, the medium once | Spec written, all four questions answered with Alex 2026-11-08: whole window (~18 MB), `.c64rering` keeps its name, a foreign bitstream refuses the whole file, `ringdump` goes and `ringsave` writes U10. Not built | [U10-ring-in-a-file](docs/specs/U10-ring-in-a-file.md) |
| U11 | 🔨 | A 16 bit sample starts at any byte address: the sampler took the pair at the even address below, and a sample across the DDR2 word was not assembled | Built 2026-11-09 from issue #2 (tbybell): offsets 0-2 from one read, offset 3 in two with atomic publish, `fkill` over both; `tb_sampler` section 10 + `tb_stress` on any byte green, old benches unchanged; LENGTH/REPEAT keep bit 0 dropped (D5) and the interleaved strides are deliberately not built (§4). Board hear test `U11-HEAR` open | [U11-sampler-any-byte-address](docs/specs/U11-sampler-any-byte-address.md) |
| U12 | 🔨 | The sound of a 6581: the filter's non-linearity, its distortion, the chip model reaching the filter and the bits Mahoney material needs | Built into reDIP, the release engine (ADR-0071): the 6581's op-amp curve on the filter output, 0.932 -> 0.962 against libresidfp, benches green, Vivado routed (WNS +0.018). Open: board, D4 (Mahoney trace via c64re, U15); reply to Tony only after a joint listening test | [U12-the-sound-of-a-6581](docs/specs/U12-the-sound-of-a-6581.md) |
| U13 | 🔨 | The monitor reads $D000-$DFFF as the CPU would: one decode (the arbiter's) for the bus and the lens unit's look, every chip's read view, no bus cycle (`m d031` showed `FF` for a written `$0E`, MetalDust) | Built 2026-11-13: `io_decode()` shared and proven equal (yosys), turbo/SID/sampler read functions shared and proven equal, look of the whole I/O page (CAPS3 b1), UCI through its slot address; `tb_statebus look` (4096 addresses against bus reads, side effects seen, 0 cycles) and `tb_uci_core` (UCI stopped and running) green, both in CI tier 2; slot look/socket tests green. Board test `U13-MON` open | [U13-the-monitor-reads-io-as-the-cpu](docs/specs/U13-the-monitor-reads-io-as-the-cpu.md) |
| U14 | 📋 | The hard combined waveforms: Wonderland 13 and 14 as benches against libresidfp, before the tables are touched | From U12 D6 (Q1, 2026-11-14); nothing built | [U14](docs/todo/U14.md) |
| U15 | 📋 | Register traces of real tunes: a `.sid` turned into the log `tb_wav` and `filt_cmp` play | Source: the c64re runtime (Alex 2026-11-14); first trace (Mahoney, Musik Run/Stop) plays through `tb_cmp` and `filt_cmp`; the exporter is not in the repo yet | [U15](docs/todo/U15.md) |
| U16 | ✅ | More HDMI modes (576p, 480p, 720p, 800 x 600 beside 1080p, locked to the VIC frame, chosen by the application) and the picture's scale (Integer / Fit / Fill) | Done 2026-11-15 (board at 50 Hz, Alex); not tested: 60 Hz rows, projector; open: host tests not in CI | [U16-hdmi-modes](docs/specs/U16-hdmi-modes.md) |
| U18 | 🔨 | trx64u's application part moves here: libraries on MultiCore's app core (MultiCore T239.3) | Branch `u18`: SDK 0.4.6 pinned (UCI server in `uci/`, D48; the ring's `c64cart.cmd` writes gone, D50); trx64u's app linked here from the SDK (TOML A, TOML B, symbol check), heap word 0 (P4-1), `lint-src` in lint; P2-5 (`.cfw` run, `prg.*`, trex_crt sub-app; no Softboot in trx64u, Alex), Online sub-app in record 7, ops checked on the running app (`pack_cor.sh --emu`, ue2emu READY); ICAP loader in c64_basic (Alex, MultiCore D37: RTL, bench, CI run 475 green; TOML A `icap`); our `trx64u.cor` and `trx64g.cor` in MultiCore's package (P1-8, D46: their pins are our run 481 on SDK 0.4.6, MultiCore bff5d6730, delivery D45, contract 8ca00d17d; `trx64g.cor` packed in stage cor with MultiCore's scripts in `software/trx64g`); open: trx64g's settings import question fails under MultiCore, card and stick (firmware, §5), the trx64u tests MultiCore dropped (D22, list in §5), trx64u's `.cor` version `<branch>-<sha8>` vs contract §2's `x.y.z-dev+<sha8>`, emu CI stage, TOML A record in the .cor (MultiCore), SDK 0.4.7 (not packed yet): `rtl_check check` in lint (MultiCore's tool fix: a decoded read of the constant 0 counts as none), D53's open driver table (every source writes every block, replaces D50 and D52), owner split on the board (built: core blocks, UCI library `uci/`; decided gap: no Volume/Tone of the base on trx64u, no RTL, Alex), monitor engine, 1351 mouse and paddles (no POT pins in c64_basic; MultiCore GitHub #146, the board's `PADDLE_DIS` polarity not measured yet), T-3 Timewarp (T256 S6; built on 0.4.5, our bar the only one, on every canvas): `u18` not to `main` before it, board test (Return to host, Timewarp) | [U18](docs/specs/U18-trx64u-app-part-moves-here.md) |
| U19 | 🔨 | The app RISC-V gets MultiCore's caches (8 KB data, 8 KB instruction, 16-byte lines from the DDR2 word): `trx64u`'s menu took 3-8 s on the board, memory latency | Built 2026-11-23 (Alex: "bitstream anpassen … neuen Core bauen"); benches and CI green (27,593 LUT, 61/75 BRAM, WNS +0.104); board: trx64u's menu acceptable, the bitstream stays (Alex 11-23); open: `trx64g` `.cfw` on the board | [Spec](docs/specs/U19-app-riscv-caches.md) |
| U20 | ✅ | GitHub becomes the home: GitHub's runners build everything but Vivado, our builder the bitstream (MultiCore's T239.1/T239.5 pattern) | done 2026-11-25: PR #3 merged (d72ab54a), run #6 green, first pre-release `fw-main-d72ab54a`, `integration` cut; F (Hetzner, D24-D27): F-a and F-b ran green 2026-11-25 (Vivado on Hetzner works, 1.3-1.5x the builder's time, EUR 0.14-0.22 a build); Vivado stays on the builder (D27), a switch is a later spec | [U20](docs/specs/U20-github-home.md) |
| U21 | ⏸ | The online search of trx64u: a form, a results tab, Beyond64 as a third source | specified 2026-11-25 (D1-D6); waits for SDK 0.5.0 (MultiCore T262: `list`, `pick`, `show`, busy caption, providers, 3 base defects) | [U21](docs/specs/U21-online-search-rework.md) |
| T69 | ❌ | SID filter profiles for reSID8 | – (won't do) | [T69](docs/todo/T69.md) |

## Parked: platform, other cores, the old U64

**Multi-core moved (2026-10-13, Alex: "MultiCore inherits all the multi-core stuff under M3"):** T23, T26, T79, T81,
T83-T94 live in `TREX/TRX64U-MultiCore` (TODO.md, docs/specs, docs/todo there). This repository is the drop-in line.

| ID | Status | Title | Open | Details |
|---|---|---|---|---|
| T27 | ⏸ | Idea: port TRX64 Ultimate to the old U64 (Cyclone V) | Does not fit (120 % ALM); Alex decides | [T27](docs/specs/T27-u64-cyclone-port.md) |
| T34 | ⏸ | Replace the reDIP CIAs (needed only before a source release) | Alex's call whether still needed | [T34](docs/todo/T34.md) |
| T44 | ⏸ | Flash write gate | WEL=1 probe never run; protection returns with T50 | [T44-flash-write-gate](docs/specs/T44-flash-write-gate.md#status-and-history) |
| T46 | ✅ | Release build switch: leave out jtag_dbg | - | [T46](docs/todo/T46.md) |
| T50 | ⏸ | T+R+X flash unlock for updates | Not needed while the gate is open; protection returns with T44/T50 | [T50-update-unlock](docs/specs/T50-update-unlock.md#status-and-history) |
| T70 | ⏸ | TRX64 RE core for the old U64 (Cyclone V), minimal: C64, 1541, 2 SIDs, carts, Timewarp, monitor, C64RE, 576p | Next: real DDR2 and pins | [T70](docs/specs/T70-u64-re-minimal.md#status-and-history) |

## Extra

| ID | Status | Title | Open | Details |
|---|---|---|---|---|
| T22 | ❌ | DDT's demos on HDMI | – (won't do) | [T22](docs/todo/T22.md) |
| T47 | ❌ | Paddles/mouse, WASD-to-joystick, port swap, NMI sense | – (won't do for now) | [T47-paddles-mouse](docs/specs/T47-paddles-mouse.md#status-and-history) |
| T42 | ⏸ | MAC TX frame counter undercounts | Recheck after the store-and-forward patch | [T42](docs/todo/T42.md) |
| T43 | ⏸ | Gideon's `_sbrk` adds `incr` twice when the heap grows | Fix in trx64/main and report upstream | [T43](docs/todo/T43.md) |
| T51 | 📅 | EasyFlash 3 emulation in the generic decoder (banking only) | Board steps 3-4 (emulation round trip, erase) | [T51-ef3](docs/specs/T51-ef3.md#status-and-history) |
| T68 | ✅ | Licence: GPL-3.0-or-later (LICENSE, LICENSING.md) | – | [T68](docs/todo/T68.md) |
