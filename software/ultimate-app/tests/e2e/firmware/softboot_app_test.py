#!/usr/bin/env python3
"""E2E: the softboot app starts the firmware inside an update image (C64U).

apps/softboot registers "Run as Softboot" for .cfw, .ue2 and .u64. It checks
that the image carries the bitstream that is in flash, byte for byte, takes
the application record behind it, hands the machine over through the ABI's
machine_handover and jumps into the copy.

The emulator creates its flash erased. For the run that must succeed the
suite writes the XC7A100T bitstream to it first, at offset 0 as the updater
does; update.cfw of the same build carries that bitstream. It runs once
with each bitstream the image carries: with the XC7A50T one in flash the app
has to step over the XC7A100T bitstream behind it to reach the application.
The emulated board itself does not care which bitstream is in flash. The
checks:

- with an erased flash the app refuses: the image brings another core;
- with the bitstream in flash it asks, and after Yes the firmware of the
  image boots a second time;
- the bitstream and the application region of the flash are unchanged.
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
from emulator import (BOOT_MARKER, Emulator, emulator_binary,   # noqa: E402
                      firmware_image, repo_root)
from report import (Failure, check, detail, format_exception,   # noqa: E402
                    suite_fail, suite_ok, suite_skip)

SUITE = "softboot_app_test"
APP = "apps/softboot/softboot.u2a"
IMAGE = "update.cfw"
BITSTREAMS = ("external/u64e2_50t.bit", "external/u64e2_100t.bit")
FLASH_SIZE = 16 * 1024 * 1024
# Bitstream and application of the XC7A100T layout end here
# (flash_addresses_u64ii_100t in software/io/flash/w25q_flash.cc).
FIRMWARE_END = 0x580000
MODE = "ultimate"


class SkipSuite(Exception):
    pass


def choose(emu: Emulator, entry: str) -> None:
    """Take `entry` from the open context menu, counted from its first row."""
    rows = carttool.wait_for_text(emu, entry, "the context menu")
    target = next(i for i, r in enumerate(rows) if entry in r)
    border = max(i for i, r in enumerate(rows[:target]) if "+---" in r or "-----" in r)
    emu.keys(["down"] * (target - border - 1), settle=200)
    emu.key("return", settle=3000)


def start(work: Path, name: str, flash: bytes | None) -> Emulator:
    stick = work / name / "usb"
    stick.mkdir(parents=True)
    shutil.copy(repo_root() / APP, stick / "softboot.u2a")
    shutil.copy(repo_root() / IMAGE, stick / IMAGE)
    emu = Emulator(usb_dir=stick, board="c64u", settings=carttool.settings_for(MODE))
    if flash is not None:
        (emu.work / "flash.bin").write_bytes(flash)
    emu.start()
    carttool.run_app(emu, MODE, app="softboot.u2a")
    carttool.wait_for_text(emu, "Choose an update image", "the app's hint")
    emu.key("return", settle=1500)
    carttool.rewind_listing(emu)
    rows = carttool.wait_for_entry(emu, "update", "the USB0 listing")
    carttool.step_to(emu, rows, "update", "the USB0 listing")
    emu.key("return", settle=800)
    choose(emu, "Run as Softboot")
    return emu


def run(work: Path) -> None:
    emu = start(work, "erased", None)
    try:
        with check("with an erased flash the app refuses the image"):
            carttool.wait_for_text(emu, "another FPGA core", "the refusal")
    finally:
        emu.stop()

    for bitstream in BITSTREAMS:
        run_seeded(work, bitstream)


def run_seeded(work: Path, bitstream: str) -> None:
    name = Path(bitstream).stem
    flash = bytearray(b"\xff" * FLASH_SIZE)
    bit = (repo_root() / bitstream).read_bytes()
    flash[:len(bit)] = bit
    emu = start(work, name, bytes(flash))
    try:
        with check(f"{name} in flash: it asks, and Yes boots the image's firmware"):
            carttool.wait_for_text(emu, "Developer mode", "the question")
            carttool.wait_for_text(emu, "does not cover this run", "the support warning")
            emu.key("y", settle=6000)
            for _ in range(60):
                if emu.console().count(BOOT_MARKER) >= 2:
                    break
                emu.wait(1000)
            else:
                raise Failure(f"{BOOT_MARKER!r} came only once")
            carttool.freeze_into_menu(emu, MODE)

        with check(f"{name} in flash: bitstream and application are unchanged"):
            now = (emu.work / "flash.bin").read_bytes()
            if now[:FIRMWARE_END] != bytes(flash[:FIRMWARE_END]):
                first = next(i for i in range(FIRMWARE_END) if now[i] != flash[i])
                raise Failure(f"the flash differs from ${first:06X}")
            detail(f"{len(bit)} bytes of bitstream compared")
    finally:
        emu.stop()


def main() -> int:
    parser = argparse.ArgumentParser(description="Run the softboot app in the emulator.")
    parser.add_argument("--keep", action="store_true", help="keep the work directory")
    args = parser.parse_args()
    work = Path(tempfile.mkdtemp(prefix="softboot-app-"))
    try:
        try:
            emulator_binary()
            firmware_image()
        except Failure as exc:
            raise SkipSuite(str(exc)) from None
        for need, how in ((APP, "apps/softboot/build.sh"), (IMAGE, "build/build.sh u64ii"),
                          *((b, "the FPGA build") for b in BITSTREAMS)):
            if not (repo_root() / need).is_file():
                raise SkipSuite(f"no {need}; see {how}")
        run(work)
    except SkipSuite as exc:
        suite_skip(SUITE, str(exc))
        return 0
    except Exception as exc:            # noqa: BLE001
        suite_fail(SUITE, format_exception(exc))
        return 1
    finally:
        if not args.keep:
            shutil.rmtree(work, ignore_errors=True)
    suite_ok(SUITE)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
