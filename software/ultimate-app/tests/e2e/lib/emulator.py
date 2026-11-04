#!/usr/bin/env python3
# A firmware under test that runs on this machine rather than on a device.

"""ue2emu as a bench.

The suites in tests/e2e reach a device over REST, Telnet and FTP, which is
right for everything a device can do. Two things it cannot do sit in the way
of testing the cartridge tools: a board has to be physically in the expansion
port, and somebody has to plug it in. The emulator can be told to have one,
so these suites drive it instead.

What this module owns is the process and its control port. The control
protocol is one command per line and result lines until `ok`, with block
output framed by `--- <marker> ---`; see crates/ue2emu/src/control.rs. Three
of its commands are the whole reason to use it here:

    cart-info    what board the port holds and whether it asserts its lines
    c64screen    the 40x25 text of the C64 screen, where a frozen UI draws
    png          the VIC frame, which is the screenshot of that same screen

Settings are written into the flash before the firmware runs (`--settings`,
the firmware's own .cfg format), so a suite states the configuration it needs
instead of navigating to it through the menu it is about to test.
"""

from __future__ import annotations

import os
import shutil
import socket
import subprocess
import tempfile
import time
from collections.abc import Sequence
from pathlib import Path

from report import Failure

# Where the emulator and a firmware to run in it are, when nothing says
# otherwise. Both are outside this repository, so a bench that keeps them
# elsewhere says so in the environment rather than in a patch.
EMU_ENV = "U64_EMU"
FIRMWARE_ENV = "U64_EMU_FIRMWARE"
DEFAULT_EMU = Path.home() / "Development" / "u64-emulator" / "target" / "release" / "ue2emu"
FIRMWARE_IN_TREE = Path("target/u64ii/riscv/ultimate/result/ultimate.elf")

BOOT_MARKER = "All linked modules have been initialized"
BOOT_TIMEOUT = 90.0
CONTROL_TIMEOUT = 120.0


def repo_root() -> Path:
    return next(p for p in Path(__file__).resolve().parents if (p / "tests" / "lib").is_dir())


def emulator_binary() -> Path:
    named = os.environ.get(EMU_ENV)
    path = Path(named) if named else DEFAULT_EMU
    if not path.is_file() or not os.access(path, os.X_OK):
        raise Failure(f"no ue2emu at {path}; set {EMU_ENV} to the built binary")
    return path


def firmware_image() -> Path:
    named = os.environ.get(FIRMWARE_ENV)
    path = Path(named) if named else (repo_root() / FIRMWARE_IN_TREE)
    if not path.is_file():
        raise Failure(f"no firmware at {path}; build it or set {FIRMWARE_ENV}")
    return path


def _free_port() -> int:
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def settings_file(directory: Path, stores: dict[str, dict[str, str]]) -> Path:
    """The firmware's own .cfg format: a section per store, `Item=Value`."""
    path = directory / "settings.cfg"
    lines = []
    for store, items in stores.items():
        lines.append(f"[{store}]")
        lines.extend(f"{name}={value}" for name, value in items.items())
        lines.append("")
    path.write_text("\n".join(lines), encoding="utf-8")
    return path


class Control:
    """One connection to the control port."""

    def __init__(self, port: int, timeout: float = CONTROL_TIMEOUT):
        self._sock = socket.create_connection(("127.0.0.1", port), timeout=timeout)
        self._sock.settimeout(timeout)
        self._rx = self._sock.makefile("r", encoding="utf-8", newline="\n")
        self._tx = self._sock.makefile("w", encoding="utf-8", newline="\n")

    def close(self) -> None:
        for stream in (self._tx, self._rx):
            try:
                stream.close()
            except OSError:
                pass
        try:
            self._sock.close()
        except OSError:
            pass

    def command(self, line: str) -> list[str]:
        """Send one command; return its result lines, without the `ok`."""
        self._tx.write(line + "\n")
        self._tx.flush()
        out: list[str] = []
        while True:
            reply = self._rx.readline()
            if not reply:
                raise Failure(f"the emulator closed the control port during {line!r}")
            reply = reply.rstrip("\n")
            if reply == "ok":
                return out
            if reply.startswith("error "):
                raise Failure(f"{line!r}: the emulator answered {reply!r}")
            out.append(reply)

    def block(self, line: str, marker: str) -> list[str]:
        """The lines a block-producing command framed with `--- marker ---`."""
        lines = self.command(line)
        fence = f"--- {marker} ---"
        try:
            first = lines.index(fence)
            last = lines.index(fence, first + 1)
        except ValueError:
            raise Failure(f"{line!r} did not return a {marker!r} block: {lines!r}") from None
        return lines[first + 1:last]


class Emulator:
    """A running ue2emu, started for one scenario and stopped with it."""

    def __init__(self, *, cart: Path | None = None, cart_mode: str = "ro",
                 save_to: Path | None = None, usb_dir: Path | None = None,
                 board: str = "u64ii", settings: dict[str, dict[str, str]] | None = None,
                 c64_roms: bool = True, extra: Sequence[str] = ()):
        self.cart = cart
        self.cart_mode = cart_mode
        self.save_to = save_to
        self.usb_dir = usb_dir
        self.board = board
        self.settings = settings or {}
        self.c64_roms = c64_roms
        self.extra = list(extra)
        self.work = Path(tempfile.mkdtemp(prefix="ue2emu-e2e-"))
        self.port = _free_port()
        self._proc: subprocess.Popen | None = None
        self._control: Control | None = None
        self.log = self.work / "console.log"

    # -- lifetime ----------------------------------------------------------

    def start(self) -> Emulator:
        roms = repo_root() / "roms"
        argv = [str(emulator_binary()), "run", "--headless", "--speed", "max",
                "--control", f"127.0.0.1:{self.port}",
                "--firmware", str(firmware_image()),
                "--roms", str(roms),
                "--flash", str(self.work / "flash.bin"),
                "--board", self.board]
        if self.c64_roms:
            argv += ["--c64-roms", str(roms)]
        if self.settings:
            argv += ["--settings", str(settings_file(self.work, self.settings))]
        if self.usb_dir:
            argv += ["--usb-dir", str(self.usb_dir),
                     "--usb-dir-work", str(self.work / "usb")]
        if self.cart:
            spec = str(self.cart)
            if self.save_to:
                spec += f",save={self.save_to}"
            elif self.cart_mode == "rw":
                spec += ",rw"
            argv += ["--cart-slot", spec]
        argv += self.extra

        handle = self.log.open("w", encoding="utf-8")
        self._proc = subprocess.Popen(argv, stdout=handle, stderr=subprocess.STDOUT)
        self._await_boot()
        self._control = Control(self.port)
        return self

    def _await_boot(self) -> None:
        deadline = time.monotonic() + BOOT_TIMEOUT
        while time.monotonic() < deadline:
            if self._proc and self._proc.poll() is not None:
                raise Failure(f"the emulator exited while booting; see {self.log}")
            if BOOT_MARKER in self.console():
                return
            time.sleep(0.1)
        raise Failure(f"the emulator did not boot within {BOOT_TIMEOUT:.0f}s; see {self.log}")

    def stop(self) -> None:
        if self._control:
            try:
                self._control.command("quit")
            except Failure:
                pass
            self._control.close()
            self._control = None
        if self._proc:
            try:
                self._proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self._proc.kill()
                self._proc.wait(timeout=10)
            self._proc = None

    def discard(self) -> None:
        """Stop and remove the run directory. Kept separate so a failing run
        can hold on to its console log and flash image."""
        self.stop()
        shutil.rmtree(self.work, ignore_errors=True)

    def __enter__(self) -> Emulator:
        return self.start()

    def __exit__(self, *_exc) -> None:
        self.stop()

    # -- talking to it -----------------------------------------------------

    @property
    def control(self) -> Control:
        if not self._control:
            raise Failure("the emulator is not running")
        return self._control

    def console(self) -> str:
        try:
            return self.log.read_text(encoding="utf-8", errors="replace")
        except OSError:
            return ""

    def wait(self, ms: int) -> None:
        self.control.command(f"wait {ms}")

    def trx_io(self) -> bool:
        """Whether this emulator models TRX64's IO block, from its own line.

        ue2emu 0.6.0 and newer print `trx-io: on (...)` or `trx-io: off (...)`
        when they start, and auto is on for TRX firmware. An older one prints
        neither and has no block at all.
        """
        return "trx-io: on" in self.console()

    def button(self, ms: int = 100, settle: int = 400) -> None:
        # With the IO block `button` is the board's rocker: the board holds the
        # menu button its own 200 ms and the command refuses an ms of ours
        # (ue2emu S42). Without the block it is the bare ITU bit and takes one.
        self.control.command("button" if self.trx_io() else f"button {ms}")
        self.wait(settle)

    def key(self, name: str, ms: int = 80, settle: int = 300) -> None:
        self.control.command(f"key {name} {ms}")
        if settle:
            self.wait(settle)

    def keys(self, names: Sequence[str], settle: int = 300) -> None:
        for name in names:
            self.key(name, settle=120)
        if settle:
            self.wait(settle)

    def type_text(self, text: str, settle: int = 300) -> None:
        self.control.command(f"type {text}")
        if settle:
            self.wait(settle)

    def overlay_rows(self) -> list[str]:
        return self.control.block("screen", "screen")

    def c64_rows(self) -> list[str]:
        return self.control.block("c64screen", "c64")

    def cart_info(self) -> dict[str, str]:
        out = {}
        for line in self.control.command("cart-info"):
            if ":" in line:
                name, _, value = line.partition(":")
                out[name.strip()] = value.strip()
        return out

    def screenshot(self, path: Path) -> Path:
        path.parent.mkdir(parents=True, exist_ok=True)
        self.control.command(f"png {path}")
        return path
