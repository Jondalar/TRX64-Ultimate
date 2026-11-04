#!/usr/bin/env python3
"""E2E: choice lists take left and right, and so do settings items.

SPEC-04 (doc/specs/SPEC-04-dialog-keys.md), from Spiffy's 72c04833,
16a75020 and fd499929.

- A choice list (UserInterface::choice, behind the apps' ui_menu) confirms
  on RIGHT and cancels on LEFT, the way a joystick is moved, and leaves the
  screen as it found it. The TREX CRT app's tool chooser is one.
- RIGHT on a settings item opens it, as RETURN does; before it only opened
  a group.
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
from emulator import Emulator, emulator_binary, firmware_image, repo_root  # noqa: E402
from report import (Failure, check, detail, format_exception,   # noqa: E402
                    suite_fail, suite_ok, suite_skip)

SUITE = "dialog_keys_test"
APP = "apps/trex_crt/trex_crt.u2a"


class SkipSuite(Exception):
    pass


def screen(emu: Emulator) -> str:
    return "\n".join(emu.c64_rows())


def step_in_settings(emu: Emulator, pattern: str, what: str) -> None:
    """step_to for a settings list: its "-- Group --" headings are not
    selectable, the cursor jumps over them, so they are not counted."""
    entries = [body for _, body in carttool.entry_rows(emu.c64_rows())
               if not body.strip().startswith("--")]
    for offset, body in enumerate(entries):
        if pattern in body:
            if offset:
                emu.keys(["down"] * offset, settle=200)
            return
    raise Failure(f"{what}: no entry holding {pattern!r}")


def run_mode(mode: str, shots: Path, work: Path) -> None:
    stick = work / mode / "usb"
    stick.mkdir(parents=True)
    shutil.copy(repo_root() / APP, stick / "trex_crt.u2a")
    emu = Emulator(usb_dir=stick, board=carttool.BOARD_FOR_MODE[mode],
                   settings=carttool.settings_for(mode))
    emu.start()
    try:
        with check(f"{mode}: RIGHT confirms a choice list"):
            carttool.run_app(emu, mode)
            carttool.wait_for_text(emu, carttool.TOOLS[0], "the tool chooser")
            emu.key("right", settle=2500)
            carttool.wait_for_text(emu, "F1:Go", "the Cart Inspector (first entry)")
            emu.key("runstop", settle=2000)

        with check(f"{mode}: LEFT cancels a choice list and leaves the screen as it was"):
            carttool.wait_for_text(emu, "trex_crt", "the listing after the app")
            emu.key("return", settle=800)                  # the cursor stays on the app
            carttool.wait_for_text(emu, "Run App", "the app's context menu")
            emu.key("return", settle=2500)
            if "to Flash" in screen(emu):
                emu.key("n", settle=2500)
            carttool.wait_for_text(emu, carttool.TOOLS[0], "the tool chooser")
            emu.key("left", settle=1500)
            text = screen(emu)
            carttool.shot(emu, shots, f"dialogs-{mode}-01-cancelled")
            if carttool.TOOLS[0] in text or "TREX CRT Tool" in text:
                raise Failure("the chooser is still on the screen after LEFT")
            if "trex_crt" not in text:
                raise Failure("the listing did not come back after LEFT")

        with check(f"{mode}: RIGHT opens a settings item"):
            emu.key("f2", settle=1500)
            carttool.wait_for_entry(emu, "User Interface Settings", "the settings")
            step_in_settings(emu, "User Interface Settings", "the settings")
            emu.key("right", settle=1200)
            carttool.wait_for_text(emu, "Interface Type", "the User Interface page")
            step_in_settings(emu, "Interface Type", "the User Interface page")
            emu.key("right", settle=1200)
            carttool.shot(emu, shots, f"dialogs-{mode}-02-item-opened")
            carttool.wait_for_text(emu, "Overlay on HDMI", "the Interface Type values")
    finally:
        emu.stop()


def run(args) -> None:
    try:
        emulator_binary()
        firmware_image()
    except Failure as exc:
        raise SkipSuite(str(exc)) from None
    if not (repo_root() / APP).is_file():
        raise SkipSuite(f"no {APP}; build it with apps/trex_crt/build.sh")
    work = Path(tempfile.mkdtemp(prefix="dialog-keys-"))
    shots = Path(args.shots) if args.shots else work / "shots"
    try:
        for mode in (carttool.UI_MODES if args.mode == "all" else (args.mode,)):
            run_mode(mode, shots, work)
        detail(f"screenshots in {shots}")
    finally:
        if not args.keep:
            shutil.rmtree(work, ignore_errors=True)


def main() -> int:
    parser = argparse.ArgumentParser(description="Check choice-list and settings keys.")
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
