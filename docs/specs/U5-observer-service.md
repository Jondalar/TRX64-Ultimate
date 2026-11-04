# U5 — The stops move into the firmware: observer service, RPC server and monitor

**Status:** spec 2026-10-30, built 2026-10-30 (app repo `46e182b8`); the board tests are open. **Task:** TODO.md U5 (M2).
**Depends on:** T21 (monitor), T20 (delta ring), T12 (freeze overlay), U4 (Timewarp into the monitor).
**Touches:** [ADR-0031](../adr/0031-trxmon-single-rpc-connection.md) (one RPC connection — stays),
[ADR-0056](../adr/0056-trx64-ultimate-is-the-monolith.md) (the monolith), the MultiCore contract
([docs/contracts/trx64u-firmware-handover.md](../contracts/trx64u-firmware-handover.md)).

**Origin (Alex, 2026-10-30), after a board session with the shipped firmware:** "Ich dachte, ich kann am
System BKs und Observer registrieren und verwalten, über den Monitor (UI oder RPC), und wenn einer kickt,
dann macht das System was. De facto muss die Monitor-App aber laufen und ich mit `x` die Maschine laufen
lassen. Sobald ich freeze mache und die App nochmal per UI aufmache, schiesst sie meiner Meinung nach ab.
Das heisst für mich, BK und Observer sitzen im Monitor … ich denke, das ist schlechte Architektur und wir
brauchen eine Art Observer-Service, der dauerhaft läuft, und RPC und MonitorUI sind Clients davon." And
on the server: "Der Server sollte mit in die Firmware dann."

## 1. What it has to do

1. Set a breakpoint or an observer — from the monitor screen or over RPC — then leave the monitor and use
   the machine. The stops stay armed.
2. A breakpoint fires: the machine stops **there**, and the monitor opens on it. No key, no app start, no
   condition attached to it (Alex, 2026-10-30: "Wenn ich einen BK setze und dann spiele und der feuert,
   dann muss der Monitor aufgehen und die Maschine da anhalten").
3. Observers whose action is count or log keep counting and logging; they do not stop the machine, as today.
4. Running another app (`trex_crt`, a sample, anything) does not clear the stops.
5. RPC on TCP 4312 answers whether or not an app is loaded, with the method set C64RE uses today.
6. Opening the monitor twice in a row, after a freeze, from the browser or from Timewarp, is an ordinary
   screen open — there is no second instance to collide with.

## 2. What is there today, read from the code

- **The table is the app's.** `apps/trxmon/src/mach.c` keeps `M.bp[]`, `M.obs[]` and the exec bitmap
  `s_bmp[2048]` in app RAM; `rpc_stops_armed()` (`apps/trxmon/src/rpc.c:540`) walks exactly those arrays.
- **The firmware wipes the hardware when the app ends.** `api_trx_release()`
  (`software/app_loader/app_api_net.cc:342`) writes `TRX_P3(0x048)=0` (exec bitmap off), `TRX_P3(0x080)=0`
  and `TRX_P3(0x0A0)=0` (both watch units), `TRX_P3(0x0C0)=0` (drive comparator), `TRX_P0(0x134)=0`
  (CYC_CMP), `TRX_P0(0x118)=0` (OWNER) and `TRX_P3(0x000)=0x03` (RUN, stop record dropped). It runs from
  `run_app()` when the app returns and from `resident_entry()` when a detached task ends. So every stop
  dies with the app, by policy, not by accident.
- **Half a service already exists.** Leaving the monitor with `x` while stops are armed detaches it
  (`apps/trxmon/src/main.c:839` -> `trx->detach`, `app_api_net.cc:294`), and a stop then asks the firmware
  for the screen (`trx->ui_request`, `app_api_net.cc:465`, run by `UserInterface::run_once` through
  `app_loader.cc:75`). It works — until anything else wants the app area.
- **There is one app slot.** `AppLoader::run_app()` (`software/app_loader/app_loader.cc:377`) ends a
  resident app first (`app_resident_stop(3000)`), and refuses the launch with "An app still runs in the
  background" when it does not end in time. A REST launch is refused outright. Any second app therefore
  takes the monitor's state with it.
- **An app may not run while the machine runs.** `run_app()` stops the C64 for the app's run unless the app
  holds the machine itself. That is why the monitor and the machine are tied together today.
- **A loopback client is not possible.** `software/network/config/lwipopts.h:811` has
  `LWIP_HAVE_LOOPIF 0`; only `LWIP_NETIF_LOOPBACK` (packets to the device's own address) is on, and that
  needs an interface with an address. An app could not reach an in-firmware server over TCP without a
  network.
- **The UI table is a thin wrapper over the firmware's own screen.** `api_ui_screen_puts()` is
  `win->move_cursor(); win->output()` (`app_api_impl.cc:538`), `api_ui_win_set_char_at()` is
  `w->set_char()` (`:599`), `api_ui_getch_monitor()` is `keyboard->getch()` plus the monitor keymap
  (`:631`). `ui.c` and `main.c` together use 16 distinct entries of that table.
- **Gideon's machine monitor is in the tree but not in our build.** `software/monitor/` (499 KB of source,
  upstream `f91b7799 Add machine code monitor (#358)`, 43 commits) is compiled by the u64, u2plus,
  u2plus_L and PC test targets. `target/u64ii/riscv/ultimate/Makefile` does not name it. On the C64U it
  costs nothing, and it is not what this spec replaces.

## 3. The budget, measured on 2026-10-30

**Flash.** The APPL slot on the 50T is `0x220000 .. 0x400000` = 1,966,080 B
(`software/io/flash/w25q_flash.cc:52`). The shipped `ultimate.app` of package `main-11265524` is
1,420,928 B, so 545,152 B of that slot are unused. The code that moves measures 164,172 B of text
(riscv32, `-Os`, built from this tree):

| object | text | object | text |
|---|---:|---|---:|
| `mon.o` | 54,603 | `ring.o` | 15,741 |
| `rpc.o` | 28,244 | `mach.o` | 14,304 |
| `trace.o` | 17,841 | `asm6502.o` | 11,695 |
| `hw_trx.o` | 5,207 | `cond.o` | 4,731 |
| `json.o` | 4,500 | `ws.o` | 3,791 |
| `dis6502.o` | 2,152 | `spans.o` | 1,363 |

plus `ui.o` 10,149 for the screen. After the move the slot still has ~370 KiB.

**RAM is the tight one.** The firmware's region is `0x00030000` + `0xE60000` = 15,073,280 B
(`target/u64ii/riscv/ultimate/linker.x:7`), and `ultimate.elf` already uses text 1,064,372 + data 359,896 +
bss 13,203,180 = 14,627,448 B. The 445,832 B that are left **are** the heap and the stack
(`__heap_start` after `.bss`, `__crt0_stack_begin = __heap_limit - 4`), not spare room. trxmon's static bss
alone is 511,256 B — `s_fbuf`, `s_reply` and `s_move` 64 KiB each, `s_text` 48 KiB, `s_b64` 43 KiB, the
client table 36 KiB, `s_mem` 32 KiB, `spans` 16 KiB, the exec bitmap 8 KiB. A 1:1 move does not fit.

## 4. The shape

**One binary, three faces.** The sources that are trxmon today become the firmware's debug service:

```
        exec bitmap, watch units, drive comparator, OWNER        (trx_dbg.v, page 3)
                                  |
                       the service: table + arming + stop dispatch + run control
                      (mach, cond, ring, trace, mon)   <- owns the debug block
                          /                        \
              monitor screen (ui)            JSON-RPC/WebSocket on 4312 (rpc, json, ws)
          Ctrl-O, Timewarp C= + O                     C64RE, any client
```

**The sources move by being compiled, not by being rewritten.** They stay C, they keep calling the same
`A->ui`, `A->net`, `A->sys`, `A->trx` and `A->ring` tables, and the firmware builds a table of its own
whose entries are the functions `app_api_impl.cc` and `app_api_net.cc` already provide. That keeps the
host test suite (`apps/trxmon/host`, `test_main.c` alone is 101 KB) pointed at the same code, which is the
only test cover this code has.

**The service task** replaces `main.c`: it starts with the firmware, owns the debug block from then on,
polls for stops the way `serve()` does today, and asks the UI task for the screen on a stop, through the
mechanism `trx->ui_request` already uses. The RPC listener binds when the network comes up and rebinds
after a link loss.

**The buffers live in a DDR2 window of their own**, `0x03800000 .. 0x03A00000` (2 MB), directly below the
app area (`app_format.h:32`, `U2A_LOAD_ADDR 0x03A00000`). The window is inside the updater's area
(`update_trx.cc:48`, `IMAGE_LOAD 0x03000000`), which nothing else uses while the firmware runs.

## 5. Decisions

- **D1: the monitor screen moves with the service; `trxmon.u2a` is retired** (Alex, 2026-10-30). `ui.c`
  keeps its code and gets the firmware's screen through the same 16 calls it uses today. One binary, no new
  ABI, no app slot to lose, and no second copy of the state. The app's manifest, its `rest: ["serve"]`
  entry and its CI step go; the sources stay where they are so the host tests keep building.
- **D2: the service's buffers live in `0x03800000 .. 0x03A00000`** (Alex, 2026-10-30, "eigenes DDR2-Fenster
  im Updater-Bereich"). Agreed with the MultiCore session on 2026-10-30: their host uses
  `0x03000000-0x033FFFFF` (`STAGE_APP`) and `0x03400000-0x03BFFFFF` (`STAGE_PART`) **only before the jump
  into the core**, and nothing of theirs lives there afterwards; their generic core app uses the same area
  only while updating. Two rules follow and both are ours to keep: the service **zeroes the window at
  start** (it holds the remains of a partial bitstream after a MultiCore boot) and **gives it up when an
  update starts**. The reservation goes into the contract document in both repositories.
- **D3: a breakpoint stops the machine and opens the monitor. Always.** (Alex, 2026-10-30: "dann muss der
  Monitor aufgehen und die Maschine da anhalten".) No per-stop setting, nothing to configure. Observers
  with the count or log action go on running and never open a screen, which is what they are for.
- **D4: the service owns the debug block.** `trx->acquire()` from an app succeeds only while nothing is
  armed and no stop is being held; the service then stands down until the app ends and re-arms afterwards.
  With stops armed the acquire is refused with a reason that names them, instead of silently wiping them as
  `api_trx_release()` does today. `api_trx_release()` keeps its register writes for the app case, but it is
  no longer the end of the stop table.
- **D5: the RPC contract does not change.** Port 4312, one connection ([ADR-0031](../adr/0031-trxmon-single-rpc-connection.md)),
  the same method names, the same `ping` with capabilities and build ids. What changes is that the server
  is there before any client, and stays after every client. `trx->announce()` keeps reporting the port for
  the ident service; the firmware fills it at start.
- **D6: a reset keeps the stops, a reboot does not** (Alex, 2026-10-30: "Reset? Ja. Reboot? Nein").
  Resetting the C64 leaves everything armed and the table untouched; a power cycle, a new bitstream or a
  firmware restart starts the service empty. Nothing goes to flash — breakpoints are session state, and the
  `.cfg` beside a `.cor` is for settings. The hardware already draws the line in the same place:
  `top.v:1327` wires the core's reset as `.rst(sys_rst)`, so a C64 reset does not reach the `rst` branch of
  `trx_dbg.v:381` that clears `bmp_en` and `cmp_en`, and `trx_dbg.v:314` (`if (brk) hold <= 1'b1`) halts on
  a hit whatever `own` says. Two things follow for the build: the firmware drops OWNER on every stop it
  makes itself (`trx_dbg_yield()`, called from `C64::hard_stop()`, `C64::stop()` and `C64::freeze_slim()`),
  so the service **must not read OWNER as "my stops are gone"** and must take run control back when it
  wants to hold the machine; and a stop that fires while the firmware owns the gate has to be picked up the
  same way as any other.
- **D7: U4 gets shorter.** With the monitor in the firmware, Timewarp's C= + O calls the service directly
  instead of starting an app with `land=<n>` in `ctx->args`. That left `AppRegistry::run_task()` without a
  caller, so it is gone (2026-10-31); the `args` a task launch can carry stays in `launch()` for a caller
  that builds the command itself. `tree_browser.cc:521` (Ctrl-O) and `app_open_trx_monitor()` lose their
  weak-symbol dance and call the service.
- **D8: the firmware grows for both cores.** The app repository is shared with MultiCore; `trx64g.cor`
  carries `ultimate.app`, and their `cor_pack.py` allows 4 MiB (`LARGE_APP_MAX`). 1.42 MB + 164 KiB fits,
  and they confirmed on 2026-10-30 that no build switch is needed on their side.
- **D9: Gideon's `software/monitor/` stays untouched and unbuilt.** It is upstream code in a fork shared
  with MultiCore; deleting it would only cost merges. It is not compiled for the C64U today and this spec
  does not start compiling it.
- **D10: nothing of the app ABI is taken away.** `trx->detach`, `trx->ui_request`, `trx->stop_requested`
  and the ring table stay for other apps (c64re is the next one that will want them). This spec removes one
  app, not the mechanism that carried it.

## 6. What this changes outside the service

- `tree_browser.cc` Ctrl-O and the Timewarp key open the firmware monitor; the app registry entry and the
  `trxmon` manifest go.
- The update package stops carrying `trxmon.u2a` (`tools/make_update.sh --u2a`, `ci/release.sh`), and the
  pipeline step that builds it goes with it. The user manual (U2) loses the "run trxmon.u2a from the file
  browser" step: the monitor is simply there.
- The contract with MultiCore gains a "DDR2 windows" section with the reservation from D2.

## 7. Tests

- **Host** (`make -C apps/trxmon/host test`): unchanged and must stay green — same sources, same fakes.
  They are the regression net for mach/cond/rpc/mon while the build moves.
- **Firmware build:** the service compiles into `ultimate.app` for u64ii/riscv; the flash number
  (`ultimate.app` <= 1,966,080 B) and the RAM number (bss + heap headroom) are checked in CI, not eyeballed.
- **Bench:** the `dbgsess` sim gets a case that arms a breakpoint, ends the thing that armed it, and checks
  the bitmap is still armed and the stop still fires — the regression this whole spec is about.
- **Board:**
  1. **PASSED (Alex, 2026-11-04)** Set a breakpoint, leave the monitor, start a game, hit the address: the
     machine stops there and the monitor opens on it.
  2. The same with an observer on count: the game keeps running, the count rises.
  3. **PASSED (Alex, 2026-11-04, with WL)** Set a breakpoint, run `trex_crt`, come back: the breakpoint is
     still there.
  4. C64RE connects over RPC with no app loaded at all, sets a breakpoint, runs, gets the stop.
  5. Freeze, open the monitor, leave, open it again: no refusal, no crash (the fault that started this).
  6. Start a firmware update with the service running: the window is given up, the update runs through.

## Status and history

The TODO.md table is an overview only; the row detail lives here.

U5: The stops move into the firmware: observer service, RPC server and monitor

Milestone M2, status 🔨. Open (one line, as in the TODO table): built; board tests 1 and 3 of §7 passed 2026-11-04, four open; the dbgsess case runs first in CI

- **2026-11-04, board tests 1 and 3 passed (Alex).** "1 geht", and 3 "mit WL getestet, geht": a breakpoint
  set, the monitor left, the address hit — and the arming survives a run of `trex_crt` with WL, which is the
  whole point of the service. Earlier: "Der Monitor als App klappt, Breakpoints
  kommen hoch!". The firmware on the board is the one of release 0.2.0 (`main-d71d3928`, app `d36dcf78`); no
  other build starts the service at boot. Tests 2, 4, 5 and 6 stay open ("Der Rest mach ich noch").

- **2026-10-31 evening, the service never started at boot.** Found in ue2emu while the manual's monitor
  pictures were being taken, with the firmware of release 0.2.0 (`ultimate.app` of `main-3a9d10c2`): the
  console carried `[TRX] IO block present, CAPS EF` — bit 3 is the debug block — and the next line said
  `[TRX] no debug block: the monitor service does not start`. Both are true at once because
  `trx_service_start()` asked for two things in one condition: the CAPS bit **and** `g_app_api.trx`. That
  table is built on demand by `app_api_refresh()`, which is called when an app opens or runs
  (`app_loader.cc:104`, `:477`) and from `open_screen()` — never before `trx_service_start()` in
  `ultimate.cc`. So at boot the pointer was the NULL of its initialiser (`app_api_impl.cc`: `NULL, // trx:
  app_api_refresh()`), the service did not start, nothing listened on TCP 4312, and the menu entry — which
  hangs on the CAPS bit alone and was therefore present — answered with a popup blaming the bitstream.
  Nothing contradicted it: the six board tests of §7 are open, and no automated suite booted the firmware
  and looked. The fix is in the app at `d36dcf78`: `trx_service_start()` refreshes the table itself, the
  two printfs name their own cause, and the popup speaks of the service instead of the bitstream.
  `tests/e2e/firmware/trx_service_boot_test.py` (suite `trx-service-boot`) boots with an empty stick, reads
  CAPS from the firmware's own line and insists on `[TRX] monitor service up`; against the firmware of
  `main-3a9d10c2` it fails with the old message. **Release 0.2.0 was rebuilt on 2026-11-01** from
  `main-d71d3928` (app `d36dcf78`) and carries the fix, so its notes' promise of a port that answers from
  boot holds; the files it had for one evening were never downloaded (docs/todo/T68.md).

- **2026-10-31, what the app left behind.** `app_trx_cleanup()` cleared the announced RPC port whenever any
  app returned, so after one `trex_crt` run the UDP ident answer and `GET /v1/info` stopped naming port 4312
  although the service went on serving. The port now comes from the service itself: `trxsvc_rpc_port()`
  (`apps/trxmon/src/main.c`) reports 4312 while its listener stands, 0 before that, and the cleanup restores
  that instead of zeroing. With it: the OpenAPI text of `PUT /v1/apps:run_file` no longer names `trxmon.u2a`
  and the action `serve` (`softboot.u2a` / `run` now, and the resident-task sentence lost its "(trxmon)"),
  `AppRegistry::run_task()` and its "Run trxmon.u2a once from the file browser" popup are deleted, and the
  two "the monitor app" comments at `socket_dma.cc:638` and `routes.cc:256` say "the debug service". The
  host tests of the monitor build and pass unchanged (1077 + 74 + 29 + 279 checks).

- **2026-10-31, the documentation pulled through.** Manual: the monitor chapter, the apps chapter, the
  action menu and the network chapter were already rewritten with the build; now the index names the state
  it describes (`main` after U5, host `5021852`, app `8b915e6f`, package `main-50218525`) and says the
  screenshots come from the `b151154b` package, the REST row names the one app action that is left
  (`softboot` `run`), the discovery row drops "while it serves", and the differences chapter says the
  monitor sits in the firmware. The architecture page (`https://claude.ai/artifact/367eAB5hipPpN4u2fVj4Vj`,
  version 18) carries the same in its tester overview, three applications instead of four, and the final
  sheet now shows the service and its 2 MB DDR2 window. Four spots in the code that still belonged to the
  app were found while reading; they are the next bullet.

- **2026-10-30, built** (app repo `87d3cab8` the rename, `3159bae8` the service, `46e182b8` the app's
  retirement; [ADR-0060](../adr/0060-monitor-becomes-a-firmware-service.md)). The sources did not move,
  the build did: `target/u64ii/riscv/ultimate` compiles them in and `trx_service.cc` hands them a table
  of the firmware's own. One name collided on the way, `json.c/h` against `software/api/json.h` and
  `software/test/json.c`; it is `mjson` now. The service keeps its sockets out of the app loader's list
  and uses an acquire that leaves `s_owned` alone, so neither `app_net_cleanup()` nor
  `api_trx_release()` can take anything from it; an app that asks for the block while stops are armed is
  refused. The screen is reached three ways: Ctrl-O, Timewarp's C= + O and Developer > TRX64 Monitor,
  the last one added in the firmware because it used to come from the app's manifest. Measured: text
  1,196,584 (+132,212), data 395,100 (+35,204), `.trxsvc` 510,976 B at `0x03800000`, the image's own bss
  up by 292 B, `ultimate.app` 1,591,696 of the 1,966,080 the slot holds, 272.9 KiB left above the image
  for stack and heap; `ci/app.sh` fails on either budget now instead of printing it. Host tests
  unchanged and green (1077 + 74 + 29 + 279 checks, 0 failed). **Open:** the six board tests of §7, and
  the `dbgsess` case of §7 has never run here — this machine has no ghdl, so CI runs it first.

- **2026-10-30, D6 decided and checked against the gateware.** Alex: a reset of the C64 keeps the stops, a
  reboot does not. The hardware already separates the two — the core's reset is `sys_rst` (`top.v:1327`), so
  a C64 reset never reaches the branch in `trx_dbg.v:381` that clears `bmp_en`, and a hit halts the machine
  without regard to OWNER (`trx_dbg.v:314`). The catch for the build is the other direction: the firmware
  gives run control back on every stop it makes (`trx_dbg_yield()`), so the service may not take a missing
  OWNER bit for a lost arming.

- **2026-10-30, spec written.** Alex found on the board that breakpoints only live while the monitor app
  lives, and called the architecture wrong. Reading the code agreed with him: the table is app RAM and
  `api_trx_release()` wipes the hardware when the app ends. The measurements decided the shape — flash has
  room (545 KiB free in the APPL slot against 164 KiB of code), RAM does not (445,832 B left, which is the
  whole heap, against 511,256 B of static buffers), so the service gets a DDR2 window of its own, agreed
  with the MultiCore session the same day.
