#!/usr/bin/env python3
"""E2E: a joystick in either port drives the menu (C64U / U64-II).

SPEC-07 (doc/specs/SPEC-07-menu-joystick-port2.md), from Spiffy's 36b3a928.
The menu's joystick line reads the port the swap setting chose; a stick in
the other port used to do nothing there. Fire on the first entry of the
Commodore main menu opens the file browser, which shows on the screen, so
fire is the move used: once from the port the line reads (port 2 with the
swapper on Normal) and once from the other one.

Needs ue2emu with joystick input (`joy <port> <dirs>`, main 3d67d24 or
later); on an older emulator the suite reports SKIP.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

# The one stanza that puts the shared library on sys.path; see tests/lib/bootstrap.py.
sys.path.insert(0, str(next(p for p in Path(__file__).resolve().parents
                            if (p / "tests" / "lib").is_dir()) / "tests" / "lib"))
import bootstrap  # noqa: E402,F401

sys.path.insert(0, str(Path(bootstrap.__file__).resolve().parents[1] / "e2e" / "lib"))

import carttool                                                 # noqa: E402
from emulator import Emulator, emulator_binary, firmware_image  # noqa: E402
from report import (Failure, check, format_exception,           # noqa: E402
                    suite_fail, suite_ok, suite_skip)

SUITE = "menu_joystick_test"
MAIN = "DISK FILE BROWSER"


class SkipSuite(Exception):
    pass


def screen(emu: Emulator) -> str:
    return "\n".join(emu.c64_rows())


def joy(emu: Emulator, port: int, dirs: str) -> None:
    try:
        emu.control.command(f"joy {port} {dirs} 120")
    except Failure as exc:
        if "unknown command" in str(exc) or "unknown" in str(exc):
            raise SkipSuite(f"the emulator has no joystick input yet ({exc})") from None
        raise
    emu.wait(1200)


def back_to_main(emu: Emulator) -> None:
    for _ in range(4):
        if MAIN in screen(emu) and "Temp" not in screen(emu):
            return
        emu.key("runstop", settle=800)
    raise Failure("the Commodore main menu did not come back")


def run() -> None:
    emu = Emulator(board="c64u", settings=carttool.settings_for("commodore"))
    emu.start()
    try:
        emu.button()
        carttool.wait_for_entry(emu, MAIN, "the Commodore menu")
        for port, what in ((2, "the port the menu line reads"), (1, "the other port")):
            with check(f"fire on port {port} ({what}) opens the first entry"):
                back_to_main(emu)
                joy(emu, port, "fire")
                carttool.wait_for_text(emu, "Temp", f"the file browser after fire on port {port}",
                                       tries=5)
    finally:
        emu.stop()


def main() -> int:
    argparse.ArgumentParser(description="Check the menu joystick on both ports.").parse_args()
    try:
        emulator_binary()
        firmware_image()
        run()
    except SkipSuite as exc:
        suite_skip(SUITE, str(exc))
        return 0
    except Failure as exc:
        if "no ue2emu" in str(exc) or "no firmware" in str(exc):
            suite_skip(SUITE, str(exc))
            return 0
        suite_fail(SUITE, format_exception(exc))
        return 1
    except Exception as exc:            # noqa: BLE001
        suite_fail(SUITE, format_exception(exc))
        return 1
    suite_ok(SUITE)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
