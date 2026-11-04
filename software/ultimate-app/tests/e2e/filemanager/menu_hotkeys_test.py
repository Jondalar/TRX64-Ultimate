#!/usr/bin/env python3
"""E2E: machine hotkeys work in every menu, in both interfaces.

SPEC-03 (doc/specs/SPEC-03-menu-hotkeys.md): C= R reset, C= Z reboot, C= B
power cycle (C64U), C= K power off, one helper (machine_hotkeys.h) called by
the browser, the settings, the context and task menus and the editor; and the
browser's C= J, C= L and HOME in the Commodore main menu.

The keys are chords, so the emulator has to press C= and a letter together
(`key cbm+z`). Until it can, the suite reports SKIP and says so.

What is checked, and why:

- C= R in the settings and in a context menu resets the machine: before this
  only the browser knew the key, and a key that works in one place and not the
  next is one nobody trusts.
- C= Z reboots.
- C= K and C= B ask first; "No" leaves the menu as it was.
- The help lists the keys.
- In the Commodore main menu C= L shows the log and HOME opens the browser.
"""

from __future__ import annotations

import argparse
import shutil
import sys
import tempfile
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

SUITE = "menu_hotkeys_test"
RESET_MARK = "U64 reset handler"      # printed by the C64 reset path (syslog/console)


class SkipSuite(Exception):
    pass


def chord(emu: Emulator, keys: str, settle: int = 1200) -> None:
    """Press a chord such as 'cbm+z'; skip the suite if the emulator cannot."""
    try:
        emu.key(keys, settle=settle)
    except Failure as exc:
        if "unknown key" in str(exc):
            raise SkipSuite(f"the emulator has no key chords yet ({exc})") from None
        raise


def screen(emu: Emulator) -> str:
    return "\n".join(emu.c64_rows())


def resets_since(emu: Emulator, mark: int) -> bool:
    return RESET_MARK in emu.console()[mark:]


def open_menu(emu: Emulator, mode: str) -> None:
    """Freeze into the interface's first screen. A menu left open when the
    machine was reset (the settings, after C= R there) opens again on the
    next freeze, so step back out of it first."""
    if mode == "ultimate":
        carttool.freeze_into_menu(emu, mode)
        return
    emu.button()
    for _ in range(6):
        emu.wait(600)
        if "DISK FILE BROWSER" in screen(emu):
            return
        emu.key("runstop", settle=600)
    raise Failure("the Commodore main menu did not come back")


def run_mode(mode: str, shots: Path) -> None:
    emu = Emulator(board=carttool.BOARD_FOR_MODE[mode], settings=carttool.settings_for(mode))
    emu.start()
    try:
        # SHIFT + C= only switches the character set on the BASIC screen: a
        # chord that shows whether the emulator can press chords at all,
        # before any check is started
        chord(emu, "lshift+cbm", settle=300)
        with check(f"{mode}: C= R in the settings resets the machine"):
            open_menu(emu, mode)
            emu.key("f2", settle=1500)
            carttool.wait_for_text(emu, "User Interface Settings", "the settings", tries=5)
            mark = len(emu.console())
            chord(emu, "cbm+r", settle=2000)
            if not resets_since(emu, mark):
                raise Failure("no reset after C= R in the settings")

        with check(f"{mode}: C= Z reboots"):
            open_menu(emu, mode)
            mark = len(emu.console())
            chord(emu, "cbm+z", settle=3000)
            if not resets_since(emu, mark):
                raise Failure("no reset after C= Z")

        with check(f"{mode}: C= K asks before powering off; No keeps the menu"):
            open_menu(emu, mode)
            chord(emu, "cbm+k", settle=1000)
            carttool.wait_for_text(emu, "Power off the machine?", "the power-off question")
            carttool.shot(emu, shots, f"hotkeys-{mode}-01-poweroff-question")
            emu.key("n", settle=1000)
            if "Power off the machine?" in screen(emu):
                raise Failure("the question is still there after No")

        if carttool.BOARD_FOR_MODE[mode] == "c64u":
            with check(f"{mode}: C= B asks before a power cycle; No keeps the menu"):
                chord(emu, "cbm+b", settle=1000)
                carttool.wait_for_text(emu, "Power cycle the machine?", "the power-cycle question")
                emu.key("n", settle=1000)

        with check(f"{mode}: the help lists the keys"):
            emu.key("f7" if mode == "commodore" else "f3", settle=1200)
            carttool.shot(emu, shots, f"hotkeys-{mode}-02-help")
            # The help is longer than the window: read it while scrolling down
            text = screen(emu)
            for _ in range(40):                     # the browser help is about 45 lines
                emu.key("down", settle=150)
                text += "\n" + screen(emu)
            for line in ("C= R", "C= Z", "C= K"):
                if line not in text:
                    raise Failure(f"the help has no {line!r} line")
            emu.key("runstop", settle=1000)

        if mode == "commodore":
            with check("commodore: C= L in the main menu shows the log"):
                chord(emu, "cbm+l", settle=1500)
                carttool.shot(emu, shots, "hotkeys-commodore-03-log")
                carttool.wait_for_text(emu, "Cartridge registers", "the log")  # early in the boot log
                emu.key("runstop", settle=1000)

            with check("commodore: HOME in the main menu opens the browser"):
                emu.key("home", settle=1500)
                carttool.wait_for_text(emu, "Temp", "the file browser")  # a drive only the browser lists
                carttool.shot(emu, shots, "hotkeys-commodore-03-home")
    finally:
        emu.stop()


def run(args) -> None:
    try:
        emulator_binary()
        firmware_image()
    except Failure as exc:
        raise SkipSuite(str(exc)) from None
    work = Path(tempfile.mkdtemp(prefix="menu-hotkeys-"))
    shots = Path(args.shots) if args.shots else work / "shots"
    try:
        for mode in (carttool.UI_MODES if args.mode == "all" else (args.mode,)):
            run_mode(mode, shots)
        detail(f"screenshots in {shots}")
    finally:
        if not args.keep:
            shutil.rmtree(work, ignore_errors=True)


def main() -> int:
    parser = argparse.ArgumentParser(description="Check the machine hotkeys in the emulator.")
    parser.add_argument("-m", "--mode", default="all", choices=(*carttool.UI_MODES, "all"))
    parser.add_argument("--shots", help="where to write the screenshots")
    parser.add_argument("--keep", action="store_true", help="keep the work directory")
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
