#!/usr/bin/env python3
"""E2E: a copied file keeps a long extension.

SPEC-05 R4 (doc/specs/SPEC-05-file-operations.md), from Spiffy's b53fb820.
FileInfo holds three characters of an extension, and the copy used to set
those on the destination name: index.html arrived as index.htm. A name with
no extension of its own (a file inside a D64) still gets one.
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
from report import (Failure, check, format_exception,           # noqa: E402
                    suite_fail, suite_ok, suite_skip)

SUITE = "copy_extension_test"
BODY = b"<html>TREX</html>\n"


class SkipSuite(Exception):
    pass


def enter(emu: Emulator, name: str) -> None:
    rows = carttool.wait_for_entry(emu, name, f"the {name} entry")
    carttool.step_to(emu, rows, name, f"the {name} entry")
    emu.key("right", settle=800)


def run_mode(mode: str, work: Path) -> None:
    stick = work / mode / "usb"
    (stick / "web").mkdir(parents=True)
    (stick / "dest").mkdir()
    (stick / "web" / "index.html").write_bytes(BODY)
    emu = Emulator(usb_dir=stick, board=carttool.BOARD_FOR_MODE[mode],
                   settings=carttool.settings_for(mode))
    emu.start()
    try:
        with check(f"{mode}: index.html is copied as index.html"):
            carttool.freeze_into_menu(emu, mode)
            enter(emu, "USB0")
            enter(emu, "web")
            rows = carttool.wait_for_entry(emu, "index", "the web listing")
            carttool.step_to(emu, rows, "index", "the web listing")
            emu.key("return", settle=800)
            carttool.wait_for_text(emu, "Copy to...", "the context menu")
            rows = emu.c64_rows()
            top = next(i for i, r in enumerate(rows) if "Copy to..." in r)
            # the first entry of this menu is not known for a text file: count
            # from the box's first row, which is the row under its top border
            border = max(i for i, r in enumerate(rows[:top]) if "+---" in r or "-----" in r)
            emu.keys(["down"] * (top - border - 1), settle=200)
            emu.key("return", settle=1500)
            carttool.wait_for_text(emu, "Select Current Dir", "the path picker")
            emu.key("left", settle=800)
            rows = carttool.wait_for_entry(emu, "dest", "the picker at /USB0")
            carttool.step_to(emu, rows, "dest", "the picker at /USB0")
            emu.key("right", settle=800)
            rows = carttool.wait_for_entry(emu, "Select Current Dir", "the picker in dest")
            carttool.step_to(emu, rows, "Select Current Dir", "the picker in dest")
            emu.key("return", settle=1500)
            carttool.wait_for_text(emu, "Copy complete", "the copy")
            emu.key("return", settle=800)
            emu.stop()
            names = sorted(p.name for p in (stick / "dest").iterdir())
            if names != ["index.html"]:
                raise Failure(f"dest holds {names}")
            if (stick / "dest" / "index.html").read_bytes() != BODY:
                raise Failure("the copy's bytes differ")
    finally:
        emu.stop()


def run(args) -> None:
    try:
        emulator_binary()
        firmware_image()
    except Failure as exc:
        raise SkipSuite(str(exc)) from None
    work = Path(tempfile.mkdtemp(prefix="copy-ext-"))
    try:
        for mode in (carttool.UI_MODES if args.mode == "all" else (args.mode,)):
            run_mode(mode, work)
    finally:
        if not args.keep:
            shutil.rmtree(work, ignore_errors=True)


def main() -> int:
    parser = argparse.ArgumentParser(description="Check that a copy keeps a long extension.")
    parser.add_argument("-m", "--mode", default="all", choices=(*carttool.UI_MODES, "all"))
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
