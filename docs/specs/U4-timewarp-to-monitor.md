# U4 — From Timewarp into the monitor, at the frame you are on

**Status:** spec 2026-10-30, built 2026-10-30 (app repo 83d38d82); the board test is open. **Task:** TODO.md U4 (M2).
**Depends on:** T20 (delta ring), T21 (monitor), T12 (freeze overlay).

**Origin (Alex, 2026-10-30):** "Wenn ich im Timewarp bin, dann würde ich gerne per Tastendruck (im Menü ist
es die Kombi C= + O) in den Monitor, und dann aber auch auf dem Stand auf dem ich bin. Also im Grunde das
selbe wie RUN/STOP + Break + Monitor aufmachen."

## 1. What it has to do

In Timewarp, scrub to a frame, press C= + O, and the monitor opens showing **that** frame's machine: the
rewound RAM and the CPU registers of its first instruction. Not the freeze point, and not a machine that has
already run on past it.

## 2. What is there today (read, not assumed)

- `TrxTimewarpUI::poll()` (`software/io/c64/trx_timewarp_ui.cc:255`) has the key loop: cursors, PageUp/Down,
  Home, `KEY_RETURN` -> `leave(true)`, `KEY_BREAK`/`KEY_ESCAPE` -> `leave(false)`. No monitor key.
- **Timewarp does not apply anything.** `timewarp_action()` only calls `trx_ring_set_pending(chosen)` and
  hides the menu. Its own comment says it: "after a scrub step only the rewind in `C64::unfreeze()` puts a
  machine back that can run". While the scrub box is open, C64 RAM still holds the freeze point; the bar
  shows a frame out of the ring.
- The rewind happens in `C64::unfreeze()` (`software/io/c64/c64.cc:1701`): `trx_ring_unfreeze()` ->
  `scrub_final_locked()` walks the ring, re-enables log and capture, and the unfreeze then drops the freeze
  (`backupIsValid = false`, `slim_memonly_end()`, `trx_video_freeze_leave()`, `isFrozen = false`). Restore
  and resume are one step; there is no "apply and stay frozen".
- The ring knows the frame's CPU state: `DeltaEntry` (`trx_ring.h:55`) carries `pc`, `a`, `x`, `y`, `sp`, `p`,
  the opcode and its operands, and the timeline cycle of the edge that ended its opcode fetch.
- The monitor is an app. `app_open_trx_monitor(ui)` (`software/app_loader/app_registry.cc:621`) is
  `AppRegistry::run_task(ui, "trxmon", "serve")`; `tree_browser.cc:521` uses it for Ctrl-O today. The weak
  symbol is absent in a build without the app loader.
- **The breakpoint is hardware.** `apps/trxmon/src/mach.c:18` keeps `uint32_t s_bmp[2048]`, "the exec bitmap
  as written" - one bit per 6502 address - and pushes it with `hw_bmp_word()` / `hw_bmp_enable()`
  (`apps/trxmon/src/hw.h:131`). A run is a plain free run, `mach_run()` -> `hw_cmd(CMD_RUN, 0)`; the stop
  comes back with a hardware reason, `RS_BITMAP = 3`, and only then does `obs_dispatch()` in the app decide
  whether the stop stays. So a breakpoint can be armed and left; it does not need the app to be watching.
- **Nothing reacts to a stop on its own.** `TRX_IRQ_STOP` (`trx_io.h:58`) exists as a cause on the shared TRX
  interrupt line, but no handler is attached to it anywhere: `trx_cart.cc:283` takes FLASH|EEPROM,
  `trx_video.cc:910` FLIP|VSYNC, `trx_lv_port.cc:133` FLIP. trxmon does not use the interrupt either; its
  `hw_stop_record()` reads the stop registers directly.
- **Run control has an owner.** `trx_io.h:109` describes `trx_dbg_yield()`: when a debugger app holds run
  control (`trx_dbg.v` OWNER), the firmware's `C64_STOP` does not stop the machine, so the firmware clears
  OWNER and drops the debugger's hold. The app reaches the debug block through the register window the ABI
  hands it (`apps/trxmon/src/hw_trx.c`, `s_regs`, pages `P0/P1/P3`); the firmware has no header for that
  block today.

## 3. The mechanism

There is a call built for exactly this, and it is already in the app ABI: `ring->land(index)`
(`software/app_loader/app_api.h:463`, firmware `api_ring_land` -> `trx_ring_land`). Its contract:

> Puts the machine on entry index and leaves it paused on that frame's snap edge, the debugger holding it,
> without cutting the timeline: what runs from there is undone by the next land. The stop record says
> CYC_CMP; the app sets IPC (page 3 IPC_SET). 0, or < 0: `error()`.

and `ring->commit()`: "The debugger runs on from a land: the frames after it are cut and the ring records
again. Nothing when no land is open."

So the monitor can put the machine on a frame and hold it there itself. Nothing has to be applied while the
menu's freeze is up, no breakpoint has to be armed, and the unfreeze path is not involved at all.

1. In Timewarp, C= + O takes the entry the bar is on (`trx_ring_target(back)`, the same index RETURN uses).
2. Timewarp starts trxmon with that index.
3. trxmon calls `ring->land(index)`: the machine stands on that frame, the debugger holding it, and the
   monitor shows its RAM and registers. The timeline is not cut, so leaving without `commit()` changes
   nothing.
4. Running on from there is `ring->commit()`, which the monitor does when the user runs the machine.

An earlier draft of this spec had Timewarp arm an exec breakpoint at the frame's `pc` and let
`C64::unfreeze()`'s rewind run into it. That works in principle - the bitmap is hardware and can be armed
and left - but it is the long way round to what `land()` does in one call, it needs a second owner of the
bitmap, and it would report a bare stop rather than a breakpoint. Dropped.

## 4. Decisions

- **D1: the monitor does the work, not the firmware** (Alex, 2026-10-30). Timewarp hands over an index;
  `land()` and `commit()` are the monitor's, as is run control. The firmware does not arm anything and does
  not touch the exec bitmap, so there is one owner of the debug block at a time.
- **D2: no interrupt path.** Nothing has to react to a stop, because the monitor is the one that stopped the
  machine. `TRX_IRQ_STOP` stays unused; a running monitor notices its own stops by polling (`mach_poll()`
  reads `hw_status()` for `ST_STOP_NEW`), which is how a breakpoint set in the monitor works today.
- **D3: the key is `KEY_CTRL_O`,** the same the browser uses for the monitor (`tree_browser.cc:507`), so
  C= + O means the same thing in both places.
- **D4: without the app loader there is no key.** `app_open_trx_monitor` is a weak symbol; when it is absent
  the entry is not offered rather than offered and dead.
- **D5: `run_task()` carries the value, in `ctx->args`.** `AppRegistry::run_task(ui, id, action)`
  (`app_registry.h:89`) gains an optional `args` string that the launch puts into
  `u2p_app_context_t.args` - the field that already exists for it, "NULL or `args` of a REST call, passed on
  unchanged" (`app_api.h:72`). The app side needs nothing new and `app_api.h` does not change, so MultiCore,
  which pins this repository, is untouched. Timewarp passes the ring index as decimal text; trxmon reads it
  and lands on it. This is deliberately not a general app-to-app channel: there is none today (the ABI root
  table is `sys`, `mem`, `ui`, `fs`, `media`, `config`, `diag`, `net`, `trx`, `ring`, and `trx->announce()`
  only records the RPC port for discovery, `s_rpc_port = port`), and Alex wants this for Timewarp and the
  monitor only.
- **D6: the way back is out of scope** (Alex, 2026-10-30: "Ne lass D6 mal weg erst"). Monitor -> Timewarp
  would need an app asking the firmware to open a menu action with a value, which nothing in the ABI does.
  Not built, not designed; it comes back as its own decision if it is wanted.

## 5. Conditions that stay

- Timewarp already refuses to open unless the machine is frozen, and `trx_ring_reu_refusal()` blocks the ring
  while the REU is on. Both stay as they are; this adds no new way in.
- `backupIsValid = false` in the unfreeze is not in the way here: the machine really runs again, so the
  monitor attaches to a running, then halted, machine - the case it is built for.

## 6. Tests

- Bench: a ring with a known frame, arm the bitmap at its `pc`, unfreeze, and check the machine halts with
  `RS_BITMAP` at that `pc` and with the frame's registers.
- Board: scrub back a measurable distance, C= + O, and compare the monitor's PC and registers against the
  `DeltaEntry` the bar showed.
- Negative: the same in a build without the app loader (no key), and with the REU on (Timewarp refuses).

## Status and history

The TODO.md table is an overview only; the row detail lives here.

U4: From Timewarp into the monitor, at the frame you are on

Milestone M2, status 🔨. Open (one line, as in the TODO table): board test open; the way back (monitor -> Timewarp) is out of scope

- **2026-10-30, spec written, then cut down.** The first draft had Timewarp arm an exec breakpoint at the
  frame's `pc` and let the unfreeze's rewind run into it, with an open question about who arms it and who
  brings the monitor up. Both questions fell away on reading the ring table of the app ABI: `land(index)`
  already puts the machine on a frame and holds it there, and `commit()` runs on from it. The monitor is
  therefore the only actor, there is no interrupt path to build, and all that is missing is a way to hand
  the index to the task being started (D5) and a decision about the way back (D6).

- **2026-10-30, D5 fell away again** (U5). With the monitor in the firmware the key does not start an
  app any more: `C= + O` calls `trx_service_open_at()`, which lands on the entry and puts the screen up
  in one step, so nothing has to be handed over as text. The optional `args` of
  `AppRegistry::run_task()` stays — it is still the only way a task launch carries a value — but the
  monitor no longer uses it, and `app_open_trx_monitor_at()` is gone with the app. What the key does for
  the person is unchanged.

- **2026-10-30, built** (app repo `83d38d82`). `KEY_CTRL_O` in the scrub box takes the entry the bar is on
  and starts trxmon with it instead of making a rewind pending, so leaving the monitor changes nothing.
  `AppRegistry::run_task()` gained an optional `args` string (D5): a task launch builds no selected file, so
  the command's file name carries it into `ctx->args`; `app_api.h` is untouched, so MultiCore is untouched.
  trxmon reads `land=<n>` at start and lands through its own `ring_land()`, keeping its landed flag and
  cycle offset right. The firmware and trxmon both compile clean (trxmon for RISC-V locally; the Nios II
  variant needs the toolchain CI has). **Open: the board test** (§6).
