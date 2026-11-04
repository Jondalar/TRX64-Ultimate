#!/usr/bin/env python3
"""E2E: the monitor service comes up at boot, with no app loaded (U5).

`trx_service_start()` runs once, from `ultimate.cc`, and needs two things: the
debug block in the bitstream (page 3, CAPS bit 3) and the trx table of the app
API. That table is built on demand by `app_api_refresh()`, and until
2026-10-31 nothing called it before the service looked: at boot the pointer was
still the NULL of its initialiser, so the service never started although the
block was there. The console said "no debug block", which was wrong, and the
menu entry answered with a popup about the bitstream.

This suite is the guard for that boot order. It boots the firmware with nothing
on the stick and reads the console:

- the IO block is there and CAPS has bit 3, so the debug block is present;
- the service says it is up;
- neither "does not start" nor "refused the table" was printed.

The emulator models the debug block whenever `--trx-io` is on (ue2emu
`crates/ue2-core/src/devices/trx.rs`, CAPS 0xEF), which is the default for TRX
firmware, so a failure here is the firmware's and not the emulator's.
"""

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

# The one stanza that puts the shared library on sys.path; see tests/lib/bootstrap.py.
sys.path.insert(0, str(next(p for p in Path(__file__).resolve().parents
                            if (p / "tests" / "lib").is_dir()) / "tests" / "lib"))
import bootstrap  # noqa: E402,F401

sys.path.insert(0, str(Path(bootstrap.__file__).resolve().parents[1] / "e2e" / "lib"))

import carttool                                           # noqa: E402
from emulator import Emulator, emulator_binary, firmware_image   # noqa: E402
from report import (Failure, check, detail, format_exception,     # noqa: E402
                    suite_fail, suite_ok, suite_skip)

SUITE = "trx_service_boot_test"
MODE = "ultimate"
CAPS_DBG = 0x08
# The service prints this when it owns the debug block and watches the stops.
UP = "[TRX] monitor service up"
# What it prints instead, each a different cause (software/io/c64/trx_service.cc).
REFUSALS = ("[TRX] no debug block: the monitor service does not start",
            "[TRX] the app API has no trx table: the monitor service does not start",
            "[TRX] the monitor service refused the table: no debug link or no net")


class SkipSuite(Exception):
    pass


def trx_io_on() -> str:
    """`--trx-io=on`, or SkipSuite when this ue2emu cannot model the block.

    The TRX IO block arrived in ue2emu 0.6.0 (S37); 0.5.x does not know the
    flag. The CI image pins the version in build/Dockerfile.e2e, so with an old
    one the suite cannot judge the firmware at all and says so instead of
    blaming it. The flag is passed even though `auto` would find TRX firmware
    by itself: this suite is about the block being there.
    """
    emu = emulator_binary()
    out = subprocess.run([str(emu), "run", "--help"], capture_output=True,
                         text=True, check=False)
    if "--trx-io" not in (out.stdout + out.stderr):
        raise SkipSuite(f"{emu} has no --trx-io: it cannot model the debug block "
                        "(ue2emu 0.6.0 or newer; the image pins UE2EMU_TAG in "
                        "build/Dockerfile.e2e)")
    return "--trx-io=on"


def caps_of(console: str) -> int:
    """The CAPS byte the firmware read, from its own line."""
    m = re.search(r"\[TRX\] IO block present, CAPS ([0-9A-Fa-f]{2})", console)
    if not m:
        raise Failure("the firmware never printed the IO block's CAPS although "
                      "the emulator ran with --trx-io=on")
    return int(m.group(1), 16)


def run(work: Path) -> None:
    stick = work / "usb"
    stick.mkdir(parents=True)
    emu = Emulator(usb_dir=stick, board="c64u", settings=carttool.settings_for(MODE),
                   extra=[trx_io_on()])
    emu.start()
    try:
        # The service starts late in the boot, after the settings and the menu.
        for _ in range(30):
            if UP in emu.console() or any(r in emu.console() for r in REFUSALS):
                break
            emu.wait(500)
        console = emu.console()

        with check("the bitstream the emulator models has the debug block"):
            caps = caps_of(console)
            detail(f"CAPS {caps:02X}")
            if not caps & CAPS_DBG:
                raise Failure(f"CAPS {caps:02X} has no debug block (bit 3)")

        with check("the monitor service is up with no app loaded"):
            said = [r for r in REFUSALS if r in console]
            if said:
                raise Failure("the service did not start: " + said[0])
            if UP not in console:
                raise Failure(f"the console never said {UP!r}; see {emu.log}")
    finally:
        emu.stop()


def main() -> int:
    parser = argparse.ArgumentParser(description="Boot the firmware and check the monitor service.")
    parser.add_argument("--keep", action="store_true", help="keep the work directory")
    args = parser.parse_args()
    work = Path(tempfile.mkdtemp(prefix="trx-service-boot-"))
    try:
        try:
            emulator_binary()
            firmware_image()
        except Failure as exc:
            raise SkipSuite(str(exc)) from None
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
