#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""A fake UE2 for tools/ue2_session.sh --dry-run and tests/ue2/ue2_selftest.py: no USB, no hardware.

    ue2_fake.py prepare   make the fake flash (16 MiB) and a synthetic package, once
    ue2_fake.py ofl ...   stands in for openFPGALoader (only the flags the session scripts use; any other exits 99)
    ue2_fake.py trxdbg .. stands in for tools/ue2_trxdbg.py (console, boot staged|result, regs, mode)
    ue2_fake.py rest      HTTP server standing in for the UE2's REST (+ the KEYB socket); prints its two ports

State in $UE2_FAKE_DIR (flash.bin, journal.log, counters). Faults, all by environment:
FAKE_IDCODE (hex, default 13631093), FAKE_JEDEC (default ef4018), FAKE_FLIP_READ=1 (every dump after the fourth has one flipped byte: the second read of a backup differs),
FAKE_CORRUPT_WRITE=<flash offset> (a byte there is flipped after a write), FAKE_WRITE_FAIL=1, FAKE_WRITEPROT=1,
FAKE_NODEVICE=1, FAKE_KEYS=all|none, FAKE_TAPE=1 (tape sense goes low), FAKE_IDENT=0 (no trx64 block).
"""
import gzip
import hashlib
import json
import os
import random
import struct
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse

FLASH = 16 << 20
DIR = Path(os.environ.get("UE2_FAKE_DIR", str(Path.home() / "trx64-till-dryrun" / "fake")))
FORBIDDEN = {"--unprotect-flash", "--protect-flash", "--bulk-erase", "--enable-quad", "--disable-quad",
             "--target-flash", "--external-flash", "--flash-sector", "--misc-device"}


def journal(line):
    DIR.mkdir(parents=True, exist_ok=True)
    with open(DIR / "journal.log", "a") as f:
        f.write(line + "\n")


def counter(name):
    p = DIR / f"{name}.count"
    n = int(p.read_text()) + 1 if p.exists() else 1
    p.write_text(str(n))
    return n


def make_bit(rng, stream_len=300000, idcode=0x03631093):
    stream = bytearray(b"\xff" * 32 + b"\x00\x00\x00\xbb\x11\x22\x00\x44" + b"\xff" * 16 + b"\xaa\x99\x55\x66")
    stream += b"\x30\x01\x80\x01" + idcode.to_bytes(4, "big") + b"\x30\x01\xa0\x01" + (0x1D34FEE5).to_bytes(4, "big")
    stream += bytes(rng.getrandbits(8) for _ in range(stream_len))
    hdr = b"\x00\x09\x0f\xf0\x0f\xf0\x0f\xf0\x0f\xf0\x00\x00\x01"
    for tag, val in ((b"a", b"fake;UserID=0XFFFFFFFF\x00"), (b"b", b"7a100tfgg484\x00"), (b"c", b"2026/10/04\x00"), (b"d", b"12:00:00\x00")):
        hdr += tag + len(val).to_bytes(2, "big") + val
    hdr += b"e" + len(stream).to_bytes(4, "big")
    return bytes(hdr + stream), bytes(stream)


def prepare():
    DIR.mkdir(parents=True, exist_ok=True)
    rng = random.Random(64)
    fl = DIR / "flash.bin"
    if not fl.exists():
        data = bytearray(b"\xff" * FLASH)
        s = b"\xff" * 16 + b"\x00\x00\x00\xbb\x11\x22\x00\x44" + b"\xff" * 8 + b"\xaa\x99\x55\x66"
        body = bytes(rng.getrandbits(8) for _ in range(0x3A6000))
        data[0:len(s) + len(body)] = s + body
        n = 0x140000
        data[0x3C0000:0x3C000C] = struct.pack("<III", 0x30000, n, 0x30000)
        data[0x3C000C:0x3C000C + n] = bytes(rng.getrandbits(8) for _ in range(n))
        data[0x580000:0x580000 + 0x100000] = bytes(rng.getrandbits(8) for _ in range(0x100000))
        data[0xFE8000:0xFE8000 + 0x10000] = bytes(rng.getrandbits(8) for _ in range(0x10000))
        fl.write_bytes(bytes(data))
    home = Path(os.environ.get("UE2_HOME", str(DIR.parent)))
    pkg = home / "pkg"
    if not pkg.exists():
        pkg.mkdir(parents=True)
        bit, _ = make_bit(rng)
        app = struct.pack("<III", 0x30000, 0x100000, 0x30000) + bytes(rng.getrandbits(8) for _ in range(0x100000))
        (pkg / "c64_basic_100t.bit").write_bytes(bit)
        (pkg / "ultimate.app").write_bytes(app)
        (pkg / "SHA256SUMS").write_text("".join(f"{hashlib.sha256((pkg / n).read_bytes()).hexdigest()}  {n}\n"
                                                for n in ("c64_basic_100t.bit", "ultimate.app")))
        (pkg / "INFO.txt").write_text("synthetic package of ue2_fake.py\n")


# ---------------------------------------------------------------------------
# openFPGALoader
# ---------------------------------------------------------------------------

def arg_value(args, *names):
    for n in names:
        if n in args:
            return args[args.index(n) + 1]
    return None


def ofl(args):
    DIR.mkdir(parents=True, exist_ok=True)
    journal("ofl " + " ".join(args))
    bad = FORBIDDEN & set(args)
    if bad:
        journal(f"FORBIDDEN {sorted(bad)}")
        print(f"fake: forbidden flag {sorted(bad)}")
        return 99
    known = {"-c", "--detect", "-f", "--write-flash", "-m", "--write-sram", "--dump-flash", "--file-size", "-o", "--offset",
             "--fpga-part", "--skip-reset", "--file-type", "-r", "--reset"}
    i = 0
    positional = None
    while i < len(args):
        a = args[i]
        if a.startswith("-"):
            if a not in known:
                print(f"fake: unknown flag {a}")
                journal(f"UNKNOWN {a}")
                return 99
            if a in ("-c", "--file-size", "-o", "--offset", "--fpga-part", "--file-type"):
                i += 1
        else:
            positional = a
        i += 1
    if os.environ.get("FAKE_NODEVICE"):
        print("JTAG init failed")
        return 1
    idc = int(os.environ.get("FAKE_IDCODE", "13631093"), 16)
    jedec = int(os.environ.get("FAKE_JEDEC", "ef4018"), 16)
    flash_path = DIR / "flash.bin"
    flash = bytearray(flash_path.read_bytes())
    part = arg_value(args, "--fpga-part")
    writing = "-f" in args or "--write-flash" in args
    if "--detect" in args and not writing:
        print(f"Jtag frequency : requested 6.00MHz   -> real 6.00MHz\nindex 0:\n\tidcode 0x{idc:x}\n\tmanufacturer xilinx\n"
              f"\tfamily artix a7 100t\n\tmodel  xc7a100t\n\tirlen  6")
        return 0
    if "--detect" in args:
        print(f"Detect flash:\nJEDEC ID: 0x{jedec:06x}\nDetected: Winbond W25Q128 256 sectors size: 128Mb\nDone")
        return 0
    if "-m" in args or "--write-sram" in args:
        data = Path(positional).read_bytes()
        (DIR / "sram.txt").write_text(hashlib.sha256(data).hexdigest())
        journal(f"SRAM {len(data)}")
        print("Load SRAM: [==================================================] 100.00%\nDone\ndone 1")
        return 0
    if "--reset" in args or "-r" in args:
        journal("RESET")
        print("reset")
        return 0
    if "--dump-flash" in args:
        n = int(arg_value(args, "--file-size"))
        off = int(arg_value(args, "-o", "--offset") or 0)
        k = counter("dumps")
        out = bytearray(flash[off:off + n])
        if os.environ.get("FAKE_FLIP_READ") and k > 4 and out:
            out[len(out) // 2] ^= 0x01
        Path(positional).write_bytes(bytes(out))
        print(f"Dump flash: 100.00%\nDone")
        return 0
    if writing:
        if arg_value(args, "--file-type") != "bin" or part != "xc7a100tfgg484":
            print("fake: write needs --file-type bin and --fpga-part xc7a100tfgg484")
            return 99
        if os.environ.get("FAKE_WRITEPROT"):
            print("Error: block protection is set\n       can't unlock without --unprotect-flash")
            return 1
        off = int(arg_value(args, "-o", "--offset") or 0)
        data = Path(positional).read_bytes()
        end = off + len(data)
        e0, e1 = off // 4096 * 4096, (end + 4095) // 4096 * 4096
        journal(f"ERASE {e0:06X} {e1:06X}")
        flash[e0:e1] = b"\xff" * (e1 - e0)
        if os.environ.get("FAKE_WRITE_FAIL"):
            flash_path.write_bytes(bytes(flash))
            print("write failed")
            return 1
        flash[off:end] = data
        journal(f"PROGRAM {off:06X} {end:06X}")
        c = os.environ.get("FAKE_CORRUPT_WRITE")
        if c and off <= int(c, 0) < end:
            flash[int(c, 0)] ^= 0x10
        flash_path.write_bytes(bytes(flash))
        print("Erase Flash: DONE\nWriting: 100.00%\nDone")
        return 0
    print("fake: nothing to do for", args)
    return 99


# ---------------------------------------------------------------------------
# trxdbg
# ---------------------------------------------------------------------------

def trxdbg(args):
    journal("trxdbg " + " ".join(args))
    cmd = args[0] if args else ""
    if cmd == "console":
        print("[TRX] IO block present, CAPS ff\nBuild 1D34FEE5 flash bitstream (no stamp)\nStatus update IP = 127.0.0.1")
    elif cmd == "boot" and args[1:2] == ["staged"]:
        if not Path(args[2]).exists():
            return 1
        print("staged: header and sums equal the file's")
    elif cmd == "boot":
        print("boot result: status 1 booting")
    elif cmd == "regs":
        print("fake regs")
    elif cmd == "mode":
        print("mode PAL requested PAL lines 312")
    else:
        print("fake trxdbg:", args)
    return 0


# ---------------------------------------------------------------------------
# REST + KEYB
# ---------------------------------------------------------------------------

class Board:
    def __init__(self):
        self.mem = bytearray(65536)
        self.lock = threading.Lock()
        self.reads = 0
        self.probe = None
        self.config = {}
        self.t0 = time.time()
        self.reset()

    def reset(self):
        self.probe = None
        self.mem[:] = bytes(65536)
        self.mem[0x0400:0x0400 + 1000] = b" " * 1000
        for row, text in enumerate(["", "    **** COMMODORE 64 BASIC V2 ****", "", " 64K RAM SYSTEM  38911 BASIC BYTES FREE", "", "READY."]):
            self.put_text(row, text)
        self.mem[0x00CB] = 64

    def put_text(self, row, text):
        for i, ch in enumerate(text[:40]):
            c = ord(ch.upper())
            self.mem[0x0400 + row * 40 + i] = c - 64 if 65 <= c <= 90 else c

    def typed(self, text):
        t = text.upper().replace("\r", "\n")
        if "SYS49152" in t.replace(" ", "") and self.mem[0xC000] in (0x78, 0xA5, 0xA9):
            self.probe = {0x78: "matrix", 0xA5: "sense", 0xA9: "motor"}[self.mem[0xC000]]
            self.reads = 0
        if "LIST" in t:
            self.put_text(8, "664 BLOCKS FREE.")

    def read(self, addr, n):
        with self.lock:
            if addr == 0xC100 and self.probe == "matrix":
                self.reads += 1
                raw = bytearray(b"\xff" * 8)
                if os.environ.get("FAKE_KEYS", "all") == "all":
                    k = self.reads
                    if k <= 70:
                        key = k - 1
                        if key < 64:
                            raw[key // 8] &= ~(1 << (key % 8)) & 0xFF
                    elif k <= 140:
                        for c, r in ((2, 6), (2, 1), (2, 7)):      # T, R, X
                            raw[c] &= ~(1 << r) & 0xFF
                self.mem[0xC100:0xC108] = raw
            if addr == 0xC100 and self.probe == "sense":
                self.reads += 1
                self.mem[0xC100] = 0x27 if os.environ.get("FAKE_TAPE") and self.reads > 4 else 0x37
            self.mem[0x00A2] = int((time.time() - self.t0) * 60) & 255
            return bytes(self.mem[addr:addr + n])


BOARD = Board()


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def send(self, body, code=200, ctype="application/json"):
        if isinstance(body, (dict, list)):
            body = json.dumps(body).encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def handle_any(self):
        u = urlparse(self.path)
        q = {k: v[0] for k, v in parse_qs(u.query).items()}
        p = u.path
        if p == "/v1/info":
            inf = {"product": "Ultimate 64 Elite-II (fake)", "firmware_version": "3.14d", "git_commit_hash": "fakefake",
                   "fpga_version": "122", "core_version": "1.49", "hostname": "Ultimate-64-II-FAKE00", "unique_id": "FAKE00",
                   "ethernet_mac": "02:15:41:00:00:01", "errors": []}
            if os.environ.get("FAKE_IDENT", "1") != "0":
                inf["trx64"] = {"core": "TRX2", "caps": "0xFF", "board": "U64-II", "rpc": 6400}
            return self.send(inf)
        if p == "/v1/machine:readmem":
            return self.send(BOARD.read(int(q["address"], 16), int(q["length"])), ctype="application/octet-stream")
        if p == "/v1/machine:writemem":
            data = bytes.fromhex(q.get("data", ""))
            if not data and self.command in ("POST", "PUT"):
                n = int(self.headers.get("Content-Length") or 0)
                data = self.rfile.read(n)
            a = int(q["address"], 16)
            with BOARD.lock:
                BOARD.mem[a:a + len(data)] = data
            return self.send({"errors": []})
        if p == "/v1/machine:reset":
            with BOARD.lock:
                BOARD.reset()
            return self.send({"errors": []})
        if p.startswith("/v1/configs/"):
            BOARD.config[p] = q.get("value")
            return self.send({"errors": []})
        if p.startswith("/v1/drives/") or p.startswith("/v1/runners:") or p.startswith("/v1/files/"):
            return self.send({"errors": []})
        return self.send({"errors": ["unknown"]}, 404)

    do_GET = do_PUT = do_POST = handle_any


def keyb_server():
    import socketserver

    class K(socketserver.BaseRequestHandler):
        def handle(self):
            data = self.request.recv(4096)
            if len(data) >= 4:
                BOARD.typed(data[4:].decode("latin-1"))

    srv = socketserver.ThreadingTCPServer(("127.0.0.1", 0), K)
    srv.daemon_threads = True
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv.server_address[1]


def rest():
    srv = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    kp = keyb_server()
    print(srv.server_address[1], flush=True)
    print(kp, flush=True)
    srv.serve_forever()


if __name__ == "__main__":
    cmd = sys.argv[1]
    if cmd == "prepare":
        prepare()
    elif cmd == "ofl":
        sys.exit(ofl(sys.argv[2:]))
    elif cmd == "trxdbg":
        sys.exit(trxdbg(sys.argv[2:]))
    elif cmd == "rest":
        rest()
