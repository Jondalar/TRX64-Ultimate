#!/usr/bin/env python3
"""E2E: a disk menu offers "Mount Disk" first where the last disk came from.

SPEC-05 R1-R3 (doc/specs/SPEC-05-file-operations.md), from Spiffy's smart
mount (feb83add). A two-disk game is run from disk 1; when disk 2 is asked
for, its menu in the same directory should put the mount under the cursor,
not another run. Elsewhere the interface's own order stays.

- Commodore interface: Run Disk first normally; after a mount or run from a
  directory, Mount Disk first for the disks in that directory, Run Disk first
  again in any other.
- Ultimate interface: Mount Disk first always (as before).
- D64 and G64 alike.
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

SUITE = "smart_mount_test"
D64_SIZE = 174848


class SkipSuite(Exception):
    pass


def prepare(work: Path) -> Path:
    stick = work / "usb"
    for folder, names in (("set", ("disk1.d64", "disk2.d64", "disk3.g64")), ("other", ("solo.d64",))):
        (stick / folder).mkdir(parents=True)
        for name in names:
            # An empty image: the menu is what is tested, not the disk
            (stick / folder / name).write_bytes(bytes(D64_SIZE) if name.endswith(".d64")
                                                else b"GCR-1541\x00\x54\xf8\x1e" + bytes(84 * 8))
    return stick


def first_action(emu: Emulator) -> str:
    """Which of Run Disk / Mount Disk the open context menu lists first."""
    rows = emu.c64_rows()
    run = next((i for i, r in enumerate(rows) if "Run Disk" in r), None)
    mount = next((i for i, r in enumerate(rows) if "Mount Disk" in r and "Read" not in r
                  and "Unl" not in r and " on B" not in r), None)
    if run is None or mount is None:
        for row in rows:
            detail(row)
        raise Failure("the context menu has no Run Disk / Mount Disk")
    return "Run Disk" if run < mount else "Mount Disk"


def open_menu_on(emu: Emulator, folder_rows_name: str) -> None:
    rows = carttool.rewind_listing(emu)
    rows = carttool.wait_for_entry(emu, folder_rows_name, "the directory")
    carttool.step_to(emu, rows, folder_rows_name, "the directory")
    emu.key("return", settle=800)
    carttool.wait_for_text(emu, "Mount Disk", "the disk menu")


def close_menu(emu: Emulator) -> None:
    emu.key("runstop", settle=600)


def enter(emu: Emulator, name: str) -> None:
    rows = carttool.wait_for_entry(emu, name, f"the {name} entry")
    carttool.step_to(emu, rows, name, f"the {name} entry")
    emu.key("right", settle=800)


def run_mode(mode: str, shots: Path, work: Path) -> None:
    stick = prepare(work / mode)
    emu = Emulator(usb_dir=stick, board=carttool.BOARD_FOR_MODE[mode],
                   settings=carttool.settings_for(mode))
    emu.start()
    default = "Run Disk" if mode == "commodore" else "Mount Disk"
    try:
        carttool.freeze_into_menu(emu, mode)
        enter(emu, "USB0")
        enter(emu, "set")

        with check(f"{mode}: a disk menu opens with the interface's order"):
            open_menu_on(emu, "disk1")
            carttool.shot(emu, shots, f"smartmount-{mode}-01-first")
            got = first_action(emu)
            if got != default:
                raise Failure(f"{got} is first, not {default}")

        with check(f"{mode}: after a mount, Mount Disk is first in that directory"):
            carttool.pick(emu, default, "Mount Disk", "the disk menu")
            emu.key("return", settle=1500)
            # The emulated drive may be set to another type; the mount asks first
            if "Change drive type" in "\n".join(emu.c64_rows()):
                emu.key("return", settle=1500)
            open_menu_on(emu, "disk2")
            carttool.shot(emu, shots, f"smartmount-{mode}-02-same-dir")
            got = first_action(emu)
            close_menu(emu)
            if got != "Mount Disk":
                raise Failure(f"{got} is first for the next disk of the set")

        with check(f"{mode}: a G64 in that directory as well"):
            open_menu_on(emu, "disk3")
            got = first_action(emu)
            close_menu(emu)
            if got != "Mount Disk":
                raise Failure(f"{got} is first for the G64")

        with check(f"{mode}: another directory keeps the interface's order"):
            # Up to the root and into USB0 again, so the cursor is on the first entry
            emu.key("left", settle=800)
            emu.key("left", settle=800)
            emu.key("right", settle=800)
            enter(emu, "other")
            open_menu_on(emu, "solo")
            got = first_action(emu)
            close_menu(emu)
            if got != default:
                raise Failure(f"{got} is first in another directory, not {default}")
    finally:
        emu.stop()


def run(args) -> None:
    try:
        emulator_binary()
        firmware_image()
    except Failure as exc:
        raise SkipSuite(str(exc)) from None
    work = Path(tempfile.mkdtemp(prefix="smart-mount-"))
    shots = Path(args.shots) if args.shots else work / "shots"
    try:
        for mode in (carttool.UI_MODES if args.mode == "all" else (args.mode,)):
            run_mode(mode, shots, work)
        detail(f"screenshots in {shots}")
    finally:
        if not args.keep:
            shutil.rmtree(work, ignore_errors=True)


def main() -> int:
    parser = argparse.ArgumentParser(description="Check smart mount in the emulator.")
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
