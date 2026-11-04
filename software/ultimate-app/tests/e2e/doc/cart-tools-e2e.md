# Cart tools E2E

The two TREX cart tools read and program a board in the expansion port. The
other cartridge suites mount a `.crt` file and let the firmware's own
cartridge emulation present it to the C64, so the port is not involved — and a
device cannot be told to have a board in it anyway. These suites run the
firmware in ue2emu, which can.

    U64_CART_SLOT=emulator ./run-tests            # or run either file directly
    U64_CART_SLOT=emulator python3 tests/e2e/cart/dumper_test.py -m all --shots shots/

Without the variable both suites report SKIP with what they would need, so a
bench that has no emulator is not held to them.

## What was added

| File | What it is |
| --- | --- |
| `tests/e2e/lib/emulator.py` | A ue2emu process and its control port. Settings are written into the flash before the firmware runs, so a suite states the configuration it needs instead of navigating to it through the menu it is about to test. |
| `tests/e2e/lib/cartslot.py` | The board in the port, and the CRT images the suites build. |
| `tests/e2e/lib/carttool.py` | Freeze, walk the browser to the app, run it, and read the screen a cart tool draws. |
| `tests/e2e/cart/dumper_test.py` | 4 checks per interface. |
| `tests/e2e/cart/writer_test.py` | 4 checks per interface, plus the erased board. |

Three things the emulator gives that hardware does not, and all three are why
these suites exist at all:

- **A board in the port.** `--cart-slot FILE.crt[,rw|,save=OUT.crt]`, and
  `cart-info` says what it is and whether it asserts its lines.
- **The C64 screen as text.** Apps run only while the machine is stopped
  (`app_loader.cc:241`), so a cart tool always draws on the C64 screen, never
  on the HDMI overlay. `c64screen` returns it as 25 rows of text, which is
  what every assertion here is written against.
- **A screenshot.** `png` writes the VIC frame, which is that same screen.
  Shots are written on every run, pass or fail, and named after the step:
  `dumper-commodore-02-scanned.png`.

## What the suites check

The tools are the port of the firmware's own Cart Inspector, dumper and
writer (`apps/trex_crt/src`, from `feature/trex-crt-app`). A plain Run App
shows a chooser; "Write to Cartridge" in the context menu of a CRT hands the
writer that file. The suites use both ways in.

**Dumper** (4 checks per interface). The screen it draws -- the frame, the
four legend rows beside the first map, the status label, the key line, and
that no row runs past the frame. Three layout faults turned up while that
screen was being built (a status line over the legend, a line that wrapped
and ate the row below, a second frame nested in the first), none of which
stops the tool working, so only a screen check finds them. Then F1 scans, F3
dumps through the type question, the path picker and the name box, the screen
is whole again afterwards, and the file holds all 64 banks of the board, the
erased ones included, byte for byte where the board has data.

**Writer** (4 checks per interface, plus one). "Write to Cartridge" opens the
writer on that file; F3 erases and writes, without a second question; F5 verifies;
and the board's saved image is compared against the CRT, ROML and ROMH, bank
by bank. Once, not per interface: an erased board is still found as a flash
cartridge.

**Both interfaces, every time.** F1 is `KEY_PAGEUP` in the Ultimate interface
and `KEY_TASKS` in the Commodore one. The tools switch on the physical key
(`ui_fkey_pressed()` in `ui_shim.c`, over `ui_fkey()` from the firmware), and
running every check in both interfaces is what keeps it that way. Before it,
F7 (select slot) reached the writer as F3 (write) in the Commodore interface.

## What the suites found

- **A chip erase never finished against the emulator.** A DMA byte cost no
  C64 time, so the erase alarm of the flash model was never reached. Fixed on
  the emulator side (`d53bd46`, a byte now costs 3.2 C64 cycles, measured on
  a C64U at 3.26 µs).
- **A dialog left the tool's screen behind half drawn.** A popup restores the
  screen it found when it closes, and that was not the tool's. The tools now
  open their frame again after every dialog instead of repainting into the
  old window.
- **A board that switches itself off reads back as nothing.** A test cartridge
  whose own program disables the cartridge reads `cart_detect` 3. The suites
  check it first and say so rather than reporting a wrong probe as a tool
  defect.

## Notes for extending them

- The row a cursor sits on is colour, which the control port's text does not
  carry. The cursor is on the first entry whenever a list has just been drawn,
  so `carttool.step_to` counts down to a row found by name. `entry_rows`
  leaves out the firmware's title, the rule or border, the path and key hint,
  and a picker's own centred heading — counting any of those puts the cursor
  that many rows past what was asked for.
- The two interfaces do not draw the same browser: the Commodore one frames
  every list, the Ultimate one only the ones that ask. The frame is stripped
  where there is one rather than required.
- A single keystroke sent while the UI is still drawing is occasionally read
  by nothing, so `press_until` presses again where the screen has not changed.
  A USB stick takes a moment to enumerate, so `wait_for_entry` re-reads the
  list rather than assuming it complete on the first look.
- Waits are emulated milliseconds, not wall clock: a full run of both suites
  in both interfaces takes about fifteen seconds.
