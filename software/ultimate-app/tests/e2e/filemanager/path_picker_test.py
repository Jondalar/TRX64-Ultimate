#!/usr/bin/env python3
"""E2E: the path picker opens where it is useful, and can be told where to go.

SPEC-02 (doc/specs/SPEC-02-path-picker.md). The picker is pick_path() in
tree_browser.cc, behind Copy to..., Move to..., the IEC partition path and
the apps' ui_path_input. It runs against ue2emu, driven through the file
browser the way a person does, in both interfaces.

What is checked, and why each one is here:

- The first pick opens in the caller's directory, not at the root: that is
  where a copy usually goes, and walking there from / every time is the
  complaint that started this.
- The next pick opens where the last one went, whichever file it is for.
- A cancelled pick does not move that memory.
- HOME goes to the home directory, or to / with none set; CLR goes to /.
- '/' opens a box with the path to type; a path that does not exist is
  answered with its nearest existing parent and a message, not an error.
- The copy lands where it was picked (the stick is compared on the host).
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

SUITE = "path_picker_test"
PRG = b"\x01\x08\x00\x00"


def prepare(work: Path) -> Path:
    stick = work / "usb"
    (stick / "games").mkdir(parents=True)
    (stick / "dest").mkdir()
    for name in ("alpha.prg", "beta.prg"):
        (stick / "games" / name).write_bytes(PRG)
    return stick


def screen(emu: Emulator) -> str:
    return "\n".join(emu.c64_rows())


def open_copy_to(emu: Emulator, name: str) -> list[str]:
    """From the games listing, Copy to... on `name`; returns the picker screen."""
    rows = carttool.rewind_listing(emu)
    rows = carttool.wait_for_entry(emu, name, "the games listing")
    carttool.step_to(emu, rows, name, "the games listing")
    emu.key("return", settle=800)
    carttool.wait_for_text(emu, "Copy to...", "the context menu")
    carttool.pick(emu, "Run", "Copy to...", "the context menu")
    emu.key("return", settle=1500)
    return carttool.wait_for_text(emu, "Select Current Dir", "the path picker")


def picker_path(emu: Emulator, want: str, what: str) -> list[str]:
    """Wait until the picker shows directory `want` (the path row reads it)."""
    return carttool.wait_for_text(emu, want, what, tries=10, ms=500)


def run_mode(mode: str, shots: Path, work: Path) -> None:
    stick = prepare(work / mode)
    emu = Emulator(usb_dir=stick, board=carttool.BOARD_FOR_MODE[mode],
                   settings=carttool.settings_for(mode))
    emu.start()
    try:
        rows = carttool.freeze_into_menu(emu, mode)
        rows = carttool.wait_for_entry(emu, "USB0", "the drive list")
        carttool.step_to(emu, rows, "USB0", "the drive list")
        emu.key("right", settle=800)
        rows = carttool.wait_for_entry(emu, "games", "the USB0 listing")
        carttool.step_to(emu, rows, "games", "the USB0 listing")
        emu.key("right", settle=800)

        with check(f"{mode}: the first pick opens in the caller's directory"):
            open_copy_to(emu, "alpha")
            picker_path(emu, "/USB0/games/", "the first picker")
            carttool.shot(emu, shots, f"picker-{mode}-01-first")
            if "beta.prg" not in screen(emu):
                raise Failure("the picker does not list the games directory")

        with check(f"{mode}: a pick copies there"):
            emu.key("left", settle=800)
            rows = carttool.wait_for_entry(emu, "dest", "the picker at /USB0")
            carttool.step_to(emu, rows, "dest", "the picker at /USB0")
            emu.key("right", settle=800)
            rows = carttool.wait_for_entry(emu, "Select Current Dir", "the picker in dest")
            carttool.step_to(emu, rows, "Select Current Dir", "the picker in dest")
            emu.key("return", settle=1500)
            carttool.wait_for_text(emu, "Copy complete", "the copy")
            emu.key("return", settle=800)

        with check(f"{mode}: the next pick opens where the last one went"):
            open_copy_to(emu, "beta")
            picker_path(emu, "/USB0/dest/", "the second picker")
            carttool.shot(emu, shots, f"picker-{mode}-02-remembered")

        with check(f"{mode}: HOME with no home directory goes to the root"):
            emu.key("home", settle=1000)
            rows = carttool.wait_for_entry(emu, "USB0", "the picker after HOME")
            if "/USB0/dest/" in screen(emu):
                raise Failure("HOME left the picker in /USB0/dest/")

        with check(f"{mode}: '/' asks for a path; a missing one shows its parent"):
            emu.key("/", settle=1000)
            carttool.wait_for_text(emu, "Go to path", "the path box")
            emu.key("del", settle=200)                  # the box holds "/"
            emu.type_text("/USB0/NOTHERE", settle=300)
            emu.key("return", settle=1000)
            carttool.wait_for_text(emu, "Not found", "the missing path message")
            carttool.shot(emu, shots, f"picker-{mode}-03-not-found")
            emu.key("return", settle=1000)
            picker_path(emu, "/USB0/", "the picker at the parent")
            if "dest" not in screen(emu):
                raise Failure("the picker did not move to /USB0/")

        with check(f"{mode}: CLR goes to the root, RUN/STOP cancels"):
            emu.key("clr", settle=1000)
            carttool.wait_for_entry(emu, "USB0", "the picker after CLR")
            emu.key("runstop", settle=1200)

        with check(f"{mode}: a cancelled pick keeps the memory"):
            open_copy_to(emu, "beta")
            picker_path(emu, "/USB0/dest/", "the picker after a cancel")
            emu.key("runstop", settle=1200)

        with check(f"{mode}: the copy is on the stick"):
            emu.stop()                              # the stick is written back on exit
            copied = stick / "dest" / "alpha.prg"
            if not copied.is_file() or copied.read_bytes() != PRG:
                raise Failure(f"no {copied} with the source's bytes")
    finally:
        emu.stop()


def run(args) -> None:
    try:
        emulator_binary()
        firmware_image()
    except Failure as exc:
        raise SkipSuite(str(exc))
    work = Path(tempfile.mkdtemp(prefix="path-picker-"))
    shots = Path(args.shots) if args.shots else work / "shots"
    try:
        for mode in (carttool.UI_MODES if args.mode == "all" else (args.mode,)):
            run_mode(mode, shots, work)
        detail(f"screenshots in {shots}")
    finally:
        if not args.keep:
            shutil.rmtree(work, ignore_errors=True)


class SkipSuite(Exception):
    pass


def main() -> int:
    parser = argparse.ArgumentParser(description="Check the path picker in the emulator.")
    parser.add_argument("-m", "--mode", default="all", choices=(*carttool.UI_MODES, "all"),
                        help="which interface to present (default: both)")
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
