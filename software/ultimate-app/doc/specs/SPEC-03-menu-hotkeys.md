# SPEC-03 Menu hotkeys

| | |
|---|---|
| Status | Done (branch `feature/menu-hotkeys`) |
| Boards | U64, C64U (power cycle C64U only) |
| TODO | S1, S2 |
| Source | Spiffy `c0c7a589` (hotkeys), `5571b3de` (home hotkey) |

## Purpose

Machine actions from the keyboard while the menu is open, and the browser's
hotkeys also in the Commodore main menu.

## Keys already taken (checked, `tree_browser.cc:440-590`)

C= A select all · C= C copy · C= V paste · C= N select none · C= I · C= J swap
joysticks · C= L log · C= O machine monitor · C= R reset C64 · HOME / C= HOME
home directory. At boot only: Ctrl-P, Ctrl-N, Ctrl-B, Ctrl-S, Ctrl-F
(`u64_config.cc:965-985`).

## Requirements

- **R1** New hotkeys, active whenever a menu of either interface has focus
  (browser, Commodore main menu, settings, context menu, editor):

  | Key | Action | Board | Spiffy key |
  |---|---|---|---|
  | C= R | Reset C64 (exists, now in every menu) | both | C= X |
  | C= Z | Reboot C64 | both | C= Z |
  | C= B | Power cycle | **C64U only**; on U64 no action and no message (`c64_subsys.cc:389-394`) | C= B |
  | C= K | Power off | both (`U64_POWER_REG`, `c64_subsys.cc:396-402`) | C= O — taken by our monitor |

- **R2** C= X stays unused (Spiffy's reset key); C= R is our reset already.
- **R3** C= J and C= L work in the Commodore main menu as in the browser; HOME opens
  the file browser there, in the home directory when one is set
  (`commodore_menu.cc`). C= HOME (set home) stays a browser key: the main menu has
  no current directory to set. The Commodore interface runs on both boards
  (`ultimate.cc:143`).
- **R4** Power off and power cycle ask for confirmation (Q1); reset and reboot do
  not (as C= R today).
- **R5** The help screen of each interface lists the keys; the theme's help text
  (`t_ui_theme::help_fkeys`) stays for F-keys, a second block lists C= keys.

## Acceptance

| # | Check | Where |
|---|---|---|
| A1 | Each key in each menu type triggers its action (C= R/Z) or its confirmation (C= B/Q) | emulator, both UI modes |
| A2 | C= B on U64: nothing happens, no crash | U64 hardware |
| A3 | C= O still opens the machine monitor | emulator |
| A4 | C= J / C= L / HOME in the Commodore main menu | emulator, commodore mode |

## Decisions taken

- **Q1** Power off and power cycle ask first (Yes/No); reset and reboot do not.
- **Q2** Power off is **C= K**. C= Q cannot be used: C=+Q is 0x11, which is RUN/STOP
  (`keymap_control`, `keyboard_c64.cc`). Also taken by the keymap: C= S (HOME),
  C= H (DEL), C= M (RETURN); C= D is disk swap in the main loop.

## Implementation

`machine_hotkey()` in `software/userinterface/machine_hotkeys.h`, called first by
`TreeBrowser`, `ConfigBrowser`, `ContextMenu` (and so `CommodoreMenu`, `TaskMenu`)
and `Editor`. New key codes `KEY_CTRL_K` (0x0B), `KEY_CTRL_Z` (0x1A). Test:
`tests/e2e/filemanager/menu_hotkeys_test.py`, 4 checks in the Ultimate interface and
7 in the Commodore one (needs ue2emu with key chords, main 89ef413 or later; SKIP
on an older one). The settings help lists the keys too.
