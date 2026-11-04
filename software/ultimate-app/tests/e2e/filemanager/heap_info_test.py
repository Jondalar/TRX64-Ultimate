#!/usr/bin/env python3
"""E2E: System Info shows the heap, and the Commodore menu does not leak.

SPEC-06 R1 and R2 (doc/specs/SPEC-06-robustness.md), from Spiffy's 7d930f2c
and d37c3b70.

- System Info (F4) lists the FreeRTOS heap: total, free, low-water mark.
  malloc and new are wrapped onto that heap, so these are the figures that
  matter; newlib's mallinfo(), which Spiffy used, would not see it.
- Opening and leaving a settings page from the Commodore main menu used to
  leave the page behind on the heap every time. With the heap figures on
  screen, a leak shows as free memory that goes down and stays down.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

# The one stanza that puts the shared library on sys.path; see tests/lib/bootstrap.py.
sys.path.insert(0, str(next(p for p in Path(__file__).resolve().parents
                            if (p / "tests" / "lib").is_dir()) / "tests" / "lib"))
import bootstrap  # noqa: E402,F401

sys.path.insert(0, str(Path(bootstrap.__file__).resolve().parents[1] / "e2e" / "lib"))

import carttool                                                 # noqa: E402
from emulator import Emulator, emulator_binary, firmware_image  # noqa: E402
from report import (Failure, check, detail, format_exception,   # noqa: E402
                    suite_fail, suite_ok, suite_skip)

SUITE = "heap_info_test"
ROUNDS = 30
TOLERANCE_KIB = 1          # free memory may move by a block while the menu runs


class SkipSuite(Exception):
    pass


def screen(emu: Emulator) -> str:
    return "\n".join(emu.c64_rows())


def heap_free(emu: Emulator) -> int:
    """Open System Info (F4), read "Heap free", close it."""
    emu.key("f4", settle=1500)
    text = screen(emu)
    for _ in range(12):
        match = re.search(r"Heap free:\s+(\d+) KiB", text)
        if match:
            break
        emu.key("down", settle=100)
        text = screen(emu)
    emu.key("runstop", settle=1000)
    if not match:
        raise Failure("System Info shows no 'Heap free' line")
    return int(match.group(1))


def run_mode(mode: str) -> None:
    emu = Emulator(board=carttool.BOARD_FOR_MODE[mode], settings=carttool.settings_for(mode))
    emu.start()
    try:
        carttool.freeze_into_menu(emu, mode)

        with check(f"{mode}: System Info shows the heap"):
            emu.key("f4", settle=1500)
            text = screen(emu)
            for _ in range(12):
                if "Heap low water" in text:
                    break
                emu.key("down", settle=100)
                text += "\n" + screen(emu)
            emu.key("runstop", settle=1000)
            for line in ("Heap total:", "Heap free:", "Heap low water:"):
                if line not in text:
                    raise Failure(f"System Info has no {line!r} line")
            total = int(re.search(r"Heap total:\s+(\d+) KiB", text).group(1))
            free = int(re.search(r"Heap free:\s+(\d+) KiB", text).group(1))
            if not (0 < free < total):
                raise Failure(f"heap free {free} KiB of {total} KiB")
            detail(f"heap: {free} KiB free of {total} KiB")

        if mode == "commodore":
            with check(f"commodore: {ROUNDS} settings pages leave the heap as it was"):
                # freeze_into_menu went on into the file browser: back to the main menu
                for _ in range(4):
                    if "VIDEO SETUP" in screen(emu):
                        break
                    emu.key("runstop", settle=700)
                before = heap_free(emu)
                rows = carttool.wait_for_entry(emu, "VIDEO SETUP", "the Commodore menu")
                carttool.step_to(emu, rows, "VIDEO SETUP", "the Commodore menu")
                for _ in range(ROUNDS):                 # the cursor stays on the entry
                    emu.key("return", settle=700)
                    emu.key("runstop", settle=700)
                after = heap_free(emu)
                detail(f"heap free {before} KiB before, {after} KiB after {ROUNDS} pages")
                if before - after > TOLERANCE_KIB:
                    raise Failure(f"{before - after} KiB gone after {ROUNDS} settings pages")
    finally:
        emu.stop()


def run(args) -> None:
    try:
        emulator_binary()
        firmware_image()
    except Failure as exc:
        raise SkipSuite(str(exc)) from None
    for mode in (carttool.UI_MODES if args.mode == "all" else (args.mode,)):
        run_mode(mode)


def main() -> int:
    parser = argparse.ArgumentParser(description="Check the heap figures and the Commodore menu for leaks.")
    parser.add_argument("-m", "--mode", default="all", choices=(*carttool.UI_MODES, "all"))
    args = parser.parse_args()
    try:
        run(args)
    except SkipSuite as exc:
        suite_skip(SUITE, str(exc))
        return 0
    except Exception as exc:            # noqa: BLE001
        suite_fail(SUITE, format_exception(exc))
        return 1
    suite_ok(SUITE)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
