#!/usr/bin/env python3
"""E2E: the TREX Cart Writer programs the board in the expansion port.

The Writer erases a flash cartridge and programs a CRT into it, which is the
one thing in these tools that changes the board rather than reading it. It
runs against ue2emu with an EasyFlash in its port, opened for writing and
saved back to a file when the run ends; see tests/e2e/lib/cartslot.py.

The tool is reached the way a person reaches it: "Write to Cartridge" in the
context menu of a CRT file, which hands the tool that file.

The check that matters is not the tool's own "verified OK": that is the tool
marking its own homework. What the suite compares is the board's saved image
against the CRT that was written into it, bank by bank.
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

import cartslot                                                 # noqa: E402
import carttool                                                 # noqa: E402
from emulator import Emulator                                   # noqa: E402
from report import (Failure, check, detail, format_exception,   # noqa: E402
                    suite_fail, suite_ok, suite_skip)

SUITE = "cart_writer_test"

APP = "apps/trex_crt/trex_crt.u2a"
BOARD_BANKS = 64    # a whole EasyFlash, which is what the model presents
BANKS = 8           # the image written into it: programming is byte by byte
SOURCE = "source.crt"


def prepare(work: Path) -> tuple[Path, Path, bytes]:
    board = cartslot.write_image(work, "board.crt", cartslot.easyflash(BOARD_BANKS, seed=0))
    payload = cartslot.easyflash(BANKS, name="TREX WRITTEN", seed=1)
    stick = work / "usb"
    stick.mkdir(parents=True, exist_ok=True)
    app = Path(cartslot.__file__).resolve().parents[3] / APP
    if not app.is_file():
        raise Failure(f"no {APP}; build it with apps/trex_crt/build.sh")
    shutil.copy(app, stick / "trex_crt.u2a")
    (stick / SOURCE).write_bytes(payload)
    return board, stick, payload


def open_writer_on(emu: Emulator, mode: str, crt: str) -> carttool.ToolScreen:
    """Register the app with one plain run, then "Write to Cartridge" on the
    CRT, which is only offered once the app is known."""
    carttool.run_app(emu, mode)
    carttool.wait_for_text(emu, carttool.TOOLS[0], "the tool chooser")
    emu.key("runstop", settle=2000)
    rows = carttool.rewind_listing(emu)
    rows = carttool.wait_for_entry(emu, crt, "the USB0 listing")
    carttool.step_to(emu, rows, crt, "the USB0 listing")
    emu.key("return", settle=1000)
    carttool.wait_for_text(emu, "Write to Cartridge", "the CRT context menu")
    first = next(r for r in emu.c64_rows() if "Run Cart" in r or "Run App" in r)
    carttool.pick(emu, first.strip("| +-").split("|")[0].strip(), "Write to Cartridge", "the CRT context menu")
    emu.key("return", settle=4000)
    return carttool.wait_for_status(emu, "", "the writer screen", tries=60)


def run_mode(mode: str, shots: Path, board: Path, stick: Path, payload: bytes, work: Path) -> None:
    saved = work / f"written-{mode}.crt"
    emu = Emulator(cart=board, cart_mode="rw", save_to=saved, usb_dir=stick,
                   board=carttool.BOARD_FOR_MODE[mode], settings=carttool.settings_for(mode))
    emu.start()
    try:
        info = emu.cart_info()
        if info.get("writable") != "yes":
            raise Failure(f"the board in the port is not writable: {info!r}")

        with check(f"{mode}: Write to Cartridge opens the writer on that file"):
            screen = open_writer_on(emu, mode, SOURCE)
            carttool.shot(emu, shots, f"writer-{mode}-01-opened")
            screen.check_chrome()
            if "TREX Cart Writer - EasyFlash" not in screen.header():
                raise Failure(f"the header reads {screen.header()!r}")
            if SOURCE not in screen.file_line():
                raise Failure(f"the file line reads {screen.file_line()!r}")
            if "F3=Write" not in screen.hint() or "F5=Verify" not in screen.hint():
                raise Failure(f"the key line reads {screen.hint()!r}")

        with check(f"{mode}: F3 writes the CRT, without a second question"):
            emu.key("f3", settle=1500)
            # F3 on the writer's screen is the decision; a popup here hid the
            # screen behind the file browser and read as the writer never opening
            if "Erases ALL data" in "\n".join(emu.c64_rows()):
                raise Failure("F3 asks again before writing")
            screen = carttool.wait_for_status(emu, "Write complete", "the write", tries=240)
            carttool.shot(emu, shots, f"writer-{mode}-03-written")
            screen.check_chrome()

        with check(f"{mode}: F5 verifies it"):
            emu.key("f5", settle=1500)
            carttool.wait_for_status(emu, "All banks verified OK", "the verify", tries=120)

        with check(f"{mode}: the board holds what was written into it"):
            emu.stop()                                  # the board is saved back on exit
            if not saved.is_file():
                raise Failure(f"no {saved} after the write")
            want = cartslot.banks_in(payload)
            got = cartslot.banks_in(saved.read_bytes())
            for bank in range(BANKS):
                for load in (0x8000, 0xA000):
                    if got.get((bank, load)) != want[(bank, load)]:
                        raise Failure(f"bank {bank} at ${load:04X} is not what was written")
    finally:
        emu.stop()
        shutil.rmtree(stick / ".ue2-trash", ignore_errors=True)


def run_blank_board(shots: Path, stick: Path, work: Path) -> None:
    """A board that holds nothing is still a board: exactly the one somebody
    is about to write. A probe that looks for banking by comparing two banks
    finds none on an erased chip, and reports no flash at all."""
    blank = cartslot.write_image(work, "blank.crt", cartslot.erased_easyflash(BOARD_BANKS))
    emu = Emulator(cart=blank, cart_mode="rw", save_to=work / "blank-out.crt", usb_dir=stick,
                   board=carttool.BOARD_FOR_MODE["ultimate"], settings=carttool.settings_for("ultimate"))
    emu.start()
    try:
        with check("an erased board is found as a flash cartridge"):
            screen = open_writer_on(emu, "ultimate", SOURCE)
            carttool.shot(emu, shots, "writer-blank-01-opened")
            if "No cart detected" in screen.header() or "EasyFlash" not in screen.header():
                raise Failure(f"an erased EasyFlash reads as {screen.header()!r}")
    finally:
        emu.stop()
        shutil.rmtree(stick / ".ue2-trash", ignore_errors=True)


def run(args) -> None:
    cartslot.require_slot()
    work = Path(tempfile.mkdtemp(prefix="cart-writer-"))
    shots = Path(args.shots) if args.shots else work / "shots"
    try:
        board, stick, payload = prepare(work)
        run_blank_board(shots, stick, work)
        for mode in (carttool.UI_MODES if args.mode == "all" else (args.mode,)):
            run_mode(mode, shots, board, stick, payload, work)
        detail(f"screenshots in {shots}")
    finally:
        if not args.keep:
            shutil.rmtree(work, ignore_errors=True)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Check the TREX Cart Writer against a board in the expansion port.")
    parser.add_argument("-m", "--mode", default="all", choices=(*carttool.UI_MODES, "all"),
                        help="which interface to present (default: both)")
    parser.add_argument("--shots", help="where to write the screenshots")
    parser.add_argument("--keep", action="store_true",
                        help="keep the work directory, including the emulator log")
    args = parser.parse_args()
    try:
        run(args)
    except cartslot.SlotUnavailable as exc:
        suite_skip(SUITE, str(exc))
        return 0
    except Exception as exc:            # noqa: BLE001
        suite_fail(SUITE, format_exception(exc))
        return 1
    suite_ok(SUITE)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
