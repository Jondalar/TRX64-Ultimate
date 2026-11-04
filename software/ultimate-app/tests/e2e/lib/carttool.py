#!/usr/bin/env python3
# Getting to the TREX cart tools, and reading the screen they draw.

"""Both cart suites start the same way: freeze the machine, walk the file
browser to the app on the stick, run it, and answer the two questions it asks
before its own menu appears. That walk is here rather than in each suite.

Everything is decided from the text on screen. The row a cursor sits on is
colour, which the control port's text dump does not carry, but the cursor is
on the first entry whenever a list has just been drawn, so counting down to a
row that was found by name is deterministic.
"""

from __future__ import annotations

import re
from collections.abc import Sequence
from pathlib import Path

from emulator import Emulator
from report import Failure, detail

FREEZE_SETTINGS = {"User Interface Settings": {"Interface Type": "Freeze"}}

# Which interface the machine presents. Both are exercised, because the
# function keys mean different things in them.
UI_MODES = ("ultimate", "commodore")
BOARD_FOR_MODE = {"ultimate": "u64ii", "commodore": "c64u"}


def settings_for(mode: str) -> dict[str, dict[str, str]]:
    if mode not in UI_MODES:
        raise Failure(f"unknown UI mode {mode!r}")
    ui = dict(FREEZE_SETTINGS["User Interface Settings"])
    # Automatic follows the board, and the board is chosen with it; saying it
    # outright keeps a suite honest if that ever stops agreeing.
    ui["User Interface"] = "Commodore" if mode == "commodore" else "Ultimate"
    # The tools refuse the port unless the machine leaves it to them: with
    # any other preference the firmware's own cartridge sits in front of the
    # board (cart_dumper.c, "No access to the cartridge").
    return {"User Interface Settings": ui,
            "C64 and Cartridge Settings": {"Cartridge Preference": "Internal"}}


def row_index(rows: Sequence[str], pattern: str, what: str) -> int:
    """Index of the first row holding `pattern`, or a failure naming the screen."""
    for index, row in enumerate(rows):
        if pattern in row:
            return index
    for row in rows:
        detail(row)
    raise Failure(f"{what}: no row holding {pattern!r}")


def entry_rows(rows: Sequence[str]) -> list[tuple[int, str]]:
    """The selectable rows of a browser list, as (index, text without frame).

    The two interfaces do not draw the same browser: the Commodore one puts
    every list in a frame, the Ultimate one only the ones that ask, so the
    frame is stripped where there is one rather than required.

    Three kinds of row are not entries and are left out, because counting one
    would put the cursor that many rows past what was asked for: the title the
    firmware owns at the top, the rule or border under it, and the path and
    key hint along the bottom. A picker also prepends a heading of its own
    (`BrowsableStatic`, tree_browser.cc prepend_headers) which is drawn
    centred and cannot be selected, so a first row starting with a space goes
    the same way.
    """
    out: list[tuple[int, str]] = []
    for index, row in enumerate(rows):
        body = row[1:-1] if row.startswith("|") and row.endswith("|") else row
        stripped = body.strip()
        if not stripped:
            continue
        if set(stripped) <= set("+-#"):                 # border, rule, logo bar
            continue
        if "***" in stripped or "COMMODORE 64" in stripped or "ULTIMATE o" in stripped:
            continue
        if "HELP" in stripped and (stripped.startswith("/") or stripped.startswith("-")
                                   or "=" in stripped):
            continue
        if not out and body.startswith(" "):
            continue
        out.append((index, body))
    return out


def step_to(emu: Emulator, rows: Sequence[str], pattern: str, what: str) -> None:
    """Move the cursor from the first entry of a freshly drawn list onto the
    entry holding `pattern`."""
    entries = entry_rows(rows)
    if not entries:
        for row in rows:
            detail(row)
        raise Failure(f"{what}: the list is empty")
    for offset, (_, body) in enumerate(entries):
        if pattern in body:
            if offset:
                emu.keys(["down"] * offset, settle=200)
            return
    for row in rows:
        detail(row)
    raise Failure(f"{what}: no entry holding {pattern!r}")


def wait_for_entry(emu: Emulator, pattern: str, what: str,
                   tries: int = 12, ms: int = 1000) -> list[str]:
    """Re-read the list until an entry appears. A USB stick takes a moment to
    enumerate after the machine starts, and the browser shows it when it does,
    so the list is read again rather than assumed complete on the first look."""
    rows = emu.c64_rows()
    for _ in range(tries):
        if any(pattern in body for _, body in entry_rows(rows)):
            return rows
        emu.wait(ms)
        rows = emu.c64_rows()
    return rows


def press_until(emu: Emulator, key: str, pattern: str, what: str,
                tries: int = 8, settle: int = 800) -> list[str]:
    """Press a key until the screen shows what it was pressed for. A single
    keystroke sent while the UI is still drawing is occasionally read by
    nothing; pressing again where the screen has not changed is harmless."""
    for _ in range(tries):
        rows = emu.c64_rows()
        if any(pattern in body for _, body in entry_rows(rows)):
            return rows
        emu.key(key, settle=settle)
    rows = emu.c64_rows()
    for row in rows:
        detail(row)
    raise Failure(f"{what}: {pattern!r} never appeared after pressing {key}")


def freeze_into_menu(emu: Emulator, mode: str) -> list[str]:
    """Press the menu button and return the frozen screen it draws."""
    emu.button()
    if mode == "commodore":
        # The Commodore interface opens on its own menu; the browser is its
        # first entry. The Ultimate one opens on the browser itself.
        # The menu is drawn a moment after the freeze; reading it once too
        # early found no entry about one run in four
        rows = wait_for_entry(emu, "DISK FILE BROWSER", "the Commodore menu")
        step_to(emu, rows, "DISK FILE BROWSER", "the Commodore menu")
        return press_until(emu, "return", "Flash", "the file browser")
    return wait_for_entry(emu, "Flash", "the file browser")


def run_app(emu: Emulator, mode: str, *, volume: str = "USB0",
            app: str = "trex_crt.u2a") -> list[str]:
    """Walk to the app and start it. Returns the tool's own menu screen."""
    rows = freeze_into_menu(emu, mode)

    rows = wait_for_entry(emu, volume, "the drive list")
    step_to(emu, rows, volume, "the drive list")
    emu.key("right", settle=800)
    rows = emu.c64_rows()

    rows = wait_for_entry(emu, app, f"the {volume} listing")
    step_to(emu, rows, app, f"the {volume} listing")
    emu.key("return", settle=600)          # context menu, "Run App" on top
    rows = emu.c64_rows()
    if "Run App" not in "\n".join(rows):
        for row in rows:
            detail(row)
        raise Failure("the context menu of the app does not offer Run App")
    emu.key("return", settle=2500)

    # "Copy ... to Flash so it stays available?" - No, so the run leaves the
    # device as it found it.
    rows = emu.c64_rows()
    if "to Flash" in "\n".join(rows):
        emu.key("n", settle=3000)
        rows = emu.c64_rows()
    return rows


def pick(emu: Emulator, first: str, target: str, what: str) -> None:
    """In a popup list whose first entry is `first`, move onto `target`.
    The cursor starts on the first entry; both are found by name, so the
    distance is counted rather than assumed."""
    rows = emu.c64_rows()
    top = row_index(rows, first, what)
    row = row_index(rows, target, what)
    if row > top:
        emu.keys(["down"] * (row - top), settle=200)


def rewind_listing(emu: Emulator) -> list[str]:
    """Leave and re-enter the directory, so the cursor is on its first entry
    again. After an app returns, the browser keeps the cursor on the app."""
    emu.key("left", settle=800)
    emu.key("right", settle=1200)
    return emu.c64_rows()


TOOLS = ("Cart Inspector", "Dump Cartridge", "Flash Cartridge")


def open_tool(emu: Emulator, mode: str, tool: str) -> None:
    """Run the app and take `tool` from the chooser a plain Run App shows."""
    run_app(emu, mode)
    wait_for_text(emu, TOOLS[0], "the tool chooser")
    pick(emu, TOOLS[0], tool, "the tool chooser")
    emu.key("return", settle=3000)


def wait_for_text(emu: Emulator, text: str, what: str,
                  tries: int = 40, ms: int = 1000) -> list[str]:
    for _ in range(tries):
        rows = emu.c64_rows()
        if text in "\n".join(rows):
            return rows
        emu.wait(ms)
    rows = emu.c64_rows()
    for row in rows:
        detail(row)
    raise Failure(f"{what}: {text!r} never appeared")


# -- the tool's own screen ----------------------------------------------------

LEGEND_WORDS = ("Empt", "Data", "OK", "Err")


class ToolScreen:
    """The rows of a cart tool's screen, with the parts of it named."""

    def __init__(self, rows: Sequence[str]):
        self.rows = list(rows)
        self.text = "\n".join(self.rows)

    def header(self) -> str:
        return self.rows[row_index(self.rows, "TREX Cart", "the tool screen")].strip("| ")

    def file_line(self) -> str:
        return self.rows[row_index(self.rows, "CRT:", "the tool screen")].strip("| ")

    def status(self) -> str:
        row = self.rows[row_index(self.rows, "STATUS :", "the tool screen")]
        return row.strip("| ").split("STATUS :", 1)[1].strip()

    def hint(self) -> str:
        return self.rows[row_index(self.rows, "R/S=Exit", "the tool screen")].strip("| ")

    def map_rows(self) -> list[str]:
        """The bank-map rows: a two-digit row label, a colon, then the cells."""
        out = []
        for row in self.rows:
            match = re.search(r"\b([0-9A-F]{2}):\s(.{16})", row)
            if match:
                out.append(match.group(2))
        return out

    def cells(self) -> str:
        return "".join(self.map_rows())

    def check_chrome(self) -> None:
        """What never varies with the cartridge: the frame, the legend beside
        the first map, the status label and the key line. Every one of these
        is a layout fault that was seen while the screen was being built."""
        # The app's window sits below the two title rows the firmware owns, so
        # the frame is looked for rather than expected at the top.
        frame = [r for r in self.rows if r.startswith("+") and r.endswith("+")]
        if len(frame) < 2:
            self._blame(f"the screen has {len(frame)} border rows, not two")
        if len(frame) > 2:
            self._blame("a second frame is drawn inside the first")
        for word in LEGEND_WORDS:
            if word not in self.text:
                self._blame(f"the legend has no {word!r} entry")
        legend_rows = [r for r in self.rows if any(w in r for w in LEGEND_WORDS)]
        if len(legend_rows) != len(LEGEND_WORDS):
            self._blame(f"the legend is on {len(legend_rows)} rows, not {len(LEGEND_WORDS)}")
        if any("STATUS :" in r for r in legend_rows):
            self._blame("the status line is drawn over the legend")
        for row in self.rows:
            if len(row) > 40:
                self._blame(f"a row is {len(row)} columns wide: {row!r}")

    def _blame(self, message: str) -> None:
        for row in self.rows:
            detail(row)
        raise Failure(message)


def tool_screen(emu: Emulator) -> ToolScreen:
    return ToolScreen(emu.c64_rows())


def wait_for_status(emu: Emulator, starts_with: str, what: str,
                    tries: int = 40, ms: int = 1000) -> ToolScreen:
    """Let a pass finish. Scanning, verifying and dumping report their progress
    on the status line bank by bank, so the line is read until it carries the
    summary rather than after a fixed wait."""
    last = ""
    rows: list[str] = []
    for _ in range(tries):
        rows = emu.c64_rows()
        if any("STATUS :" in row for row in rows):
            screen = ToolScreen(rows)
            last = screen.status()
            if last.startswith(starts_with):
                return screen
        emu.wait(ms)
    for row in rows:
        detail(row)
    raise Failure(f"{what}: the status still reads {last!r}")


def shot(emu: Emulator, directory: Path, name: str) -> Path:
    return emu.screenshot(directory / f"{name}.png")
