#!/usr/bin/env python3
"""E2E: the TREX Cart Dumper reads the board in the expansion port.

The tool is an app (`trex_crt.u2a`) that the firmware loads while the machine
is frozen, so it draws on the C64 screen rather than the HDMI overlay, and it
reads the real expansion port rather than a cartridge the firmware emulates.
Neither can be arranged on a bare device, so this runs against ue2emu with a
board already in its port; see tests/e2e/lib/cartslot.py.

What is checked, and why each one is here:

- The screen the tool draws. Three layout faults were found while it was being
  built -- a status line over the legend, a line that wrapped and ate the row
  below it, and a second frame nested in the first -- and none of them stops
  the tool working, so only a screen check finds them.
- Scan and Dump from their function keys, in both interfaces. F1 is
  KEY_PAGEUP in the Ultimate interface and KEY_TASKS in the Commodore one; the
  tool has to answer the key that was pressed, not what it means in one of them.
- The dumped file against the board: every bank the tool reported, erased ones
  included, because a game saves into flash that was empty when it was dumped.
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

SUITE = "cart_dumper_test"

APP = "apps/trex_crt/trex_crt.u2a"
BANKS = 8           # banks the image fills; the EasyFlash model presents 64
FILE = "cartridge.crt"


def prepare(work: Path) -> tuple[Path, Path, bytes]:
    image = cartslot.easyflash(BANKS)
    crt = cartslot.write_image(work, "board.crt", image)
    stick = work / "usb"
    stick.mkdir(parents=True, exist_ok=True)
    app = Path(cartslot.__file__).resolve().parents[3] / APP
    if not app.is_file():
        raise Failure(f"no {APP}; build it with apps/trex_crt/build.sh")
    shutil.copy(app, stick / "trex_crt.u2a")
    return crt, stick, image


def run_mode(mode: str, shots: Path, crt: Path, stick: Path, image: bytes) -> None:
    emu = Emulator(cart=crt, usb_dir=stick, board=carttool.BOARD_FOR_MODE[mode],
                   settings=carttool.settings_for(mode))
    emu.start()
    try:
        if emu.cart_info().get("cart_detect") == "0x03":
            raise Failure("the board in the port asserts nothing, so nothing can be probed")

        with check(f"{mode}: the dumper opens on its own screen"):
            carttool.open_tool(emu, mode, "Dump Cartridge")
            screen = carttool.wait_for_status(emu, "", "the dumper screen")
            carttool.shot(emu, shots, f"dumper-{mode}-01-opened")
            screen.check_chrome()
            if "TREX Cart Dumper - EasyFlash" not in screen.header():
                raise Failure(f"the header reads {screen.header()!r}")
            if screen.file_line() != "CRT: -":
                raise Failure(f"the file line starts as {screen.file_line()!r}")
            if "F1=Scan" not in screen.hint() or "F3=Dump" not in screen.hint():
                raise Failure(f"the key line reads {screen.hint()!r}")

        with check(f"{mode}: F1 scans every bank"):
            emu.key("f1", settle=1000)
            screen = carttool.wait_for_status(emu, "Scan complete", "the scan")
            carttool.shot(emu, shots, f"dumper-{mode}-02-scanned")
            cells = screen.cells()[:BANKS]
            if "." in cells:
                raise Failure(f"a bank the image fills reads as empty: {cells!r}")

        with check(f"{mode}: F3 dumps into a file and the screen comes back whole"):
            emu.key("f3", settle=1500)
            carttool.wait_for_text(emu, "Dump as type", "the type question")
            emu.key("return", settle=1500)                     # Yes
            rows = carttool.wait_for_entry(emu, "USB0", "the path picker")
            carttool.step_to(emu, rows, "USB0", "the path picker")
            emu.key("right", settle=1200)
            carttool.wait_for_text(emu, "Select Current Dir", "the USB0 listing")
            emu.key("return", settle=1500)                     # << Select Current Dir >>
            carttool.shot(emu, shots, f"dumper-{mode}-03-name")
            emu.key("return", settle=1500)                     # default name "cartridge"
            screen = carttool.wait_for_status(emu, "Done:", "the dump", tries=120)
            carttool.shot(emu, shots, f"dumper-{mode}-04-dumped")
            screen.check_chrome()
            if FILE not in screen.file_line():
                raise Failure(f"the file line reads {screen.file_line()!r}")

        with check(f"{mode}: the dump holds every bank of the board"):
            emu.stop()                              # the stick is written back on exit
            dumped = stick / FILE
            if not dumped.is_file():
                raise Failure(f"no {dumped} after the dump")
            got = cartslot.banks_in(dumped.read_bytes())
            want = cartslot.banks_in(image)
            roml = sorted(bank for bank, load in got if load == 0x8000)
            if roml != list(range(64)):
                raise Failure(f"the dump holds {len(roml)} ROML banks, not all 64")
            for bank in range(BANKS):
                if got[(bank, 0x8000)] != want[(bank, 0x8000)]:
                    raise Failure(f"bank {bank} at $8000 differs from the board")
    finally:
        emu.stop()
        (stick / FILE).unlink(missing_ok=True)
        shutil.rmtree(stick / ".ue2-trash", ignore_errors=True)


def run(args) -> None:
    cartslot.require_slot()
    work = Path(tempfile.mkdtemp(prefix="cart-dumper-"))
    shots = Path(args.shots) if args.shots else work / "shots"
    try:
        crt, stick, image = prepare(work)
        for mode in (carttool.UI_MODES if args.mode == "all" else (args.mode,)):
            run_mode(mode, shots, crt, stick, image)
        detail(f"screenshots in {shots}")
    finally:
        if not args.keep:
            shutil.rmtree(work, ignore_errors=True)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Check the TREX Cart Dumper against a board in the expansion port.")
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
