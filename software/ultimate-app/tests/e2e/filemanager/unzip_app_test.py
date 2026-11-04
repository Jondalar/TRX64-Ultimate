#!/usr/bin/env python3
"""E2E: the unzip app extracts ZIP files from the file browser.

apps/unzip registers three actions for .zip in its manifest: Unzip to
folder, Unzip here and Extract one file. It reads the archive with miniz
through the firmware's file calls and creates folders with the ABI's mkdir.

The suite puts the app and a ZIP on the stick, runs the app once so the
firmware registers it, and then checks from the ZIP's context menu that:

- Unzip to folder creates games/ and extracts every entry, the one in a
  subfolder included, byte for byte;
- an entry named ../escape.txt is skipped and reported, and nothing is
  written outside the folder.
"""

from __future__ import annotations

import argparse
import io
import shutil
import sys
import tempfile
import zipfile
from pathlib import Path

# The one stanza that puts the shared library on sys.path; see tests/lib/bootstrap.py.
sys.path.insert(0, str(next(p for p in Path(__file__).resolve().parents
                            if (p / "tests" / "lib").is_dir()) / "tests" / "lib"))
import bootstrap  # noqa: E402,F401

sys.path.insert(0, str(Path(bootstrap.__file__).resolve().parents[1] / "e2e" / "lib"))

import carttool                                                 # noqa: E402
from emulator import Emulator, emulator_binary, firmware_image, repo_root  # noqa: E402
from report import (Failure, check, format_exception,           # noqa: E402
                    suite_fail, suite_ok, suite_skip)

SUITE = "unzip_app_test"
APP = "apps/unzip/unzip.u2a"
ARCHIVE = "games.zip"
ENTRIES = {
    "disk1.d64": bytes((i * 7) & 0xFF for i in range(4096)),
    "docs/readme.txt": b"TREX ZIP test\n" * 40,
}
UNSAFE = "../escape.txt"


class SkipSuite(Exception):
    pass


def archive() -> bytes:
    data = io.BytesIO()
    with zipfile.ZipFile(data, "w", zipfile.ZIP_DEFLATED) as z:
        for name, body in ENTRIES.items():
            z.writestr(name, body)
        z.writestr(UNSAFE, b"must not be written\n")
    return data.getvalue()


def choose(emu: Emulator, entry: str) -> None:
    """Take `entry` from the open context menu, counted from its first row."""
    rows = carttool.wait_for_text(emu, entry, "the context menu")
    target = next(i for i, r in enumerate(rows) if entry in r)
    border = max(i for i, r in enumerate(rows[:target]) if "+---" in r or "-----" in r)
    emu.keys(["down"] * (target - border - 1), settle=200)
    emu.key("return", settle=4000)


def run_mode(mode: str, work: Path) -> None:
    stick = work / mode / "usb"
    stick.mkdir(parents=True)
    shutil.copy(repo_root() / APP, stick / "unzip.u2a")
    (stick / ARCHIVE).write_bytes(archive())
    emu = Emulator(usb_dir=stick, board=carttool.BOARD_FOR_MODE[mode],
                   settings=carttool.settings_for(mode))
    emu.start()
    try:
        with check(f"{mode}: a plain Run App registers the app and says how to use it"):
            carttool.run_app(emu, mode, app="unzip.u2a")
            carttool.wait_for_text(emu, "Choose a .zip", "the app's hint")
            emu.key("return", settle=1500)

        with check(f"{mode}: Unzip to folder extracts every entry into games/"):
            # After an app returns the cursor stays on it: start from the top.
            carttool.rewind_listing(emu)
            rows = carttool.wait_for_entry(emu, "games", "the USB0 listing")
            carttool.step_to(emu, rows, "games", "the USB0 listing")
            emu.key("return", settle=800)
            choose(emu, "Unzip to folder")
            carttool.wait_for_text(emu, "Extracted 2 file(s) to games/", "the result popup")
            carttool.wait_for_text(emu, "Skipped 1", "the unsafe entry in the result")
            emu.key("return", settle=800)
            emu.stop()
            for name, body in ENTRIES.items():
                path = stick / "games" / name
                if not path.is_file():
                    found = sorted(str(p.relative_to(stick)) for p in stick.rglob("*"))
                    raise Failure(f"no {path.relative_to(stick)}; the stick holds {found}")
                if path.read_bytes() != body:
                    raise Failure(f"{path.relative_to(stick)} differs from the entry in the ZIP")

        with check(f"{mode}: the ../ entry was written nowhere"):
            escaped = [str(p.relative_to(work)) for p in work.rglob("escape.txt")]
            if escaped:
                raise Failure(f"found {escaped}")
    finally:
        emu.stop()


def run(args) -> None:
    try:
        emulator_binary()
        firmware_image()
    except Failure as exc:
        raise SkipSuite(str(exc)) from None
    if not (repo_root() / APP).is_file():
        raise SkipSuite(f"no {APP}; build it with apps/unzip/build.sh")
    work = Path(tempfile.mkdtemp(prefix="unzip-"))
    try:
        for mode in (carttool.UI_MODES if args.mode == "all" else (args.mode,)):
            run_mode(mode, work)
    finally:
        if not args.keep:
            shutil.rmtree(work, ignore_errors=True)


def main() -> int:
    parser = argparse.ArgumentParser(description="Check the unzip app.")
    parser.add_argument("-m", "--mode", default="all", choices=(*carttool.UI_MODES, "all"))
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
