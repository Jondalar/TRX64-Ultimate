#!/usr/bin/env python3
"""Build check: the application cannot lift the flash protection.

SPEC-06 R3/R4 (doc/specs/SPEC-06-robustness.md), from Spiffy's 98ac6e74.
The application is built with FLASH_EXTRA_PROTECTION: the flash drivers'
protect_disable()/protect_configure() and the ESP32 programming routines
(Esp32::Download, Esp32::Flash) are not in it, so even an application that
runs wild has nothing in memory to lift the write protection with. The
updaters keep that code.

This reads the symbols of the built C64U application (the ELF the emulator
suites run). Only the base class's empty Flash::protect_* may remain.
"""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
from pathlib import Path

# The one stanza that puts the shared library on sys.path; see tests/lib/bootstrap.py.
sys.path.insert(0, str(next(p for p in Path(__file__).resolve().parents
                            if (p / "tests" / "lib").is_dir()) / "tests" / "lib"))
import bootstrap  # noqa: E402,F401

sys.path.insert(0, str(Path(bootstrap.__file__).resolve().parents[1] / "e2e" / "lib"))

from emulator import firmware_image                             # noqa: E402
from report import (Failure, check, format_exception,           # noqa: E402
                    suite_fail, suite_ok, suite_skip)

SUITE = "flash_protection_test"
DRIVERS = ("AT45_Flash", "AT49_Flash", "S25FL_Flash", "S25FLxxxL_Flash", "W25Q_Flash")


class SkipSuite(Exception):
    pass


def symbols(elf: Path) -> list[str]:
    nm = shutil.which("riscv64-elf-nm") or shutil.which("riscv32-unknown-elf-nm")
    if not nm:
        raise SkipSuite("no RISC-V nm on the PATH")
    out = subprocess.run([nm, "-C", str(elf)], capture_output=True, text=True, check=True).stdout
    return [line.split(" ", 2)[-1] for line in out.splitlines() if line.strip()]


def run(args) -> None:
    try:
        elf = Path(args.elf) if args.elf else firmware_image()
    except Failure as exc:
        raise SkipSuite(str(exc)) from None
    names = symbols(elf)

    with check("no flash driver can lift the write protection"):
        found = [n for n in names for d in DRIVERS
                 if n.startswith(f"{d}::protect_disable") or n.startswith(f"{d}::protect_configure")]
        if found:
            raise Failure(f"in the application: {', '.join(sorted(set(found)))}")

    with check("the application cannot program the ESP32"):
        found = [n for n in names if n.startswith("Esp32::Download") or n.startswith("Esp32::Flash")]
        if found:
            raise Failure(f"in the application: {', '.join(sorted(set(found)))}")

    with check("the rest of the flash drivers is still there"):
        if not any(n.startswith("S25FLxxxL_Flash::") or n.startswith("W25Q_Flash::") for n in names):
            raise Failure("no flash driver symbols at all: wrong ELF?")


def main() -> int:
    parser = argparse.ArgumentParser(description="Check that the application cannot lift flash protection.")
    parser.add_argument("--elf", help="the application ELF (default: the in-tree C64U build)")
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
