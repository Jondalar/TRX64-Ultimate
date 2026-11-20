# U3: the generic core app replaces Gideon's app on TRX64 Ultimate

**Status:** PROPOSED (2026-10-25). Gap analysis done; nothing built here. Not exported to GitHub (it speaks of the
repository the generic app lives in).
**Origin:** Alex, 2026-10-25 (translated): "ask the MC session where and what exists for the generic host app, then do a
comparison of what we must do to replace Gideon's ultimate.app in TRX64-Ultimate with ours, open a spec and a TODO entry."
**Decision behind it:** [ADR-0057](../adr/0057-generic-core-app-replaces-gideons-app.md): Gideon's app is replaced fully,
no compat mode; one generic core app for all cores; TRX64-FW drives the replacement for this repository; the day-1 list
(ADR-0057 point 7) must run on the board before `main` switches.
**Where the code lives:** `TREX/TRX64U-MultiCore` `coreapp/` (application, C64 descriptor `coreapp/cores/c64.cc`),
`app/` (libraries, e.g. `app/lib/cbm1541`), build `host-app/target/u64ii/riscv/coreapp/`, benches `tests/coreapp/`.
Its spec for the monolith is MultiCore `docs/specs/T104.2-generic-app-on-trx64-ultimate.md` (P1-P7, D1-D9, S1-S16);
this spec is the monolith's side: the requirement set, the gap and the order. TRX64-FW works in its own clone of
MultiCore; the MultiCore session reviews and merges (ADR-0057 point 6).
**Milestone:** M2.

## 1. Requirement set

What Gideon's application does today on our bitstream, as written down from the code in the user manual
[`docs/manual`](../manual/README.md) (release b151154b, app 234a0abb). Every row there is a requirement, except what
the manual marks "no effect" or lists as not available ([10-differences](../manual/10-differences.md)); those are
dropped (appendix §15).

Added on 2026-10-25: the way back from this firmware to the MultiCore host, when the TRX64 Ultimate `.cfw` is started
from there, is the **Power Cycle** menu item (the ESP32 switches the machine off and on; the FPGA loads flash 0x0). The
replacement keeps Power Cycle; no IPROG path is needed here.

## 2. State of the generic app (MultiCore `origin/main` 169a865e, 2026-10-25)

- Built and benched on the Mac for the monolith: board profile without a host ("monolith", no hand-over), the C64
  descriptor for the ID "TRX2", ROMs from `/flash/roms`, reset, freeze, Run PRG / Load PRG and start from, settings in
  `/flash/trx64u/c64.cfg`, the 1541 library (`cbm1541`: mount, Run Disk, Mount & Run, Real Run, create and save D64/G64),
  the image size check.
- **Nothing of it has run on RISC-V on the monolith or on the board.**
- The C64 menu has: Browse (`.prg .p00 .d64 .g64 .rom`), 5 settings (Freeze mode, drive), 7 tasks (Reset, Reload ROMs,
  Reset Drive, create/save D64/G64), F8 Restart core. Everything else of the manual is missing.
- Not linked: HTTP/REST, Telnet, SNTP, ident, the app runner, the monitor, any Timewarp code, any `.crt` code. Linked
  and unverified: Gideon's FTP daemon.
- Apps: the loader reads only `.u2a` format 3 / ABI 3 and never calls `app_main`; `trxmon`, `trex_crt`, `softboot` and
  `unzip` (format 2, ABI 2.1) cannot run. Decided in MultiCore T104.1 Q6: they are rebuilt on ABI 3.
- The base refuses `/flash` writes except three files, which blocks `/Flash/apps`, Copy to Flash and other configs.

## 3. Gap summary

| Group | Today in the generic app | Work (layer) | Size |
|---|---|---|---|
| Boot and start | Bench only; no flash install; boot keys missing | Board bring-up; flash install with the matching bitstream (release build); boot keys | S-M |
| Machine | Reset, freeze, menu button | Clr Mem, Power OFF / Power Cycle (control module), C= hotkeys, long-press disk swap, Save C64/REU memory | M |
| File browser and types | Browse 5 types, hides the rest | File operations (view, copy, move, rename, delete, mkdir, select), `.cfg .sid .mod .reu .t64 .vpl .bin .crt .u2a .cfw .zip` | L |
| Drive | Mostly done (bench) | Geos protection, Insert Blank, `.g71`, Create Directory | S |
| Cartridges | Capability name only | CRT library (trx_cart descriptors), startup cartridge, Save Changed Cartridge, Cartridge Preference, REU/UCI/Audio interlock, `trex_crt` on ABI 3 | L |
| Settings | 5 drive/freeze items + Picture | Video (system mode, scanlines, palette), mixers, Power LED, memory/REU/UCI/Audio, turbo, power, user interface, SID pages, joystick, network, streams | L |
| Audio | none | reSID8 and mixer settings, socket detection, REU/UCI/sampler/SID player libraries | L |
| Video | own-picture path (bench) | system mode, palette, scanlines | S-M |
| Turbo | none | four settings on the T29 registers | S |
| Timewarp | probe only | ring ops, the bar, resume sequence, REST | M-L |
| Monitor | none | ABI 3 tables (UI, net, trx, ring), `trxmon` rebuild, C= O | L |
| Apps | loader without runner | ABI 3, runner, own UI in a container, manifest -> menu, four rebuilds (MultiCore T104.3) | L |
| Network | link, FTP (unverified) | REST with an alias table for Gideon's `/v1` paths, Telnet, DMA service, ident (`trx64` block), SNTP, syslog, streams, password, services | L |
| Update | none | `.cfw` Run Update through Gideon's updater (T104.2 D9.3), rollback test before the first flash write, softboot | M |

The full table with evidence (file:line in MultiCore) is the appendix.

## 4. Order (proposed)

1. **Emulator first** (Alex, 2026-10-25: iterate in ue2emu, build to completeness, then the board): the generic app
   in ue2emu with our capability word (`--caps 25E40202 --board c64u`, trx-io), day 1 complete there. The emulator
   session closes three gaps first (requested 2026-10-25): CAPS3/reSID8 registers, drive A 1541-only, the
   expansion-port slot block (SLOT_CTRL/SLOT_STATUS) for `--cart-slot`.
2. **Day 1** (ADR-0057 point 7), first in the emulator, then on the board (T104.2 S1, S10, S11, S15, S16; every
   board session only with Alex's OK): firmware update and the way back (rollback proven before the first write to
   0x220000), REST enough for the board tests and deploy, `.crt` start, Timewarp, monitor (`trxmon` on ABI 3),
   leaving every state by menu, Power Cycle.
3. **Switch `main`** to the generic app when day 1 runs on the board. Gideon's original firmware stays the safety net.
4. **Parity** with the manual, group by group (§3), each with its benches and a board check; the manual is rewritten
   for the new menus as each group lands.

## 4a. Decided

- **2026-10-25 (Alex):** applications are self-contained `.u2a` containers that hook into the declarative menus like
  libraries; they bring their own UI: the base gives an app a container on the pixel layer and the app builds its LVGL
  UI there (the cartridge tools get a modern UI); the monitor keeps the 80-column terminal. It goes into the shared
  design: MultiCore T104.3 (`docs/specs/T104.3-applications.md`), developed by TRX64-FW in its clone (branch
  `t104-3-app-ui`) and handed to the MultiCore session when it is round.

## 5. Decisions for Alex (one at a time)

1. How the monolith installs and updates the generic app: Gideon's updater as the flash writer and how its `.cfw` is
   packed (bitstream + generic `.app`, slot 0x220000, at most 0x1E0000 - 12 bytes); rollback as the gate.
2. `/flash` write policy: allow `/Flash/apps` and the registry (and Copy to Flash), or apps and libraries only on sticks.
3. REST scope of day 1: which of the manual's routes go into the alias table; Telnet, DMA service, discovery.
4. Freeze-only (as built): drop "Overlay on HDMI" and Overlay Transparency?
5. Boot keys, C= hotkeys, long-press disk swap: keep or drop.
6. Power settings and Power OFF (control module): keep or drop (Power Cycle stays, §1).
7. Cartridge, REU, UCI, sampler, SID player, `.t64`, `.mod`: libraries (`.u2a`) or in the base, and their order.
8. Gideon's saved settings: no migration to `/flash/trx64u/c64.cfg` (as decided for MultiCore Q1)?
9. "Run with App" and "Load into MP3 Drv": keep or drop.
10. ROM choice: file selection per ROM as in the manual, or the fixed name list as built.

## 6. Status and history

- **2026-11-20: where the code lives changes (MultiCore T239, T239.3).** trx64u's menu, its feature code, the monitor,
  the RPC server and the ring move from MultiCore's app core into a library built here against their SDK
  ([U18](U18-trx64u-app-part-moves-here.md)). The header's "Where the code lives" and ADR-0057 point 6 (we work in a
  clone of MultiCore) describe the old split; whether this spec's goal stands is U18 question 2.
- **2026-11-04: the goal said plainly, and what it already costs** (Alex: *"Ich will langfristig auf MultiCore
  Only als App. Mit unseren eigene BDD tests e2e. Und bis dahin muss das möglichst störfrei und einfach bauen.
  Bistream+Gideon App."*). Nothing is built here yet; the decision it did produce is
  [ADR-0063](../adr/0063-app-repo-ci-one-job-no-gideon-ui-repairs.md): the fork's CI keeps one job, and the
  three e2e suites that test trx_crt's screens in Gideon's menu are not repaired for our bitstream - they come
  back as BDD scenarios on the generic application (T71). Until then the release stays bitstream plus Gideon's
  app, built from the pinned submodule commit.
- **2026-10-25:** spec opened (Alex). State of MultiCore asked from the MC session; gap analysis of the manual against
  MultiCore `origin/main` 169a865e (code read, spot checks: C64 golden menu, `/flash` write exceptions, linked sources).
  Power Cycle as the way back recorded (MC session, 2026-10-25).
- **2026-10-25:** emulator first (Alex): order step 1 changed; the three emulator gaps sent to the emulator session
  with Alex's OK.

---

## Appendix: gap table with evidence (MultiCore `origin/main` 169a865e)

Paths are in `TREX/TRX64U-MultiCore` unless they start with `TRX64-Ultimate`. DONE = built and benched on the Mac; no
row has run on the board.

### 1. Boot and start

| Requirement | Gideon on monolith | Generic status | Evidence | Work to close |
|---|---|---|---|---|
| P1 start without hand-over (JTAG or boot ROM, no `HANDOVER_ADDR` read) | Boot ROM copies record, jumps | DONE (bench; board: no) | `coreapp/src/coreapp.cc:50-55,72-91` (hand-over read only if `board.has_handover`); `board.cc:6-50` (ICAP byte != 0x7A => monolith); `start.cc:96-110` (cfg path from profile); `tests/coreapp/test_board.cc`, `test_start.cc` | Board run S1 (JTAG `trxdbg.sh boot staged`) |
| Identify "TRX2", pick C64 descriptor | n/a | DONE (bench) | `coreapp/cores/c64.cc:439-464` (`{'T','R','X','2'}`) | Board run |
| Flash slot 0x220000, size 0x1E0000-12 | Gideon app lives there | PARTIAL: size check exists; image not shown to boot from flash | `tools/check_app_size.py:1-40` (limit 1,966,068); spec log T104 5.14 says `coreapp.app` 845,532 B on builder (not re-verified here: unverified); no code writes 0x220000 | Flash install path (see Update); board S13 |
| Boot ROM stamp check (flash bitstream build stamp == running one, else no flash boot) | Updater writes bitstream + app together | MISSING (nothing of the generic app writes the slot; it is only a build constraint) | `TRX64-Ultimate software/bootrom/bootrom.c:405-420` (checked: `has_build && found && axss == build`) | Updater/`.cfw` producer must pack generic `.app` with the matching bitstream: host/updater + release build |
| Boot ROM request boot from DDR (what `softboot` uses) | `softboot.u2a` writes `TRX_REQ_*` block | MISSING | bootrom.c:405-415 request block; no use in coreapp | Part of softboot replacement (apps/update) |
| Splash with named steps | Gideon's boot screen | DONE (bench render; board: no) | `run.cc:300-305,313-321,366-404` (4 steps) | none |
| Settings loaded at start from `/flash/trx64u/c64.cfg` | Gideon flash sectors 0xFF0000.. | DONE (bench `test_settings`, `test_noflash`; board: no, "round trip by hand" open) | `fsif.h:15-37` (only 3 flash paths writable), `start.cc:106-110`, `settings.cc:57-82` | Board S11. Note: Gideon's saved settings are NOT imported (separate store, by decision Q1) |
| ROMs from `/flash/roms` first, then `<root>/roms/c64` | `/flash/roms/<cfg name>` | DONE (bench `test_romfind`; names unmeasured on board) | `c64.cc:60-65`, `boot.cc:98-121`, `board.cc:15-17` | Measure real file names on the monolith flash (spec: no FTP listing taken) |
| Boot keys: CTRL+P/N (PAL/NTSC), CTRL+B/S/F (model), RESTORE safe mode | yes | MISSING (only the C= key that skips libraries exists) | no hit in `run.cc`/`boot.cc`; `libguard.cc` | core app (read keys at start) + decide scope (see Alex list) |
| Libraries `<usbN>/trx/lib/*.u2a` at start | n/a (Gideon has apps, not libs) | PARTIAL: format-3 libs load and register ops/state/events; menu nodes/settings/file actions of a library are kept but NOT merged | `libboard.cc`, `libscan.cc:7`, `libapi.h:9-11`, `run.cc:386-402` | menu merge (D8) |

### 2. Machine

| Requirement | Gideon | Generic status | Evidence | Work to close |
|---|---|---|---|---|
| Menu button opens/closes menu | yes | DONE in code (ITU button 1 / ESP32 rocker), board: no for C64 | `run.cc:509-534`, `hw.cc:420`, `hw.cc:385-405` | Board run |
| SCROLL LOCK / F10 opens; RUN/STOP, Esc closes | yes | DONE | `run.cc:515`, `menuctl.cc:790-801` | none |
| Menu button held >1 s = next disk; CTRL+D = swap disk | yes | MISSING | no hit | library `cbm1541` op `drive.swap` + core app key/long-press |
| Overlay on HDMI (C64 keeps running under the menu) as default | default | MISSING by design: menu always freezes (`ctx.started` => `freeze->enter()`); spec notes Overlay "is out" | `run.cc:434-441`; T104 todo "now that Overlay is out" | none if Alex confirms Freeze-only (consistent with "Freeze UI is main UI") |
| Freeze (stop without bus cycle, picture stays) | Interface Type Freeze | DONE (bench `test_freeze`; board: no). Setting "Freeze mode" Whole machine/CPU only | `freeze.cc:12-47`, `c64.cc:21-40`, `c64_regs.h:21-25` | Board S9/S10 |
| Overlay Transparency 0-75 % | yes | MISSING | no hit | core app (pixel layer alpha) - tiny |
| Reset C64 (menu) | yes | DONE (bench; board: no) | `c64.cc:408` task "Reset" -> `boot_cold_reset` (`boot.cc:141-157`) | Board S10 |
| Reboot C64 (cart + KERNAL cold) | yes | PARTIAL: F8 "Restart core" = reload ROMs + settings + reset | `menutree.cc:112`, `boot.cc:164-179` | label/semantics only |
| Reboot (Clr Mem) | yes | MISSING: `cold_ram_bytes` is 0 in the C64 descriptor, so restart does not clear RAM | `c64.cc:439-464` (field not set), `desc.h:203-206` | C64 descriptor: set `cold_ram_bytes` 65536 (RAM window exists) |
| Power OFF / Power Cycle (menu, C= B, C= K) | yes (control module) | MISSING | no hit | host/control-module access from core app; check bitstream has the path (unverified) |
| Hotkeys C= R, C= Z, C= B, C= K, C= L, C= J, C= I, C= O | yes | MISSING (menu keys are F1-F8, digits, cursors, RETURN/SPACE, BACK, ESC) | `menuctl.cc:741-805` | core app key layer (C= combos) |
| Menu keys: F1/F3/F5/F7 page/action mapping, F2 settings, F4 sysinfo, F6 search, F8 up | Gideon's layout | DIFFERENT by design: F1 Home, F3 Library, F5 C64, F7 Actions, F8 System; no F2/F4/F6 | `menutree.cc:65-118`, `run.cc:49-58` | none (new UI, ADR-0057 3) |
| Reset switch >1 s outside menu | yes | n/a hardware (unverified) | - | - |
| Save C64 Memory (.bin) / Save REU Memory (.reu) | yes | MISSING (`CoreIo::ram_read` exists) | `io.h` | C64 descriptor: op + task + name prompt (S) |
| Joystick Swap / WASD joystick / mouse modes | yes | PARTIAL: USB mouse as 1351 via `mousesink` (POT1351); no joystick settings, no keyboard-as-joystick | `c64.cc:436,461`, `mousesink.cc` | descriptor settings + `joystick_output` hook (S-M) |
| Fast Reset, DMA Load Mimics ID, GAME/EXROM, Bus Sharing | yes | MISSING | no hit | descriptor settings (Fast Reset patches KERNAL+0x1D6C in Gideon: policy) |

### 3. File browser and file types

| Requirement | Gideon | Generic status | Evidence | Work to close |
|---|---|---|---|---|
| Drives: SD, Usb0.., Flash, Temp, Ftp, Net0/WiFi | all | PARTIAL: `/` lists what FileManager mounts (SD, USB; Flash/Temp via same FileManager: unverified); Ftp/Net entries absent | `fm_fs.cc:55-83`, `run.cc:242-266` | core app |
| Only files with a known extension visible | all files shown | DIFFERENT: files without a filetype are hidden and not activatable | `menuctl.cc` `visible_ext`, `activate()` | core app (show all, "View") |
| Enter folder / up / home dir / letter jump / wildcard | yes | PARTIAL: enter, up (BACK/LEFT), HOME key = first row only; no letter jump; no home directory | `menuctl.cc:770-786` | core app |
| Per-file: View/Hex View, Copy to, Move to, Rename, Delete, Select all/none, Copy/Paste, INST delete | yes | MISSING (CoreFs has list/stat/load/save/remove/open only; no copy/move/rename/mkdir; no multi-select) | `fsif.h:68-85`, `fm_fs.cc` | base: file ops service + browser UI (M) |
| Run with App (`/Flash/apps/<EXT>.prg`) | yes | MISSING | - | decide whether kept (REU-load app); library/app |
| `.d64/.g64`: Mount, Mount RO, Mount Unlinked, Run Disk | yes | DONE (bench `test_cbm1541`; board: no) | `cbm1541_desc.h:42-54` | Board run |
| disk image opens like folder; inner PRG: Mount & Run, Real Run | yes | DONE (bench; board: no) | `cbm1541_desc.h:55-58`, `d1541_fs.cc`, `menuctl.cc` `browse_image` | Board run |
| `.g71`, `.d71/.d81/.dnp` | .g71 mounts as 1541; others refused | `.g71` MISSING; `.d81` entry exists in the library but not in the C64 descriptor (1541-only gateware) | `cbm1541_desc.h:69-76` (CBM1581_FILETYPE unused by c64.cc) | `.g71` alias: library (S). `.d81/.d71/.dnp` = drop (10-differences) |
| Load into MP3 Drv A..D | yes | MISSING (depends on REU RAM drives) | - | drop or REU work |
| `.prg/.p00`: Run, Load, DMA | yes (DMA boot cartridge handshake) | PARTIAL: "Run PRG" (reset, wait READY, place, type RUN/SYS), "Load PRG and start from"; no "DMA" (write while menu stays open) | `c64.cc:393-405`, `cbm_run.cc`, `c64_go.cc`. Differs from Gideon (D6: typed RUN, not `bootcrt` path); programs needing his boot-cart path not covered | add DMA action (S); compare behaviour on real programs (board) |
| `.cfg` next to PRG/disk/CRT loaded first | yes | MISSING | no hit | core app (S) |
| `.crt`: Run Cart, Copy to Flash, Write to Cartridge | yes | MISSING (see Cartridges) | - | - |
| `.sid .mus .str`: Play Main Tune, Show Info, Play Song n | yes | MISSING (needs SID player; Gideon's runs 6502 code from DDR) | - | library `.u2a` (L) |
| `.mod` Play, Preload; `.reu` Load, Preload | yes | MISSING | - | REU + Ultimate Audio libraries |
| `.t64` opens like folder | yes | MISSING | - | library (M) |
| `.bin/.rom/.64c`: Set as Char/Kernal/Basic ROM, Load Kernal, Load Drive ROM | yes | PARTIAL: only `.rom` -> "Set as 1541 ROM"; machine ROMs chosen only by file name list; no ROM-choice settings | `cbm1541_desc.h:60-62`, `c64.cc:60-65` | descriptor: ROM select settings / `.bin` actions (M) |
| `.vpl` Apply Palette / Set as Default | yes | MISSING | `io.h` has `palette()` hook (unused by C64) | C64 descriptor + palette op (S-M) |
| `.u2a` Run App | yes | MISSING (no app execution) | `libload.h:63` `main_addr` recorded, never called | see Apps |
| `.cfw/.ue2`: Run Update, Run as Softboot | yes | MISSING | no hit for update in coreapp | see Update |
| `.zip` unzip | via `unzip.u2a` | MISSING | - | app on ABI 3 |
| `.tap` | nothing | drop | 10-differences | - |

### 4. Drive (built-in drive A, 1541)

| Requirement | Gideon | Generic status | Evidence | Work to close |
|---|---|---|---|---|
| Drive power, bus ID 8-11, extra RAM, swap delay 100-1000 ms | yes | DONE (bench, 543 checks; board: no) | `cbm1541_desc.h:19-23`, `cbm1541.cc:151-169` | Board run |
| ROM for 1541 mode (file select) | yes | PARTIAL: first match from a name list; "Set as 1541 ROM" for session | `cbm1541_desc.h:96-98` | descriptor setting for ROM file (S) |
| Resets with C64, Freezes in menu, GCR save align, Leave menu on mount, Geos copy protection, Track Twist | settings | Fixed values, no items: reset/freeze fixed to yes, align yes, twist 2700, Geos copy protection and Leave-menu MISSING | `T104.1 5.7` "Differences from the D15 TOML" | Geos protection: library (S-M); rest decision |
| Turn On/Off, Reset, Remove Disk | yes | DONE ("Drive" setting, Reset Drive, media slot eject) | `cbm1541_desc.h:80,92` | none |
| Save as Binary (D64) / Save as GCR (G64) | yes | DONE (bench) | tasks `Save Disk as D64/G64` | none |
| Insert Blank | yes | MISSING (Create blank D64/G64 exists) | - | library op (S) |
| Create: Directory, D64, G64, D71, G71, D81, DNP image | yes | PARTIAL: blank D64, G64 only (names prompt); no Directory; `t115-soc` adds a folder chooser for Create (T122 5.2, not on main) | `cbm1541_desc.h:78-84`, `newdisk.cc`; `git diff main...t115-soc` menuctl/newfile | Directory create + merge t115-soc; D71/D81/DNP = drop |
| Mount & Run writes drive write-back to file | yes | DONE (`drive.poll` on service tick) | `c64.cc:307-313` | none |
| Drive B, 1571, 1581 | not available | drop | 10-differences | - |

### 5. Cartridges

| Requirement | Gideon | Generic status | Evidence | Work to close |
|---|---|---|---|---|
| Run Cart (internal CRT: Normal, Ocean, Simons, C64GS, Dinamic, Magic Desk, EasyFlash, EF3 all slots, EF XL, MegaByter, 2MegaByter, GMod2, C64MegaCart, AR, RR) | `trx_cart.cc` + descriptor table | INTERFACE ONLY: capability name "cartridge" only; no `.crt` filetype, no parser, no register writes. Gateware side exists (`tools/cart_descriptors/*.json`, T33 spec "built" for gateware+Gideon app) | `c64.cc:4,433`; `T104.2 D9.2` (interface only); no hit for `crt` in coreapp | `c64cart` library `.u2a` (cartridge capability): CRT parser, type table, descriptor writes, image into DDR2 (L) |
| Startup cartridge setting, Copy to Flash | yes | MISSING (and `/flash` writes refused except 3 files) | `fsif.h:30-37` | setting + Alex decision on flash writes |
| REU/UCI/Ultimate Audio off while cart runs | yes | MISSING (ADR-0053 rule not implemented) | caps only | with cart library |
| Save Changed Cartridge Off/Ask/Auto, Save Cartridge | yes (autosave) | MISSING | no hit; PROP-001/T16.2 specs only | cart library + service poll + prompt (L) |
| Real cartridge in port, Cartridge Preference Auto/Internal/External/Manual, Bus Sharing, GAME/EXROM | yes | MISSING (probe `BC_CARTRIDGE` only strikes/keeps the cap) | `board.cc:27-29` | cart library settings (M) |
| `trex_crt` (Dump/Inspector/Flash Cartridge, Write to Cartridge) | `.u2a` ABI 2 | MISSING (see Apps: cannot load; must be rebuilt on ABI 3) | - | app rebuild + needs UI API (L) |

### 6. Settings (each page; Gideon = sectors 0xFF0000.., generic = `/flash/trx64u/c64.cfg`, only changed items written)

| Page | Effective in manual | Generic status | Work to close |
|---|---|---|---|
| Video Configuration: System Mode PAL/NTSC/PAL-60/NTSC-50 (+/L), HDMI Scan lines, Palette | yes (Resolution/Analog/Digital = no effect: drop) | MISSING for C64 timing/scanlines/palette. The application's own `Picture` item picks the HDMI mode (and keeps the board's PAL/NTSC `sys`), not the C64's video standard | `picture.cc:80-98`; descriptor has no system-mode setting | C64 descriptor: settings on the existing Gideon registers (S-M) |
| Audio Mixer (21 items), Speaker Mixer | volumes/pans effective except Drive/Tape | MISSING | descriptor settings + registers (M) |
| LED Lighting: Power LED outputs | effective (Case/Keyboard lights = no effect) | MISSING | descriptor settings (S) |
| Memory Configuration: ROM files, Cartridge, Save Changed Cartridge, REU + size, Command Interface, Ultimate Audio, Drive A ROM | effective | MISSING except drive ROM (name list). REU/UCI/UltiAudio: capabilities only, no ops | settings + REU/UCI/sampler libraries (L) |
| Cartridge and ROM Settings (Preference, Bus Sharing, GAME/EXROM, Fast Reset, REU Preload, DMA Load Mimics) | effective (Bus Operation Mode = no effect) | MISSING | see Cartridges |
| Turbo Settings (control, CPU speed 1-64 MHz, badline, SuperCPU detect) | effective | MISSING | descriptor settings (S-M, registers exist in the U64 map: unverified here) |
| Machine Tweaks / Power Settings | C64U Model effective; Power On After Power Loss, Wake On Wi-Fi effective | MISSING | needs control-module path (M, unverified) |
| User Interface Settings (interface type, look, colours, nav style, home dir, auto save, squeeze, temp) | effective | MISSING (one look: LVGL prototype skins; no home dir/auto-save choice; settings write on every change) | `trxview.cc` skins; decide which to keep |
| SID Sockets Configuration / SID Addressing / reSID8 page / SID Player Behavior | effective (8 SIDs, filter curves) | MISSING (cap `sid_sockets` only; probe answered by profile, not read) | `board.cc:38-40` | descriptor settings + SID detect (L) |
| Drive A Settings | see Drive | PARTIAL | |
| Joystick Settings (input mode, paddle, mouse mode/sensitivity/accel/wheel, menu mouse nav) | effective | PARTIAL: mouse is 1351 only; no settings; menu mouse navigation intentionally removed (mouse reaches the core only) | `run.cc:358-363` | descriptor settings (M) |
| Network Settings (host name, ID, password, services on/off, syslog, SNTP/timezone), Ethernet, WiFi | effective | MISSING (no network UI; FTP daemon reads Gideon's `networkConfig` defaults; ESP32 only as rocker link) | `run.cc:208-221`, `hw.cc:256-405` | base network settings tree (M-L) |
| Data Streams targets | effective (Debug stream = no effect) | MISSING | with streams |
| Auto Save Config (No/Ask/Yes), Save to Flash, Save to File, Reset from Flash, Reset to Defaults, Clear Flash Config | effective | PARTIAL: settings saved on change; F8 "Reset to host defaults" removes the cfg; no save-to-file, no per-file `.cfg` load | `menuctl.cc:706-716`, `settings.cc:105-` | core app (S) |
| `/v1/configs` hidden items (UserPort Power, Adjust Color Clock) | no effect | drop | | |

### 7. Audio (summary)

reSID8 chip model/filter/curve/digis, mixer, sockets, sampler/Ultimate Audio, UCI ($DF1C): all MISSING; no register block, op or setting in coreapp; capability probes only (`board.cc:30-37`: `BC_REU` hard-set, `BC_UCI` from FPGA caps). SID/MOD/MUS playback: MISSING. Work: descriptor settings (S-M) + libraries for REU, UCI, sampler, SID player (L). Drive/tape sound = drop.

### 8. Video

PAL/NTSC, palette, scanlines: MISSING (section 6). Own-picture video (`video.w==0`) DONE bench (`test_video`). Menu on HDMI pixel layer at 1080p scale: DONE in code, board: no. HDMI resolution/analog/DVI = drop.

### 9. Turbo

MISSING entirely (no turbo registers in `c64_regs.h`, no setting). Work: C64 descriptor (S-M).

### 10. Timewarp

| Requirement | Generic status | Evidence | Work |
|---|---|---|---|
| C64 Machine > Timewarp bar (Freeze menu), keys, continue-from-frame | INTERFACE ONLY: `ring` capability from probe; no ops, no bar | `board.cc:31-34`; `c64.cc:433`; T104.2 D9.5 "interfaces only"; no hit for rewind/scrub/timewarp in coreapp except Atari tape | core app view + ring ops via `CoreIo::rd/wr` beyond page 0, `trx_ring_unfreeze` sequence (Gideon `c64.cc:1666-1729`) (M-L) |
| Ring off while REU on | MISSING | - | with REU |
| REST `/timewarp:*` | MISSING | - | with REST |

### 11. Monitor trxmon + RPC

MISSING. No monitor in the base; `c64_go.cc` uses the debug block (page 3) only to set PC. trxmon is a separate `.u2a` on ABI 2 using the `trx` and `ring` api tables (`TRX64-Ultimate software/ultimate-app/software/app_loader/app_api.h:306-512`); TCP 4312 JSON-RPC for C64RE needs the net table. C= O / Developer > TRX64 Monitor entries MISSING. Work: ABI 3 with UI/net/trx tables, rebuild trxmon (L). Day-1 item.

### 12. Apps (.u2a ABI)

| Requirement | Generic status | Evidence |
|---|---|---|
| Load Gideon's apps unchanged (trxmon, trex_crt, softboot, unzip: format 2, ABI major 2.1, `app_main(api, ctx)` with area tables sys/mem/ui/fs/c64/media/config/diag/net/trx/ring) | NO. Loader reads format 3 only, ABI major 3: `"Old file format 2 ... Rebuild the file."` | `libload.cc:123-125,137-140`; `libfmt.h:32` (`U2A_ABI_MAJOR 3`); Gideon `app_format.h:18` (format 2), `app_api.h:38-39` |
| Run an app (call `app_main`) | NOT BUILT: `main_addr` is recorded, never called; `LibApi` has only `log` | `libload.cc:300`; `libapi.h:18-22`; no `app_main` call anywhere |
| ABI tables for apps | NOT BUILT: ABI 3 `svc` table (call/get/subscribe, UI, fs, net) is a decision (T104.1 Q6 "ja, apps then only for our cores"), no code | `libapi.h`, T104.1 Q6 |
| Install to `/Flash/apps`, registry `/Flash/config`, copy-to-flash prompt, per-start registration | MISSING; and the base refuses `/flash` writes (only 3 paths allowed) | `fsif.h:30-37`; apps discovery in generic app = `<usb>/trx/lib` only (`libscan.cc:7`) |
| Menu entries from app manifests (category, filetype ext) | MISSING (library nodes/file actions not merged; manifest carried but unused) | `libapi.h:9-11` |
| softboot (boot request from RAM), unzip, trex_crt, trxmon | all MISSING; each needs rebuild on ABI 3 | - |
| Messages ("not a valid app file", "needs newer firmware", ...) | MISSING | - |

Work: ABI 3 tables + app runner + manifest->menu merge (L), then rebuild four apps (trxmon L, trex_crt L, softboot M, unzip S) or replace by libraries.

### 13. Network

| Requirement | Gideon | Generic status | Evidence | Work |
|---|---|---|---|---|
| Ethernet / WiFi link | yes | PARTIAL: Gideon's network_interface + rmii linked, IP shown on Home; no settings page; WiFi/ESP32 used only for rocker events (link identified at boot); WiFi AP selection MISSING | `Makefile`, `run.cc:208-221`, `hw.cc:385-405` | settings tree + WiFi UI (M) |
| FTP (21, passive) | yes | PARTIAL (inherited): `ftpd.cc` registers itself by `InitFunction`; uses Gideon's `networkConfig` defaults; no UI, board: no, flash-write refusal applies only to CoreFs, not to FTP (unverified) | `host-app/software/network/ftpd.cc:218-221,1252` | board check + settings |
| Telnet 23 (menu in terminal) | yes | MISSING | no hit | base adapter for the LVGL menu (M) |
| REST API (all routes in manual 09) | yes | MISSING: no HTTP server. Spec: REST in the base (Q8), day 1 REST only, Gideon-path alias table (Q2), nothing built | T104.1 Q2, Q8; Makefile has no `httpd` | L: service-layer adapter + alias table for `/v1/*`: `machine:*`, `runners:*`, `drives/*`, `files/*`, `configs*`, `streams`, `apps`, `timewarp` |
| DMA service TCP 64 | yes | MISSING (`dma_uart.cc` is the ESP link, not this) | - | M |
| Ident / discovery UDP 64 incl. `trx64` block | yes | MISSING | - | S-M |
| SNTP / timezone / clock | yes | MISSING: only `get_fattime` hook; clock unset => 1980 (unverified) | `hw.cc:135-140` | S |
| Syslog | yes | MISSING (console printf only) | - | S |
| Streams VIC/Audio (UDP multicast) | yes | MISSING | - | M |
| Network password | yes | MISSING (FTP reads Gideon's config key; no way to set it) | | S |
| Services on/off | yes | MISSING | | S |

### 14. Update

| Requirement | Gideon | Generic status | Evidence | Work |
|---|---|---|---|---|
| Run Update of `.cfw/.ue2` (load record to 0x03000000, jump into updater; updater writes 0x0, 0x220000, ESP32, roms, html; config reset prompt) | `filetype_u2p.cc:76-166`, `update_u64ii.cc:164-195` | MISSING. Spec: one op `update.run_record` (T104.2 D9.3), Gideon's updater stays the writer; NOT BUILT. Generic app has no `.cfw` filetype | no hit; T104.2 point 9 (rollback must work before first flash write) | core app op + filetype (M); board S13 |
| Safety net: Gideon's original stays installable | decided | NOT TESTED | ADR-0057 consequences | S13 |
| Backup/restore of flash (settings) | n/a in manual | MISSING (no flash dump/restore in app) | - | decide |
| `.cfw` layout T115 expects | host starts a `.cfw` from the **host menu**: Gideon pre-header `DE AD 15 41`, family U64MK2, 50T `.bit` (IDCODE check), app record `{load,length,start}` with load/start 0x30000; TRX64 monolith `.cfw` same layout, identity map when bitstream writes AXSS | T115 is host-side only; "no change to the generic application" | `t115-soc`: `docs/specs/T115-full-bitstream-cores.md` section 4 D1-D4, section 2 S5 | The generic app on the monolith has no host; decision needed on how the monolith's own `.cfw` carries the generic `.app` (must stay 12 + length <= 0x1E0000) |
| Softboot (run firmware from RAM, no flash write, same-bitstream check) | `softboot.u2a` | MISSING | | app rebuild (M) |
| Note t115-soc | `cores/trx64` and `cores/c64u` leave the MultiCore repo (decision 2026-10-25): TRX64 firmware comes from TRX64-Ultimate as `.cfw`. The generic app source stays in MultiCore (ADR-0057 5) | | `t115-soc` spec section 7 | none |

### 15. Items marked "drop" (no effect / not available per manual 04 and 10)

Drive B, 1571, 1581, `.d71 .d81 .dnp`, tape port and `.tap`, tapecart, software IEC drive, printer, UltiCopy, modem/SwiftLink, built-in machine-code monitor (replaced by trxmon), UltiSID filter tables, analog video, HDMI resolution/DVI, case/keyboard light effects (SHIFT LOCK LED stays), drive/tape sound, debug and IEC streams, serial bus mode / parallel cable / burst patch, user port, RTC, Pause/Resume/bus measurement/developer pokes, Bus Operation Mode, Ulticopy Uses disk name, Stream Debug items, HDMI Scan Resolution, `/v1/configs` UserPort Power Enable and Adjust Color Clock, "MODEMS, PRINTERS, BUILT-IN DRIVE B" main entries, Load into MP3 Drv A..D only if REU RAM drives are dropped (decision).

### 16. Day-1 list (ADR-0057 7) against the code

| Day-1 item | Status |
|---|---|
| Firmware update and flash | MISSING (no update op, no flash install, rollback untested) |
| Browse files; start `.d64` | DONE bench (Mount / Run Disk / Mount & Run); board: no |
| Start `.prg` | DONE bench (Run PRG, Load PRG and start from, $0801-$FFFF); board: no |
| Start `.crt` | MISSING (interface only) |
| Reset | DONE bench; board: no |
| Freeze | DONE bench; board: no |
| "Leaving every state by menu" | PARTIAL: menu always reachable (button/F10/SCRLOCK), F8 Restart core; not exercised on board (unverified) |
| Network and REST enough for tests and deploy | MISSING (no REST/telnet/DMA; FTP inherited, unverified) |
| Timewarp | MISSING (interface only) |
| Monitor | MISSING |

---

