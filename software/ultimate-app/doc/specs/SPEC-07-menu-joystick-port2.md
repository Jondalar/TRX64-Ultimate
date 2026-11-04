# SPEC-07 Menu control from joystick port 2

| | |
|---|---|
| Status | Done in the freeze menu (branch `feature/spiffy`); overlay path untested |
| Boards | **C64U only** |
| TODO | S10 |
| Source | Spiffy `36b3a928` |

## Requirements

- **R1** The menu can be driven with a joystick in either port, not only the one the
  joystick swapper currently routes to the menu.
- **R2** Only the C64U: the swap is `U64II_KEYB_JOY`. On the U64 the menu reads
  `C64_PLD_PORTA` and swapping exists only on Elite boards (`u64_config.cc:1094`);
  nothing changes there.
- **R3** Our joystick scan differs from Spiffy's base (`JoystickOutput`,
  `keyboard_c64.cc:248-270`): the change is written against ours, not merged.

## Acceptance

| # | Check | Where |
|---|---|---|
| A1 | Menu moves with a joystick in port 1 and in port 2, swapper in both positions | C64U hardware |
| A2 | Joystick input to a running program unchanged after leaving the menu | C64U hardware |

## Implementation

`Keyboard_C64::scan` (`keyboard_c64.cc`) reads the other port while the first is idle.
In the freeze menu the keyboard is the C64's CIA (`c64.cc:224`): port 2 on port A,
port 1 on port B while no row is selected. On the overlay the line is
`U64II_KEYB_JOY`, written to select a port and read for its state; the chosen port
is kept in `keyb_joy_select`. Test: `tests/e2e/filemanager/menu_joystick_test.py`
(freeze menu, both ports; ue2emu 3d67d24 or later). The overlay path has no test:
the emulator suites run the freeze menu.
