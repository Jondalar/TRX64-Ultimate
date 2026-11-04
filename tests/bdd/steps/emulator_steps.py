#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The steps the feature files use: the emulator, the port and the console.

The emulator is driven through the application's own end-to-end library
(`software/ultimate-app/tests/e2e/lib`), so a scenario here sees the machine
the same way the app's suites do. The machine is built by the Given steps and
started by the first When, which keeps `Given ... in the physical port` free to
come after the setting it depends on.
"""

from __future__ import annotations

import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
sys.path.insert(0, str(HERE.parent))
for _lib in ("software/ultimate-app/tests/e2e/lib", "software/ultimate-app/tests/lib"):
    sys.path.insert(0, str(ROOT / _lib))

from bdd import Skip, StepFailure, given, then, when      # noqa: E402

import carttool                                           # noqa: E402
import emulator as emu_lib                                # noqa: E402

PROBE_ASM = ROOT / "tests" / "bdd" / "data" / "port_probe.asm"
BIN2CRT = ROOT / "tests" / "cart" / "bin2crt.py"
TASS = ROOT / "tools" / "64tass.sh"
FIRMWARE_ENV = "U64_EMU_FIRMWARE"
BUILT_APP = ROOT / "software" / "ultimate-app" / "target" / "u64ii" / "riscv" / "ultimate" / "result"

_probe_crt: Path | None = None


# ---- helpers -----------------------------------------------------------------

def _firmware() -> Path:
    named = os.environ.get(FIRMWARE_ENV)
    if named:
        if not Path(named).is_file():
            raise Skip(f"{FIRMWARE_ENV}={named} is not a file")
        return Path(named)
    for name in ("ultimate.elf", "ultimate.app"):
        if (BUILT_APP / name).is_file():
            return BUILT_APP / name
    raise Skip(f"no firmware: set {FIRMWARE_ENV} to an ultimate.app or ultimate.elf")


def _emulator() -> None:
    try:
        emu_lib.emulator_binary()
    except Exception as exc:                                # noqa: BLE001
        raise Skip(f"no emulator: {exc}") from None


def probe_cartridge() -> Path:
    """The 8 KiB autostart cartridge of tests/bdd/data, assembled and packed."""
    global _probe_crt
    if _probe_crt and _probe_crt.is_file():
        return _probe_crt
    if not shutil.which("python3") or not TASS.is_file():
        raise Skip(f"no assembler at {TASS}")
    work = Path(tempfile.mkdtemp(prefix="bdd-probe-"))
    binary, crt = work / "port_probe.bin", work / "port_probe.crt"
    run = subprocess.run([str(TASS), "-q", "-o", str(binary), "--nostart", str(PROBE_ASM)],
                         capture_output=True, text=True)
    if run.returncode != 0 or not binary.is_file():
        raise Skip(f"64tass did not assemble the probe cartridge: "
                   f"{(run.stderr or run.stdout).strip()[:200]}")
    subprocess.run([sys.executable, str(BIN2CRT), str(binary), str(crt), "0",
                    "--name", "TRX64 PORT PROBE", "--exrom", "0", "--game", "1"],
                   check=True, capture_output=True, text=True)
    _probe_crt = crt
    return crt


def machine(world) -> None:
    """Start the emulator once, with everything the Given steps have set."""
    if getattr(world, "emu", None):
        return
    settings = carttool.settings_for("ultimate")
    settings["C64 and Cartridge Settings"]["Cartridge Preference"] = world.preference
    os.environ[FIRMWARE_ENV] = str(world.firmware)
    world.emu = emu_lib.Emulator(cart=getattr(world, "cart", None), cart_mode="ro",
                                 board="c64u", settings=settings)
    world.closing(lambda: world.emu.stop())
    world.emu.start()


def cart_info(world) -> dict[str, str]:
    if getattr(world, "info", None) is None:
        lines = world.emu.control.command("cart-info")
        world.info = dict(line.split(": ", 1) for line in lines if ": " in line)
    return world.info


def screen(world) -> list[str]:
    return [row.rstrip() for row in world.emu.c64_rows()]


# ---- Given -------------------------------------------------------------------

@given(r"the TRX64 firmware in the emulator")
def firmware(world) -> None:
    _emulator()
    world.firmware = _firmware()
    world.preference = "Internal"      # what the app's own suites use
    world.info = None


@given(r'the setting Cartridge Preference is "([^"]+)"')
def preference(world, value: str) -> None:
    world.preference = value


@given(r"the probe cartridge in the physical port")
def probe_in_port(world) -> None:
    world.cart = probe_cartridge()


@given(r"the cartridge named by \$([A-Z0-9_]+) in the physical port")
def named_cart(world, variable: str) -> None:
    path = os.environ.get(variable)
    if not path:
        raise Skip(f"${variable} names no cartridge; a game is not in this repository")
    if not Path(path).is_file():
        raise Skip(f"${variable}={path} is not a file")
    world.cart = Path(path)


@given(r"nothing in the physical port")
def empty_port(world) -> None:
    world.cart = None


# ---- When --------------------------------------------------------------------

@when(r"the firmware has booted")
def booted(world) -> None:
    machine(world)


@when(r"the machine has been running for (\d+) seconds")
def running(world, seconds: str) -> None:
    machine(world)
    world.emu.wait(int(seconds) * 1000)


@when(r'the console has said "([^"]+)" or (\d+) seconds have passed')
def wait_for_console(world, text: str, seconds: str) -> None:
    machine(world)
    for _ in range(int(seconds) * 2):
        if text in world.emu.console():
            return
        world.emu.wait(500)


# ---- Then --------------------------------------------------------------------

@then(r'cart-info reports "([a-z_]+)" as "([^"]*)"')
def info_is(world, key: str, value: str) -> None:
    info = cart_info(world)
    if key not in info:
        raise StepFailure(f"cart-info has no {key!r}; it has {', '.join(sorted(info))}")
    if info[key] != value:
        raise StepFailure(f"cart-info {key} is {info[key]!r}, not {value!r}")


@then(r'the C64 screen shows "([^"]+)"')
def screen_shows(world, text: str) -> None:
    rows = screen(world)
    if not any(text.lower() in row.lower() for row in rows):
        raise StepFailure(f"{text!r} is not on the screen: " +
                          " / ".join(r for r in rows if r.strip())[:200])


@then(r'the C64 screen does not show "([^"]+)"')
def screen_hides(world, text: str) -> None:
    rows = screen(world)
    if any(text.lower() in row.lower() for row in rows):
        raise StepFailure(f"{text!r} is on the screen although it should not be")


@then(r"the C64 screen is not the BASIC prompt")
def not_basic(world) -> None:
    rows = screen(world)
    if any("BASIC BYTES FREE" in row.upper() for row in rows):
        raise StepFailure("the machine is in BASIC: the cartridge did not take over")


@then(r'the console says "([^"]+)"')
def console_says(world, text: str) -> None:
    if text not in world.emu.console():
        raise StepFailure(f"the console never said {text!r}; see {world.emu.log}")


@then(r'the console never says "([^"]+)"')
def console_silent(world, text: str) -> None:
    console = world.emu.console()
    if text in console:
        line = next(ln for ln in console.splitlines() if text in ln)
        raise StepFailure(f"the console said it: {line.strip()!r}")


@then(r"the IO block reports the debug block")
def caps_has_debug(world) -> None:
    m = re.search(r"\[TRX\] IO block present, CAPS ([0-9A-Fa-f]{2})", world.emu.console())
    if not m:
        raise StepFailure("the firmware never printed the IO block's CAPS: "
                          "the emulator ran without --trx-io")
    caps = int(m.group(1), 16)
    if not caps & 0x08:
        raise StepFailure(f"CAPS {caps:02X} has no debug block (bit 3)")
