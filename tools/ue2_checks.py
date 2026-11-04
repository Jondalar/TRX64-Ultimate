#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""On-site checks for the UE2 (T53 session): REST of our core, keyboard on P7, picture, sound, modes, USB, tape port.

    tools/ue2_session.sh checks list
    tools/ue2_session.sh checks run [names ...] [--host IP] [--usb /Usb0/a.d64 /Usb0/b.crt ...] [--assume yes]

Default sequence: identity caps screen keyboard video modes usb tape. `lorenz` (a 12-test subset, about 2 minutes) only
when named. Needs the core and app up: `tools/ue2_session.sh sram` (or the flashed core after a power cycle).
Everything it does to the C64 is runtime only (RAM pokes, a reset, System Mode, never saved). Results go to
~/trx64-till/results/checks-<time>.json/.txt, each with the spec/TODO row it belongs to.

Statuses: PASS, FAIL, PARTIAL, INFO, NOT-BUILT (the feature does not exist in our core; stated, not pretended),
SKIP, ASSUMED (a question answered by --assume, never a real result).
"""
import argparse
import datetime
import json
import os
import select
import socket
import struct
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

import ue2_session as S

KEYS = ("DEL RETURN CRSR-RIGHT F7 F1 F3 F5 CRSR-DOWN "
        "3 W A 4 Z S E SHIFT-LEFT "
        "5 R D 6 C F T X "
        "7 Y G 8 B H U V "
        "9 I J 0 M K O N "
        "+ P L - . : @ , "
        "POUND * ; HOME SHIFT-RIGHT = UP-ARROW / "
        "1 LEFT-ARROW CTRL 2 SPACE C= Q RUN/STOP").split()          # index = column * 8 + row (CIA 1 PA = column, PB = row)
assert len(KEYS) == 64
T, R, X = KEYS.index("T"), KEYS.index("R"), KEYS.index("X")
SPEC = {"identity": "T53 §6; TODO T53", "caps": "T53 §6 (type-3 capability)", "screen": "T53 §6",
        "keyboard": "T38 (matrix), T50 §2.4/Q5 (chord bits on P7); TODO T53/T50", "video": "T53 §6; T40 B3",
        "modes": "T40 board B2/B3", "usb": "T53 §6", "tape": "T35; TODO T35 (tape port not built)",
        "lorenz": "T10/T53 (100T regression subset)"}
CAPS = {0x01: "GATE", 0x02: "STATEBUS", 0x10: "CART", 0x20: "FREEZE", 0x40: "VIDEO", 0x80: "PX565"}

# 6502 probes run from RAM at $C000 (SYS 49152); the KERNAL never sees them
MATRIX_PROBE = bytes([                       # raw CIA 1 matrix scan, IRQs off, result $C100+col (bit = 0: key down)
    0x78, 0xA9, 0xFF, 0x8D, 0x02, 0xDC, 0xA9, 0x00, 0x8D, 0x03, 0xDC,
    0xA9, 0xFE, 0xA2, 0x00,                  # C00B
    0x8D, 0x00, 0xDC, 0xEA, 0xEA, 0xEA, 0x48, 0xAD, 0x01, 0xDC, 0x9D, 0x00, 0xC1, 0x68,   # C00F loop
    0x38, 0x2A, 0xE8, 0xE0, 0x08, 0xD0, 0xEB, 0x4C, 0x0B, 0xC0])
SENSE_PROBE = bytes([0xA5, 0x01, 0x8D, 0x00, 0xC1, 0x4C, 0x00, 0xC0])      # LDA $01 : STA $C100 : JMP
MOTOR_PROBE = bytes([0xA9, 0x17, 0x85, 0x01, 0x4C, 0x00, 0xC0])             # LDA #$17 : STA $01 (motor bit 0) : JMP


class Ctx:
    def __init__(self, host, assume):
        self.host = host
        self.assume = assume
        self.results = []
        self.interactive = sys.stdin.isatty()
        self.keyb_port = int(os.environ.get("UE2_KEYB_PORT", "64"))

    # --- REST
    def req(self, method, path, body=None, timeout=15):
        r = urllib.request.Request(f"http://{self.host}{path}", data=body, method=method)
        with urllib.request.urlopen(r, timeout=timeout) as f:
            return f.read()

    def readmem(self, addr, n):
        return self.req("GET", f"/v1/machine:readmem?address={addr:04X}&length={n}")

    def writemem(self, addr, data):
        self.req("PUT", f"/v1/machine:writemem?address={addr:04X}&data={bytes(data).hex()}")

    def keyb(self, text):
        data = text.upper().replace("\n", "\r").encode()
        with socket.create_connection((self.host.split(":")[0], self.keyb_port), timeout=10) as s:
            s.sendall(struct.pack("<HH", 0xFF03, len(data)) + data)

    def reset(self, wait=3.0):
        self.req("PUT", "/v1/machine:reset")
        time.sleep(wait)

    def screen(self):
        raw = self.readmem(0x0400, 1000)
        rows = []
        for r in range(25):
            line = ""
            for c in raw[r * 40:r * 40 + 40]:
                c &= 0x7F
                line += chr(c + 64) if 1 <= c <= 26 else chr(c) if 32 <= c < 64 else "@" if c == 0 else "."
            rows.append(line.rstrip())
        return rows

    def start_probe(self, code):
        self.reset()
        self.writemem(0xC000, code)
        self.keyb("SYS49152\n")
        time.sleep(1.5)

    # --- people
    def say(self, msg=""):
        print(msg, flush=True)

    def ask(self, question):
        if self.assume:
            self.say(f"  ? {question}  -> assumed {self.assume}")
            return "assumed"
        while True:
            try:
                a = input(f"  ? {question} [y/n/s=skip] ").strip().lower()
            except EOFError:
                return "skip"
            if a in ("y", "n", "s"):
                return {"y": "yes", "n": "no", "s": "skip"}[a]

    def enter_pressed(self):
        if not self.interactive:
            return False
        r, _, _ = select.select([sys.stdin], [], [], 0)
        if r:
            sys.stdin.readline()
            return True
        return False

    def add(self, name, status, detail, data=None):
        self.results.append({"name": name, "status": status, "detail": detail, "spec": SPEC.get(name, ""), "data": data or {}})
        self.say(f"[{status}] {name}: {detail}")


def pressed(raw):
    return {c * 8 + r for c in range(8) for r in range(8) if not raw[c] >> r & 1}


def ck_identity(c):
    inf = json.loads(c.req("GET", "/v1/info"))
    t = inf.get("trx64") or {}
    d = (f"{inf.get('product')} | hostname {inf.get('hostname')} | id {inf.get('unique_id')} | mac {inf.get('ethernet_mac')} | "
         f"app {inf.get('firmware_version')} {inf.get('git_commit_hash')} | fpga {inf.get('fpga_version')} | trx64 {t}")
    if t.get("core") != "TRX2":
        return c.add("identity", "FAIL", "no TRX2 core in REST (this is not our core): " + d, inf)
    note = "" if t.get("board") == "U64-II" else f" NOTE: board field is {t.get('board')!r}, expected 'U64-II' on a UE2"
    jt = ""
    try:
        out = subprocess.run(S.NICE + S.trxdbg_cmd() + ["regs"], capture_output=True, text=True, timeout=30)
        jt = " | JTAG regs readable" if out.returncode == 0 else " | JTAG regs failed (is something else holding the Blaster?)"
    except (OSError, subprocess.TimeoutExpired):
        jt = " | JTAG regs not run"
    c.add("identity", "PASS", d + note + jt, inf)


def ck_caps(c):
    inf = json.loads(c.req("GET", "/v1/info"))
    t = inf.get("trx64") or {}
    caps = int(t.get("caps", "0x0"), 16)
    names = [n for b, n in CAPS.items() if caps & b]
    c.add("caps", "INFO", f"CAPS 0x{caps:02X}: {', '.join(names) or 'none'}. FPGA type 3 (100T) is part of the capability word the "
          f"boot ROM reads; REST does not show it. The flash boot at 0x3C0000 after `flash` is its proof.", {"caps": caps})


def ck_screen(c):
    c.reset(4)
    rows = c.screen()
    text = "\n".join(rows)
    j0 = c.readmem(0xA2, 1)[0]
    time.sleep(1.2)
    j1 = c.readmem(0xA2, 1)[0]
    ok = "COMMODORE 64 BASIC" in text and "READY." in text
    c.add("screen", "PASS" if ok and j0 != j1 else "FAIL",
          f"BASIC banner {'yes' if ok else 'NO'}, jiffy clock {'runs' if j0 != j1 else 'STANDS'} ({j0}->{j1})", {"screen": rows[:8]})


def sample(c, seen, states=None):
    """One raw-matrix sample: the pressed keys, added to `seen` (and to `states` when given)."""
    now = pressed(c.readmem(0xC100, 8))
    seen |= now
    if states is not None:
        states.append(frozenset(now))
    return now


def ck_keyboard(c):
    c.start_probe(MATRIX_PROBE)
    raw = c.readmem(0xC100, 8)
    if raw == bytes(8):
        return c.add("keyboard", "FAIL", "the matrix probe does not run (C100-C107 stay 0)")
    idle = pressed(raw)
    stuck = ", ".join(KEYS[k] for k in sorted(idle))
    c.say("\n  KEYBOARD (the classic keyboard on P7). Press EVERY key once, hold each about half a second, any order.")
    c.say("  Press Enter in this terminal when you are done or stuck. RESTORE is not in the matrix (tested after).")
    seen = set(idle)
    t0 = time.time()
    while time.time() - t0 < 240 and len(seen) < 64:
        before = set(seen)
        sample(c, seen)
        new = sorted(seen - before)
        if new:
            c.say(f"    {len(seen):2d}/64  + {', '.join(KEYS[k] for k in new)}")
        if c.enter_pressed():
            break
        time.sleep(0.03)
    missing = [KEYS[k] for k in range(64) if k not in seen]
    c.say("")
    # chord T+R+X
    c.say("  Now release everything, then hold T + R + X together for 2 seconds. Press Enter when done.")
    if c.assume:
        time.sleep(0.5)
    states = []
    dummy = set()
    t0 = time.time()
    while time.time() - t0 < (20 if c.assume else 120):
        sample(c, dummy, states)
        if c.enter_pressed() or (c.assume and len(states) > 100):
            break
        time.sleep(0.03)
    chord = frozenset({T, R, X})
    chord_ok = sum(1 for s in states if s == chord)
    extra = sorted({KEYS[k] for s in states if chord <= s for k in s - chord})
    c.say("  Release all keys.")
    time.sleep(1.5)
    after = pressed(c.readmem(0xC100, 8))
    # RESTORE
    c.say("  Now hold RUN/STOP and press RESTORE. The NMI handler sees RUN/STOP and warm-starts BASIC: the screen clears and READY. is at the top.")
    restore = None
    if c.assume:
        restore = "assumed"
    else:
        t0 = time.time()
        while time.time() - t0 < 25:
            if c.screen()[0].startswith("READY."):
                restore = "yes"
                break
            time.sleep(0.3)
            if c.enter_pressed():
                break
        restore = restore or "no"
    detail = (f"{len(seen)}/64 matrix keys seen" + (f", MISSING: {', '.join(missing)}" if missing else "") +
              (f"; held at start (stuck?): {stuck}" if stuck else "") +
              f"; chord T+R+X exact in {chord_ok} samples" + (f" (with extra keys: {', '.join(extra)})" if extra else "") +
              (f"; still pressed after release: {', '.join(KEYS[k] for k in sorted(after))}" if after else "") +
              f"; RUN/STOP+RESTORE -> warm start: {restore}")
    status = "PASS" if not missing and chord_ok and not after and restore in ("yes", "assumed") else ("PARTIAL" if len(seen) > 8 else "FAIL")
    c.add("keyboard", status, detail, {"seen": sorted(seen), "missing": missing, "chord_samples": chord_ok})
    c.say("  T50 (T+R+X unlock) is NOT built on main: this checks only that the three keys arrive as matrix bits "
          "(col 2, rows 6/1/7 = bits 22/17/23 of `keys[]`), the precondition. A P7 keyboard has no unlock in T50 (Q5).")
    c.reset(1)


def ck_video(c):
    c.reset(3)
    seq = [(2, 2, "red"), (5, 5, "green"), (6, 6, "blue"), (14, 6, "light blue / blue (normal)")]
    for b, g, name in seq:
        c.writemem(0xD020, bytes([b]))
        c.writemem(0xD021, bytes([g]))
        time.sleep(1.2)
    a1 = c.ask("Did the picture go red, green, blue, then back to the normal light-blue border on the HDMI monitor?")
    a2 = c.ask("Is the HDMI picture stable (no flicker, tearing, wrong size, cut-off edges or wrong colours)?")
    # tone: SID voice 1 triangle 440 Hz at PAL phi2
    c.writemem(0xD418, bytes([0x0F]))
    c.writemem(0xD400, bytes([0xD6, 0x1C, 0x00, 0x08, 0x11, 0x09, 0xF0]))
    time.sleep(0.2)
    c.say("  A steady tone (about 440 Hz) plays now for 10 seconds.")
    time.sleep(10 if not c.assume else 1)
    a3 = c.ask("Did you hear it on the HDMI audio?")
    a4 = c.ask("Did you hear it on the analog output (line/phones jack), if you have something plugged in? (s = nothing plugged)")
    c.writemem(0xD404, bytes([0x10]))
    c.writemem(0xD418, bytes([0x00]))
    ans = {"colours": a1, "stable": a2, "hdmi_audio": a3, "analog_audio": a4}
    bad = [k for k, v in ans.items() if v == "no"]
    st = "ASSUMED" if c.assume else ("FAIL" if bad else "PASS")
    c.add("video", st, "answers: " + ", ".join(f"{k}={v}" for k, v in ans.items()), ans)


def ck_modes(c):
    base = "/v1/configs/" + urllib.parse.quote("U64 Specific Settings") + "/" + urllib.parse.quote("System Mode")
    try:
        cur = json.loads(c.req("GET", base))
    except (urllib.error.URLError, OSError, ValueError):
        cur = {}
    ans = {}
    for mode in ("NTSC", "PAL"):
        try:
            c.req("PUT", base + "?value=" + urllib.parse.quote(mode))
        except (urllib.error.URLError, OSError) as e:
            return c.add("modes", "FAIL", f"cannot set System Mode {mode}: {e}")
        time.sleep(3)
        try:
            out = subprocess.run(S.NICE + S.trxdbg_cmd() + ["mode"], capture_output=True, text=True, timeout=30).stdout.strip().splitlines()
        except (OSError, subprocess.TimeoutExpired):
            out = []
        c.say("  trxdbg mode: " + " | ".join(out[:3]))
        ans[mode] = c.ask(f"{mode}: is the HDMI picture there and stable (a brief blank during the switch is fine), audio still running?")
    bad = [k for k, v in ans.items() if v == "no"]
    c.add("modes", "ASSUMED" if c.assume else ("FAIL" if bad else "PASS"), "NTSC then PAL via REST System Mode (runtime only): " +
          ", ".join(f"{k}={v}" for k, v in ans.items()), {"answers": ans, "config_before": cur})


def ck_usb(c, paths):
    if not paths:
        return c.add("usb", "SKIP", "no --usb paths given (put a few CRT/D64 on a USB stick, plug it in, "
                     "e.g. --usb /Usb0/game.d64 /Usb0/cart.crt; the exact path is in the Ultimate's file browser)")
    out = {}
    for p in paths:
        enc = urllib.parse.quote(p.lstrip("/"), safe="")
        try:
            info = json.loads(c.req("GET", f"/v1/files/{enc}:info"))
        except (urllib.error.URLError, OSError, ValueError) as e:
            out[p] = f"not found ({e})"
            continue
        ext = p.lower().rsplit(".", 1)[-1]
        try:
            if ext in ("d64", "g64", "d71", "d81"):
                c.reset(3)
                c.req("PUT", "/v1/drives/a:mount?image=" + urllib.parse.quote(p))
                time.sleep(1)
                c.keyb('LOAD"$",8\nLIST\n')
                time.sleep(8)
                rows = c.screen()
                out[p] = "directory listed" if any("BLOCKS FREE" in r for r in rows) else "NO directory: " + " / ".join(r for r in rows if r)[:120]
            elif ext == "crt":
                c.req("PUT", "/v1/runners:run_crt?file=" + urllib.parse.quote(p))
                time.sleep(7)
                a = c.ask(f"Did {p} start on the HDMI picture?")
                out[p] = f"started: {a}; screen text: {' / '.join(r for r in c.screen() if r)[:100]}"
            else:
                out[p] = f"extension .{ext} not handled here"
        except (urllib.error.URLError, OSError) as e:
            out[p] = f"REST error {e}"
    bad = [p for p, v in out.items() if v.startswith(("not found", "NO", "REST")) or "started: no" in v]
    c.add("usb", "ASSUMED" if c.assume else ("FAIL" if bad else "PASS"), "; ".join(f"{p}: {v}" for p, v in out.items()), out)


def ck_tape(c):
    c.say("  TAPE PORT. Our core does not have one: top.v ties it off (header comment) and c64_core.v leaves tape_cass_sense_n / "
          "tape_cass_motor_n unconnected, tape_play_toggle 0, no tape read data to CIA FLAG; the C2N_* pins are in the "
          "constraints only. This check measures that, it does not expect it to work.")
    have = c.ask("Is a Datasette plugged into the cassette port (the UE2's own, Gideon's firmware is not running now)?")
    c.start_probe(SENSE_PROBE)
    base = c.readmem(0xC100, 1)[0]
    sense_moved = None
    if have == "yes":
        c.say("  Press PLAY on the Datasette (the key clicks down) and keep it down. Enter when done.")
        t0 = time.time()
        low = False
        while time.time() - t0 < 60:
            v = c.readmem(0xC100, 1)[0]
            if not v & 0x10:
                low = True
            if c.enter_pressed() or low and time.time() - t0 > 4:
                break
            time.sleep(0.1)
        sense_moved = low
    c.start_probe(MOTOR_PROBE)
    motor = c.ask("With PLAY down (and a tape in): is the Datasette motor running NOW? (the probe pulls the motor line low)") if have == "yes" else "skip"
    c.reset(1)
    if have != "yes":
        return c.add("tape", "NOT-BUILT", f"tape port not implemented in our core (code); no Datasette connected to measure. $01 bit 4 idle = {base >> 4 & 1}")
    if sense_moved or motor == "yes":
        return c.add("tape", "INFO", f"UNEXPECTED: sense moved={sense_moved}, motor={motor}. Update T35: something reaches the tape port.")
    c.add("tape", "NOT-BUILT", f"sense does not reach the CPU port (stays {base >> 4 & 1} with PLAY down), motor line not driven: "
          "as the code says. Datasette use belongs to T35; baseline on Gideon's stock firmware first (checklist).",
          {"sense_moved": sense_moved, "motor": motor})


LORENZ = ["ADCB", "SBCIY", "BRKN", "CPUPORT", "CPUTIMING", "IRQ", "BRANCHWRAP", "CIA1TA", "CIA1TB", "ICR01", "NMI", "ONESHOT"]


def ck_lorenz(c):
    cmd = S.NICE + [sys.executable, str(S.TOOLS / "ue2_trxdbg.py"), "--run", str(S.ROOT / "tests/lorenz.py"), "1", "2", "3",
                    "--only"] + LORENZ + ["--timeout", "60"]
    if S.DRY:
        return c.add("lorenz", "SKIP", "dry run")
    c.say("  Lorenz subset (12 tests, JTAG only) ...")
    r = subprocess.run(cmd, capture_output=True, text=True)
    lines = [l for l in r.stdout.splitlines() if l.split()[:1] and l.split()[0].startswith("disk")]
    bad = [l for l in lines if " ok " not in l + " "]
    c.add("lorenz", "PASS" if lines and not bad and r.returncode == 0 else "FAIL",
          f"{len(lines) - len(bad)}/{len(lines)} ok" + (": " + "; ".join(bad) if bad else ""), {"lines": lines})


ORDER = ["identity", "caps", "screen", "keyboard", "video", "modes", "usb", "tape"]


def main(argv, dry=False):
    ap = argparse.ArgumentParser(prog="checks", description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("action", choices=["list", "run"])
    ap.add_argument("names", nargs="*")
    ap.add_argument("--host")
    ap.add_argument("--usb", nargs="*", default=[])
    ap.add_argument("--assume", choices=["yes"])
    args = ap.parse_args(argv)
    if args.action == "list":
        for n in ORDER + ["lorenz"]:
            print(f"{n:9s} {SPEC[n]}")
        return 0
    S.DRY = dry
    srv = None
    if dry:
        if "UE2_HOME" not in os.environ:
            S.HOME = Path.home() / "trx64-till-dryrun"
        os.environ["UE2_HOME"] = str(S.HOME)
        os.environ["UE2_FAKE_DIR"] = str(S.HOME / "fake")
        subprocess.run(S.FAKE + ["prepare"], check=True)
        srv = subprocess.Popen(S.FAKE + ["rest"], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
        port = srv.stdout.readline().strip()
        os.environ["UE2_KEYB_PORT"] = srv.stdout.readline().strip()
        args.host = f"127.0.0.1:{port}"
    try:
        host = args.host
        if not host and (S.HOME / "host.txt").exists():
            host = (S.HOME / "host.txt").read_text().strip()
            if not S.rest_info(host):
                host = None
        if not host:
            host = S.find_host()
        c = Ctx(host, args.assume)
        names = args.names or ORDER
        funcs = {"identity": ck_identity, "caps": ck_caps, "screen": ck_screen, "keyboard": ck_keyboard, "video": ck_video,
                 "modes": ck_modes, "usb": lambda c: ck_usb(c, args.usb), "tape": ck_tape, "lorenz": ck_lorenz}
        c.say(f"UE2 checks against {host}\n")
        for n in names:
            if n not in funcs:
                print(f"unknown check {n}; `checks list`", file=sys.stderr)
                return 2
            c.say(f"--- {n} ({SPEC[n]})")
            try:
                funcs[n](c)
            except (urllib.error.URLError, OSError) as e:
                c.add(n, "FAIL", f"REST/network error: {e}")
            except KeyboardInterrupt:
                c.add(n, "SKIP", "interrupted")
        out = S.HOME / "results"
        out.mkdir(parents=True, exist_ok=True)
        stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
        (out / f"checks-{stamp}.json").write_text(json.dumps({"host": host, "when": stamp, "results": c.results}, indent=1))
        txt = "\n".join(f"[{r['status']:9s}] {r['name']:9s} {r['detail']}\n            -> file under: {r['spec']}" for r in c.results)
        (out / f"checks-{stamp}.txt").write_text(txt + "\n")
        c.say("\n=== summary (also in " + str(out / f'checks-{stamp}.txt') + ")")
        c.say(txt)
        return 1 if any(r["status"] == "FAIL" for r in c.results) else 0
    except S.Fail as e:
        print(f"\nSTOP: {e}", file=sys.stderr)
        return 1
    finally:
        if srv:
            srv.terminate()


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
