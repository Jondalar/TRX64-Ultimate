#!/usr/bin/env python3
# A cartridge in the expansion port, which the bench has to arrange.

"""The existing cartridge suites mount a .crt file and let the firmware's own
cartridge emulation present it to the C64. The bytes come from the Ultimate's
memory and the expansion port is not involved.

The TREX cart tools read and program a board in that port, which is the one
thing a device on its own cannot be told to have. This module arranges it.

    U64_CART_SLOT=emulator   the firmware under test runs in ue2emu, which is
                             started with the board already in its port
    U64_CART_SLOT=manual     a person is at the bench and is asked to plug one
                             in; with no terminal to ask at, the suite skips
    unset                    no board can be placed, so the suite skips

Only the emulator backend is implemented, and deliberately: `manual` is here
so that a run on hardware says what it would need rather than failing in a way
that reads like a defect.

The images are built here rather than shipped, the way the EasyFlash and Ocean
suites already build theirs, so what a bank holds is known to the byte and a
dump can be compared against it.
"""

from __future__ import annotations

import os
import struct
import sys
from pathlib import Path

from report import Failure

SLOT_ENV = "U64_CART_SLOT"

BANK_SIZE = 0x2000
CRT_TYPE_NORMAL = 0
CRT_TYPE_EASYFLASH = 32

# What cart_detect reads when nothing in the port pulls its lines. A tool that
# probes then finds "Normal, 1 bank" whatever the file holds, so a case that
# needs a real type has to check this first.
NO_CART = 3


class SlotUnavailable(Exception):
    """No board can be placed, with the reason a skip should carry."""


def slot_kind() -> str:
    return os.environ.get(SLOT_ENV, "").strip().lower()


def require_slot() -> str:
    kind = slot_kind()
    if kind == "emulator":
        return kind
    if kind == "manual":
        if not sys.stdin.isatty():
            raise SlotUnavailable(f"{SLOT_ENV}=manual needs a terminal to ask at")
        return kind
    raise SlotUnavailable(
        f"no cartridge slot: set {SLOT_ENV}=emulator to run this against ue2emu, "
        f"or {SLOT_ENV}=manual to be asked to plug a board in")


def ask_for_board(description: str) -> None:
    print(f"\n  Put {description} in the expansion port, then press RETURN.", flush=True)
    input()


# -- images -------------------------------------------------------------------

def _header(crt_type: int, name: str, exrom: int, game: int) -> bytes:
    """The 64-byte CRT header: length, version $0100, the type as a 16-bit
    field, then the two line states. Getting the version and the type the
    wrong way round reads back as a Normal cartridge, which is exactly what a
    tool then probes."""
    head = b"C64 CARTRIDGE   "
    head += struct.pack(">IHHBB6x", 0x40, 0x0100, crt_type, exrom, game)
    head += name.encode("ascii")[:32].ljust(32, b"\0")
    return head


def _chip(bank: int, load: int, rom: bytes) -> bytes:
    return b"CHIP" + struct.pack(">IHHHH", len(rom) + 16, 2, bank, load, len(rom)) + rom


def bank_marker(bank: int, chip: int, seed: int = 0) -> int:
    """What every byte of one bank is. Never $FF or $00, so a scan reads the
    bank as holding something rather than as empty. `seed` gives a second
    image whose every bank differs from the first one's."""
    return 0x41 + ((bank + chip * 0x20 + seed * 0x11) & 0x3F)


def normal_16k(name: str = "TREX TEST 16K") -> bytes:
    """One bank at $8000 and one at $A000, each filled with its own marker."""
    out = _header(CRT_TYPE_NORMAL, name, 0, 0)
    out += _chip(0, 0x8000, bytes([bank_marker(0, 0)]) * BANK_SIZE)
    out += _chip(0, 0xA000, bytes([bank_marker(0, 1)]) * BANK_SIZE)
    return out


def easyflash(banks: int = 8, name: str = "TREX TEST EF", seed: int = 0) -> bytes:
    """`banks` banks on both chips. Every other bank on ROMH is left erased,
    so a scan has both "holds something" and "empty" to report."""
    out = _header(CRT_TYPE_EASYFLASH, name, 1, 0)
    for bank in range(banks):
        out += _chip(bank, 0x8000, bytes([bank_marker(bank, 0, seed)]) * BANK_SIZE)
        filler = bank_marker(bank, 1, seed) if bank % 2 == 0 else 0xFF
        out += _chip(bank, 0xA000, bytes([filler]) * BANK_SIZE)
    return out


def erased_easyflash(banks: int = 8, name: str = "TREX BLANK EF") -> bytes:
    """Every bank $FF: the board somebody is about to write. A probe that
    looks for banking by comparing two banks finds none on it, which is why
    writer_test checks that the tool still recognises it."""
    out = _header(CRT_TYPE_EASYFLASH, name, 1, 0)
    for bank in range(banks):
        out += _chip(bank, 0x8000, b"\xFF" * BANK_SIZE)
        out += _chip(bank, 0xA000, b"\xFF" * BANK_SIZE)
    return out


def write_image(directory: Path, name: str, data: bytes) -> Path:
    directory.mkdir(parents=True, exist_ok=True)
    path = directory / name
    path.write_bytes(data)
    return path


def banks_in(crt: bytes) -> dict[tuple[int, int], bytes]:
    """The CHIP packets of a CRT, keyed by (bank, load address)."""
    out: dict[tuple[int, int], bytes] = {}
    offset = struct.unpack(">I", crt[0x10:0x14])[0]
    while offset + 16 <= len(crt):
        if crt[offset:offset + 4] != b"CHIP":
            raise Failure(f"no CHIP packet at offset {offset}")
        packet, _kind, bank, load, size = struct.unpack(">IHHHH", crt[offset + 4:offset + 16])
        out[(bank, load)] = crt[offset + 16:offset + 16 + size]
        offset += packet
    return out
