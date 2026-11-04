#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Offline test of tools/ue2_session.py and tools/ue2_checks.py against tools/ue2_fake.py (no USB, no board, no flash).

    tests/ue2/ue2_selftest.sh

Cases: detect refuses a 50T / foreign flash / no device; backup refuses two differing reads; flash needs a backup,
needs the typed word, writes exactly its two ranges (checked against the fake's journal and the whole chip), refuses a
chip that is not the one in the backup, catches a corrupted write by readback; restore puts the chip back byte for
byte (diff mode and --full); forbidden openFPGALoader flags are refused; every flash access carries --skip-reset;
sram dry run; the checks run (with the fake C64) and report what they should.
"""
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
SESSION = [sys.executable, str(ROOT / "tools/ue2_session.py"), "--dry-run"]
sys.path.insert(0, str(ROOT / "tools"))
fails = []


def check(cond, what):
    print(("PASS " if cond else "FAIL ") + what)
    if not cond:
        fails.append(what)


def run(home, args, stdin="", env=None, module=None):
    e = dict(os.environ, UE2_HOME=str(home), PYTHONDONTWRITEBYTECODE="1")
    e.update(env or {})
    r = subprocess.run((module or SESSION) + args, input=stdin, capture_output=True, text=True, env=e, timeout=300)
    return r.returncode, r.stdout + r.stderr


def sha_file(p):
    return hashlib.sha256(Path(p).read_bytes()).hexdigest()


def fresh():
    home = Path(tempfile.mkdtemp(prefix="ue2test-"))
    return home, home / "fake" / "flash.bin", home / "fake" / "journal.log"


def main():
    home, flash, journal = fresh()
    rc, out = run(home, ["detect"])
    check(rc == 0 and "XC7A100T" in out, "detect: the 100T and the W25Q128 are accepted")
    for env, word in (({"FAKE_IDCODE": "0362c093"}, "50T"), ({"FAKE_IDCODE": "13632093"}, "other part"),
                      ({"FAKE_JEDEC": "ef6017"}, "foreign flash"), ({"FAKE_NODEVICE": "1"}, "no device")):
        rc, out = run(home, ["detect"], env=env)
        check(rc == 1 and "STOP" in out, f"detect refuses: {word}")

    # backup: differing reads
    rc, out = run(home, ["backup"], env={"FAKE_FLIP_READ": "1"})
    check(rc == 1 and "DIFFER" in out and not list(home.glob("backup-2*/backup.json")), "backup refuses two different reads, no backup.json written")
    rc, out = run(home, ["flash"], stdin="FLASH\n")
    check(rc == 1 and "no backup" in out, "flash without a backup is refused")
    before = sha_file(flash)

    rc, out = run(home, ["backup", "--label", "unit"])
    bk = sorted(home.glob("backup-2*"))[-1]
    check(rc == 0 and (bk / "backup.json").exists() and sha_file(bk / "flash.bin") == before, "backup OK, equals the chip")
    backup = (bk / "flash.bin").read_bytes()

    # flash: wrong word
    rc, out = run(home, ["flash"], stdin="flash\n")
    check(rc == 1 and sha_file(flash) == before and "PROGRAM" not in journal.read_text(), "flash with a wrong confirmation writes nothing")
    rc, out = run(home, ["flash"], stdin="")
    check(rc == 1 and sha_file(flash) == before, "flash with no stdin writes nothing")

    # flash: ok
    import ue2_session as S
    p = S.load_pkg(home / "pkg")
    want_bit, want_app = S.pad(p["stream"]), S.pad(p["app"])
    rc, out = run(home, ["flash"], stdin="FLASH\n")
    new = flash.read_bytes()
    exp = bytearray(backup)
    exp[0:len(want_bit)] = want_bit
    exp[0x3C0000:0x3C0000 + len(want_app)] = want_app
    check(rc == 0 and "FLASH OK" in out, "flash OK")
    check(new == bytes(exp), "chip = backup with exactly the bitstream at 0x0 and the app at 0x3C0000 (all else identical)")
    j = journal.read_text().splitlines()
    writes = [l.split() for l in j if l.startswith(("ERASE", "PROGRAM"))]
    ok = all((int(a, 16) < 0x3C0000 and int(b, 16) <= 0x3C0000) or (int(a, 16) >= 0x3C0000 and int(b, 16) <= 0x580000)
             for _, a, b in writes)
    check(ok and len(writes) == 4, f"journal: 2 erases + 2 programs, all inside the two windows ({len(writes)} write ops)")
    ofls = [l for l in j if l.startswith("ofl ")]
    check(all("--skip-reset" in l for l in ofls if ("-f" in l.split() and "--detect" not in l) or "--dump-flash" in l),
          "every flash access carries --skip-reset")
    check(not any(f in l for l in j for f in ("--unprotect-flash", "--bulk-erase", "--protect-flash", "FORBIDDEN", "UNKNOWN")),
          "no forbidden flag was ever used")
    check(any(l.startswith("ofl") and "--reset" in l for l in j), "flash ends with a reconfiguration")
    check("The flash-booted core and app answer" in out, "flash waited for REST (fake)")

    # corrupted write caught, restore fixes
    rc, out = run(home, ["restore"], stdin="RESTORE\n")
    check(rc == 0 and flash.read_bytes() == backup and "RESTORE OK" in out, "restore: chip byte-identical to the backup again")
    rc, out = run(home, ["restore"], stdin="")
    check(rc == 0 and "Nothing to write" in out, "restore on an identical chip writes nothing")
    rc, out = run(home, ["flash", "--no-reset"], stdin="FLASH\n", env={"FAKE_CORRUPT_WRITE": "0x3C0100"})
    check(rc == 1 and "VERIFY FAILED" in out, "a corrupted write is caught by the readback")
    rc, out = run(home, ["restore", "--no-reset"], stdin="RESTORE\n")
    check(rc == 0 and flash.read_bytes() == backup, "restore after the failed flash")
    rc, out = run(home, ["restore", "--full", "--no-reset"], stdin="RESTORE\n")
    check(rc == 0 and flash.read_bytes() == backup and "(whole chip)" in out, "restore --full")
    rc, out = run(home, ["flash", "--no-reset"], stdin="FLASH\n", env={"FAKE_WRITE_FAIL": "1"})
    check(rc == 1 and "restore" in out, "a failed write tells to restore")
    rc, out = run(home, ["restore", "--no-reset"], stdin="RESTORE\n")
    check(rc == 0 and flash.read_bytes() == backup, "restore after the write failure")
    rc, out = run(home, ["flash", "--no-reset"], stdin="FLASH\n", env={"FAKE_WRITEPROT": "1"})
    check(rc == 1 and "block protection" in out and flash.read_bytes() == backup, "write protection: stops, never unprotects")

    # not the chip of the backup
    b = bytearray(flash.read_bytes())
    b[0x3C0000 + 5000] ^= 0xFF
    flash.write_bytes(bytes(b))
    rc, out = run(home, ["flash", "--no-reset"], stdin="FLASH\n")
    check(rc == 1 and "differs from the backup" in out and flash.read_bytes() == bytes(b), "flash refuses a chip that is not the one in the backup")
    flash.write_bytes(backup)

    # code-level guards
    try:
        S.ofl(["--unprotect-flash"])
        check(False, "ofl() refuses --unprotect-flash")
    except S.Fail:
        check(True, "ofl() refuses --unprotect-flash")
    try:
        S.check_bitstream(b"\xff" * 64 + b"\xaa\x99\x55\x66" + b"\x30\x01\x80\x01" + (0x0362C093).to_bytes(4, "big"))
        check(False, "a 50T bitstream is refused")
    except S.Fail:
        check(True, "a 50T bitstream is refused")
    real = list((Path.home() / "trx64-till").glob("pkg*/c64_basic_100t.bit"))
    for rb in real[:2]:
        s = S.bit_to_bin(rb.read_bytes())
        stamp = S.check_bitstream(s)
        check(len(s) < 0x3C0000 and stamp is not None, f"real 100T bitstream {rb.parent.name}: stream {len(s)} bytes, stamp {stamp:08X}")

    # sram
    rc, out = run(home, ["sram"])
    check(rc == 0 and "SRAM session up" in out, "sram (dry run) reaches REST")
    rc, out = run(home, ["sram"], env={"FAKE_IDENT": "0"})
    check(rc == 1 and "not our core" in out, "sram notices Gideon's REST (no trx64 block)")

    # checks
    cm = [sys.executable, str(ROOT / "tools/ue2_session.py"), "--dry-run", "checks"]
    rc, out = run(home, ["run", "--assume", "yes"], module=cm)
    check(rc == 0 and "keyboard" in out and "tape" in out, "checks run to the end (fake C64, assumed answers)")
    res = json.loads(sorted((home / "results").glob("checks-*.json"))[-1].read_text())
    st = {r["name"]: r["status"] for r in res["results"]}
    print("   ", st)
    check(st.get("keyboard") == "ASSUMED" or st.get("keyboard") == "PASS", "keyboard check ran")
    check(st.get("tape") in ("NOT-BUILT", "INFO"), "tape reports not built")
    rc, out = run(home, ["run", "identity", "tape"], module=cm, env={"FAKE_TAPE": "1", "FAKE_IDENT": "0"})
    res = json.loads(sorted((home / "results").glob("checks-*.json"))[-1].read_text())
    st = {r["name"]: r["status"] for r in res["results"]}
    check(st.get("identity") == "FAIL", "identity fails without the trx64 block")

    shutil.rmtree(home, ignore_errors=True)
    print(f"\n{len(fails)} failure(s)" if fails else "\nall passed")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
