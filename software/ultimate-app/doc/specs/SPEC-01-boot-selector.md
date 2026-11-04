# SPEC-01 Boot selector

| | |
|---|---|
| Status | Draft |
| Boards | U64, C64U (`#if U64`) |
| TODO | T2 |
| Architecture | `doc/TREX_Launcher_Architecture.html` (sections 4, 6.2, 9) |

## Purpose

A screen in front of the interfaces, like GRUB: it chooses which interface the
machine runs — TREX, Ultimate or Commodore. It is shown when asked for, and
otherwise the machine boots the last choice without stopping.

## Terms

- **Interface**: the root view the firmware builds for the menu (today
  `TreeBrowser` for Ultimate, `CommodoreMenu` for Commodore, `ultimate.cc:147-183`).
- **Selector**: this screen.
- **Choice**: the interface the selector last booted.

## Requirements

### Showing the selector

- **R1** Holding the **C=** key while the firmware boots shows the selector.
- **R2** Without C=, the firmware boots the stored choice without showing anything
  (same boot time as today, G4 of the architecture).
- **R3** A setting **"Boot Selector"** with the values `With C= key` (default) and
  `Always` makes the selector appear at every boot.
- **R4** With `Always`, the selector boots the highlighted choice after a timeout
  (see open question Q3); any key stops the countdown.
- **R5** The selector is shown once per boot of the firmware, not at a C64 reset.

### Choosing

- **R6** The selector lists the interfaces that exist on the board: `TREX`,
  `Ultimate`, `Commodore`. The stored choice is highlighted when it opens.
- **R7** Cursor up/down, RETURN to boot, joystick in port 2 up/down/fire the same
  (port 1 as well where SPEC-07 applies).
- **R8** The choice is stored in the settings (flash) only when it differs from
  the stored one.
- **R9** The existing setting "User Interface" (`CFG_USERIF_UI_MODE`, values
  Automatic / Ultimate / Commodore, `userinterface.cc:131,138`) becomes the stored
  choice and gains the value `TREX`. "Automatic" keeps its meaning (board decides:
  Commodore on a C64U, Ultimate otherwise, `userinterface.cc:319-322`).

### Coming back to the selector

- **R10** The freeze menu and the overlay menu of every interface offer
  **"Switch Interface…"** in the task menu (C64 Machine category).
- **R11** "Switch Interface…" asks for confirmation, stating that the running C64
  program ends, then restarts the firmware with the selector shown once
  (a one-shot flag, not the "Always" setting).

### Separation

- **R12** Only the chosen interface's root objects are constructed. Code of the
  others stays linked in (variant S) or is not loaded (variants M/K, open in the
  architecture, D6).
- **R13** TREX code lives under `software/trex/`. Upstream files get only the
  selector call and the construction switch in `ultimate.cc`, the new enum value,
  and the task-menu entry.

### Reading C= at boot

- **R14** The firmware already scans the C64 keyboard at boot through CIA1
  (`u64_config.cc:965-985`, `Keyboard_C64::scan_keyboard`). That function returns a
  key code only; a modifier alone returns the "no key" code of the map
  (`keyboard_c64.cc:193-207`). The selector needs a scan that returns the modifier
  bits (`shift_flag`), e.g. a `scan_modifiers()` beside it.
- **R15** The existing boot hotkeys keep working: Ctrl-P / Ctrl-N (system mode),
  Ctrl-B / Ctrl-S / Ctrl-F (C64U model). C= alone must not collide with them;
  which modifier bit the C= key sets, versus the CTRL key, is to be verified
  (Q1).
- **R16** A USB keyboard held key at boot is not required (Q5).

## Display

- **D-R1** Drawn by the firmware, no C64 program (architecture D0).
- **D-R2** Must work without HDMI (C64 video only). Which host draws it — frozen C64,
  overlay, or before `c64->start()` — is architecture decision D1; this spec
  holds for all three.
- **D-R3** Look: the TREX theme row (architecture section 8). Minimum: title, the
  list, one help line (`↑↓ choose · RETURN boot · countdown n s`).

## Settings (new)

| Setting | Page | Values | Default |
|---|---|---|---|
| Boot Selector | User Interface Settings | With C= key / Always | With C= key |
| Selector Timeout | User Interface Settings | 0 (none), 1–30 s | 5 s (Q3) |
| User Interface | User Interface Settings | Automatic / Ultimate / Commodore / **TREX** | Automatic |

## Non-goals

- The content of the TREX interface itself (its own spec, after D6).
- Kickstart / loading modules (architecture variant K, T3/T4).
- U2, U2+, U2+L.

## Acceptance

| # | Check | Where |
|---|---|---|
| A1 | Boot without keys: stored choice runs, no selector, boot time unchanged (±5 %) | emulator + both boards |
| A2 | C= held from power-on: selector shows, stored choice highlighted | emulator (key held before boot, if the emulator supports it — Q6), both boards |
| A3 | Choosing another interface boots it and stores it; next boot without keys runs it | emulator |
| A4 | "Always" + timeout: boots the highlighted entry after n s; a key stops the countdown | emulator |
| A5 | "Switch Interface…" from the freeze menu: confirmation, restart, selector once, then normal | emulator |
| A6 | Ctrl-P / Ctrl-N / Ctrl-B at boot still do what they do today | both boards |
| A7 | Commodore and Ultimate interfaces behave exactly as before when chosen | existing e2e suites in both modes |

## Open questions

- **Q1** Which modifier bit does the physical C= key set in `modifier_map`, and is
  it distinct from CTRL on both keyboards (C64U keyboard, U64 with a C64 keyboard)?
- **Q2** Host of the selector (architecture D1).
- **Q3** Timeout default with "Always": 5 s, or none (wait for a key)?
- **Q4** "Switch Interface…": restart the firmware (simple, loses the program) or
  rebuild the interface at runtime (architecture D7)?
- **Q5** A key on a USB keyboard as an alternative to C=?
- **Q6** Can the emulator hold a key from power-on? Otherwise A2 is a hardware check.
