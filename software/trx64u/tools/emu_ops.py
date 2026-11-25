#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The operations and states trx64u's application registers, read from the running application in ue2emu (U18).

    python3 -I software/trx64u/tools/emu_ops.py --cor <trx64u.cor> --app <app.elf | app.bin> [--check <golden>]
        [--out <file>] [--ue2emu <path>] [--roms <dir>] [--work <dir>] [--timeout <s>] [--keep]

base/ops_trx64u.txt is the list pack_cor.sh hands cor_pack.py (--ops): every operation and binding of record 6 is checked against
it. Since MultiCore T239.3 D31 each core's app is linked with everything it uses, ours by pack_cor.sh, so the list has to come from
that app (T239.3 P2-5: our emu run and our --ops check are its proof). The SDK's link step writes no such list (app_build.py generates
appsizes.h, the driver table, libtab_table.cc and io.ld; the operations are registered at run time by the base, the catalog's
libraries and the .u2a of record 7), so it is read from the running app. This tool writes it as MultiCore's tests/ui/export.py
(8ff28ba9) does: GET /v1/ops and GET /v1/state,
paged by "next"; a line per operation, `op<TAB>name<TAB>arg:type:lo:hi:maxlen:label|label...` (int: min and max; bool: 0 and 1;
enum: 0, the number of labels less one, the labels; string, path, slot: maxlen; others 0), then a line per state, `state<TAB>name`,
each in the server's order.

The run, in a work directory made for it (--work: a new or empty directory, else a temporary one; removed at the end unless --keep):
 - The .cor's manifest (record 1) gives the core's name, id and version. The app has to be record 3 byte for byte, except the heap
   word (word 0 of the TRXHEAP: record), which cor_pack.py sets in its copy (pack_cor.sh: --heap 0). ue2emu's --firmware takes an
   ELF as it is (load address, entry, symbols; the symbols arm its fault hooks, which halt the run at vAssertCalled,
   C_exception_handler, crt0's trap handler or get_mem's panic loop) or an .app record file, but not the raw app.bin. So an ELF
   runs as a copy with record 3's heap word and an app.bin as an .app of record 3 (no symbols, no hooks): the emulator runs the
   image the bundle carries.
 - The stick: cores/<id>.cor, and the core folder's ROMs cores/<id>/roms/{basic,kernal,chars,1541-ii}.bin from the emulator's ROM
   folder (copied into the work directory). The SD card: an empty FAT volume, built by a first short ue2emu run with --usb-dir
   (ue2emu builds a volume from a directory only for a stick; --sd takes an image).
 - ue2emu headless at full speed, the hand-over page naming /Usb0/cores/<id>.cor: the application reads records 6, 7 and 8 from
   there. The control port (docs/specs/S08) and the forwards of the guest's TCP 80 (REST) and 4312 (the monitor library's TRX64
   runtime server) are on free ports of 127.0.0.1.
 - It waits for READY. from the C64 the application started (wait_ready: ue2emu's C64 shows READY. before the core's start) and
   prints the screen with the times of the steps; exports the list (when the service asks for a token, as it does when the core
   folder exists, POST /v1/ui/grant gets one); sends a JSON-RPC ping over WebSocket to TCP 4312 and prints the answer; ends ue2emu
   (control `quit`, then its process group) and prints the lines of its log that show the start: the image, the hand-over, the
   core identified, the libraries loaded, the menu's refusals, the files written, the monitor's server, a halt, the time emulated.
 - It writes the list to --out (else stdout). --check compares the list with the golden as sets of lines (blank and # lines of the
   golden left out, as descrec.py's load_ops does).
Everything but the list goes to stderr. Exit 0: done (and the list equals the golden); 1: the list differs from the golden; 2: no
list or no ping answer (the reason, and the end of ue2emu's log).
Defaults: --ue2emu $UE2EMU, else ~/Development/u64-emulator/target/release/ue2emu; --roms $UE2EMU_ROMS, else
~/Development/u64-emulator/firmware/1541ultimate/roms; --timeout 300, the seconds from ue2emu's start to the ping's answer (ue2emu
gets --max-seconds of that plus 60, so it ends by itself when this script cannot end it).
The module is also the .cor reader of ci/lane.sh (step deliver) and tools/release_trx64u.sh: read_cor, and for the sub-apps the fw-
pre-release ships beside the .cor (U20 D22) cor_libs, cor_files and check_subapps.
"""
import argparse
import base64
import hashlib
import http.client
import json
import os
import re
import shlex
import shutil
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time
import traceback
import urllib.error
import urllib.request

HOME = os.path.expanduser("~")
UE2EMU = os.environ.get("UE2EMU", os.path.join(HOME, "Development", "u64-emulator", "target", "release", "ue2emu"))
ROMS = os.environ.get("UE2EMU_ROMS", os.path.join(HOME, "Development", "u64-emulator", "firmware", "1541ultimate", "roms"))
# the ROM folder's C64 and 1541 ROMs and the names the core folder holds them under (names of desc.toml's [[rom]] tables); the
# folder's own chars.bin is not the character ROM but the 2 KiB font ue2emu draws the overlay with
CORE_ROMS = (("basic.901226-01.bin", "basic.bin"), ("kernal.901227-03.bin", "kernal.bin"),
             ("characters.901225-01.bin", "chars.bin"), ("1541-ii.bin", "1541-ii.bin"))
BOARD = ["--board", "c64u", "--caps", "25E40202", "--trx-io", "on"]
COR_MAGIC = b"TRXCOR01"
HEAP_MAGIC = b"TRXHEAP:"
WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
# ue2emu's log lines that show the start (ue2emu's own first, then the application's console), without the file system's seeks
KEY_LINES = re.compile(r"^(flash|c64-roms|trx-io|handover|c64|net|control|halted|trace): |App image|: TRX update|"
                       r"\[t\+\d+ ms\] phase |\[(core|lib|app|menu|rest|TRX)\] |\binstructions, .* s emulated|% of realtime")
NOT_KEY = re.compile(r"\[core\] fast seek: ")


class Failed(Exception):
    pass


def say(*a):
    print(*a, file=sys.stderr, flush=True)


# ---- the bundle and the app ---------------------------------------------------------------------------------------------------------
def read_cor(path):
    """The manifest, the records and the bytes of a .cor ("TRXCOR01", u32 length of the rest, then u32 type, u32 length, value;
    cor_pack.py)."""
    data = open(path, "rb").read()
    if data[:8] != COR_MAGIC or len(data) < 12 or struct.unpack_from("<I", data, 8)[0] != len(data) - 12:
        raise Failed("%s: not a .cor (header %r, %d bytes)" % (path, data[:8], len(data)))
    recs, p = {}, 12
    while p < len(data):
        if p + 8 > len(data):
            raise Failed("%s: a record header is cut off at %d" % (path, p))
        t, n = struct.unpack_from("<II", data, p)
        if p + 8 + n > len(data):
            raise Failed("%s: record %d at %d is cut off" % (path, t, p))
        recs[t] = data[p + 8:p + 8 + n]
        p += 8 + n
    if 1 not in recs or 3 not in recs:
        raise Failed("%s: no manifest (record 1) or no app (record 3)" % path)
    man = json.loads(recs[1].decode("utf-8"))
    for k in ("id", "name", "version", "app_load", "app_entry"):
        if k not in man:
            raise Failed("%s: the manifest has no %s" % (path, k))
    for k in ("id", "name", "version"):
        if "," in man[k]:
            raise Failed("%s: the manifest's %s %r has a comma, which ue2emu's --handover cannot carry" % (path, k, man[k]))
    return man, recs, data


# The sub-apps the fw- pre-release ships beside the .cor (U20 D22, MultiCore T261 D7) and where pack_cor.sh puts the same file into the .cor:
# online.u2a is a library of record 7 (its header names it online), trex_crt.u2a the file apps/trex_crt.u2a of record 8.
SUBAPPS = {"online.u2a": (7, "online"), "trex_crt.u2a": (8, "apps/trex_crt.u2a")}


def cor_libs(rec):
    """The .u2a files of record 7: "TRXL", u32 version 1, u32 count, then per library u32 length, the file, zeros to a multiple of 4 (cor_pack.py
    pack_libs)."""
    if len(rec) < 12 or rec[:4] != b"TRXL" or struct.unpack_from("<I", rec, 4)[0] != 1:
        raise Failed("record 7 is not a library set of version 1")
    p, out = 12, []
    for _ in range(struct.unpack_from("<I", rec, 8)[0]):
        if p + 4 > len(rec) or p + 4 + struct.unpack_from("<I", rec, p)[0] > len(rec):
            raise Failed("record 7: a library runs past its end")
        n = struct.unpack_from("<I", rec, p)[0]
        out.append(rec[p + 4:p + 4 + n])
        p += 4 + n + (-n % 4)
    if p != len(rec):
        raise Failed("record 7: %d bytes behind the last library" % (len(rec) - p))
    return out


def cor_files(rec):
    """{path: bytes} of record 8: "TRXF", u32 version 1, u32 count, then per file u32 length of the entry, u16 path length, u16 count of earlier
    versions, u32 size, 12 bytes per earlier version, the path, the bytes, zeros to a multiple of 4 (cor_pack.py pack_files)."""
    if len(rec) < 12 or rec[:4] != b"TRXF" or struct.unpack_from("<I", rec, 4)[0] != 1:
        raise Failed("record 8 is not a file set of version 1")
    p, out = 12, {}
    for _ in range(struct.unpack_from("<I", rec, 8)[0]):
        if p + 12 > len(rec):
            raise Failed("record 8: cut off")
        n, plen, nold, size = struct.unpack_from("<IHHI", rec, p)
        q = p + 12 + 12 * nold
        if n < 8 or q + plen + size > p + 4 + n or p + 4 + n > len(rec):
            raise Failed("record 8: an entry runs past its end")
        out[rec[q:q + plen].decode("ascii")] = rec[q + plen:q + plen + size]
        p += 4 + n
    if p != len(rec):
        raise Failed("record 8: %d bytes behind the last file" % (len(rec) - p))
    return out


def check_subapps(recs, files):
    """files: {name: bytes} of every name of SUBAPPS. Fails unless each is byte for byte the file the .cor carries."""
    for name, (t, where) in sorted(SUBAPPS.items()):
        if name not in files:
            raise Failed("no %s" % name)
        if t not in recs:
            raise Failed("the .cor has no record %d, which carries %s" % (t, name))
        if t == 7:
            ok = files[name][20:36].split(b"\0")[0] == where.encode() and files[name] in cor_libs(recs[7])
        else:
            ok = cor_files(recs[8]).get(where) == files[name]
        if not ok:
            raise Failed("%s (%d bytes) is not the %s the .cor's record %d carries" % (name, len(files[name]), where, t))


def elf_loads(data, path):
    """Entry and PT_LOAD segments (file offset, address, file size) of an ELF32 little-endian RISC-V file."""
    if len(data) < 52 or data[4] != 1 or data[5] != 1 or struct.unpack_from("<H", data, 18)[0] != 243:
        raise Failed("%s: not an ELF32 little-endian RISC-V file" % path)
    entry, phoff = struct.unpack_from("<II", data, 24)
    phentsize, phnum = struct.unpack_from("<HH", data, 42)
    segs = []
    for i in range(phnum):
        p_type, off, vaddr, _, filesz, memsz = struct.unpack_from("<6I", data, phoff + i * phentsize)
        if p_type == 1 and memsz:
            segs.append((off, vaddr, filesz))
    return entry, segs


def run_image(app, man, rec3, work):
    """The image ue2emu runs: the app with record 3's heap word, as an ELF (symbols kept) or as an .app record file."""
    data = open(app, "rb").read()
    load, entry = man["app_load"], man["app_entry"]
    elf = data[:4] == b"\x7fELF"
    if elf:
        e_entry, segs = elf_loads(data, app)
        if len(segs) != 1 or segs[0][1] != load or e_entry != entry:
            raise Failed("%s: entry 0x%x, loadable segments %s; the .cor's app loads at 0x%x and starts at 0x%x, one segment"
                         % (app, e_entry, ["0x%x+0x%x" % (v, n) for _, v, n in segs], load, entry))
        off = segs[0][0]
        image = data[off:off + segs[0][2]]
    else:
        image = data
    heap = rec3.find(HEAP_MAGIC)
    if heap >= 0 and rec3.find(HEAP_MAGIC, heap + 1) >= 0:
        raise Failed("the .cor's app holds more than one %s record" % HEAP_MAGIC.decode())
    word = heap + len(HEAP_MAGIC) if heap >= 0 else None
    same = len(image) == len(rec3) and (image == rec3 if word is None else image[:word] == rec3[:word] and image[word + 4:] == rec3[word + 4:])
    if not same:
        first = next((i for i in range(min(len(image), len(rec3))) if image[i] != rec3[i] and not (word is not None and word <= i < word + 4)),
                     min(len(image), len(rec3)))
        raise Failed("%s is not the .cor's app (record 3): %d against %d bytes, the first difference at +0x%x (0x%x)"
                     % (app, len(image), len(rec3), first, load + first))
    what = "%s (sha256 %s): %s, %d bytes at 0x%x, entry 0x%x; the .cor's record 3 " % (
        app, hashlib.sha256(data).hexdigest(), "ELF with symbols" if elf else "raw image", len(image), load, entry)
    if word is None or image[word:word + 4] == rec3[word:word + 4]:
        what += "byte for byte"
    else:
        what += "except the heap word (%s word 0: 0x%x in the app, 0x%x in the .cor), which the run takes from the .cor" % (
            HEAP_MAGIC.decode(), struct.unpack_from("<I", image, word)[0], struct.unpack_from("<I", rec3, word)[0])
    if elf:
        out = os.path.join(work, "app.elf")
        patched = data if word is None else data[:off + word] + rec3[word:word + 4] + data[off + word + 4:]
    else:
        out = os.path.join(work, "app.app")
        patched = struct.pack("<III", load, len(rec3), entry) + rec3 + b"\0" * (-len(rec3) % 4)
    with open(out, "wb") as f:
        f.write(patched)
    say("app:", what)
    say("app: ue2emu runs %s" % out)
    return out


# ---- the machine --------------------------------------------------------------------------------------------------------------------
def free_ports(n):
    socks = []
    try:
        for _ in range(n):
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            s.bind(("127.0.0.1", 0))
            socks.append(s)
        return [s.getsockname()[1] for s in socks]
    finally:
        for s in socks:
            s.close()


def log_tail(path, n=40):
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            return f.read().splitlines()[-n:]
    except OSError:
        return []


def stop_group(proc, wait_s):
    """Waits for ue2emu to end, then ends its process group (this script started it in a session of its own)."""
    try:
        return proc.wait(timeout=wait_s)
    except subprocess.TimeoutExpired:
        pass
    for sig, t in ((signal.SIGTERM, 10), (signal.SIGKILL, 10)):
        try:
            os.killpg(proc.pid, sig)
        except ProcessLookupError:
            pass
        try:
            return proc.wait(timeout=t)
        except subprocess.TimeoutExpired:
            continue
    return proc.poll()


def make_sd(base, work):
    """An empty FAT volume for the SD card: ue2emu builds it from a directory for a stick; the image is taken once that run ends."""
    src, mk = os.path.join(work, "sd_src"), os.path.join(work, "mk_sd")
    os.makedirs(src)
    os.makedirs(mk)
    script = os.path.join(mk, "quit.txt")
    with open(script, "w") as f:
        f.write("wait 1000\nquit\n")
    cmd = base + ["--flash", os.path.join(mk, "flash.bin"), "--usb-dir", src, "--usb-dir-work", os.path.join(mk, "usbwork"),
                  "--headless", "--speed", "max", "--script", script]
    say("ue2emu (the SD card's volume):", shlex.join(cmd))
    log = os.path.join(mk, "emu.log")
    with open(log, "w") as f:
        proc = subprocess.Popen(cmd, stdout=f, stderr=subprocess.STDOUT, cwd=mk, start_new_session=True)
    rc = stop_group(proc, 120)
    made = sorted(os.path.join(d, n) for d, _, ns in os.walk(os.path.join(mk, "usbwork")) for n in ns if n == "image-1.img")
    if not made:
        raise Failed("the SD volume run (exit %s) made no image-1.img; its log ends:\n%s" % (rc, "\n".join(log_tail(log))))
    sd = os.path.join(work, "sd.img")
    os.replace(made[0], sd)
    say("sd: %s, %d bytes (that run's exit %s)" % (sd, os.path.getsize(sd), rc))
    return sd


class Control:
    """ue2emu's control protocol (S08): a command per line; result lines, then `ok` or `error ...`. A `--- x ---` block (screen,
    c64screen) is read whole, so a screen line that reads ok cannot end the answer."""

    def __init__(self, port, deadline, proc):
        last = None
        while True:
            if proc.poll() is not None:
                raise Failed("ue2emu ended at start (exit %s)" % proc.returncode)
            try:
                self.sock = socket.create_connection(("127.0.0.1", port), timeout=5)
                break
            except OSError as e:
                last = e
            if time.time() > deadline:
                raise Failed("the control port 127.0.0.1:%d did not open: %s" % (port, last))
            time.sleep(0.2)
        self.sock.settimeout(120)
        self.f = self.sock.makefile("rw", encoding="utf-8", errors="replace", newline="\n")

    def cmd(self, line):
        self.f.write(line + "\n")
        self.f.flush()
        out, block = [], None
        while True:
            got = self.f.readline()
            if not got:
                raise Failed("the control port closed (after %r)" % line)
            got = got.rstrip("\n")
            if block is not None:
                out.append(got)
                if got == block:
                    block = None
            elif re.fullmatch(r"--- \S+ ---", got):
                out.append(got)
                block = got
            elif got == "ok":
                return out
            elif got.startswith("error"):
                raise Failed("control %r: %s" % (line, got))
            else:
                out.append(got)

    def c64_screen(self):
        out = self.cmd("c64screen")
        return out[1:-1] if len(out) >= 2 and out[0] == "--- c64 ---" and out[-1] == "--- c64 ---" else out

    def close(self):
        try:
            self.cmd("quit")
        except (Failed, OSError):
            pass
        try:
            self.sock.close()
        except OSError:
            pass


class Log:
    """ue2emu's log as it grows: poll() gives the lines completed since the last call."""

    def __init__(self, path):
        self.path, self.pos, self.part = path, 0, b""

    def poll(self):
        try:
            with open(self.path, "rb") as f:
                f.seek(self.pos)
                data = f.read()
        except OSError:
            return []
        self.pos += len(data)
        *done, self.part = (self.part + data).split(b"\n")
        return [l.decode("utf-8", "replace").strip() for l in done]


def wait_ready(ctl, proc, log, name, t0, deadline):
    """READY. from the C64 the application started. ue2emu's C64 shows READY. before the application starts the core (measured
    on trx64u.cor of 2026-11-24: READY. while the application still waits for its medium), so a READY. alone proves nothing. Taken
    is a READY. after three things: the base's line of the core's start (run.cc cs_step 3, "phase ROM files, then the core" or
    "phase Starting the core"); a screen without READY., sampled after that line (the application's ROM load and reset); the
    base's "[core] <name> released" (run.cc: boot_start returned running). The log is read before each screen, so a screen counts
    only for lines already written. The screen is sampled every 200 ms emulated (control `c64screen`, `wait 200`)."""
    released = "[core] %s released" % name
    seen = {}

    def mark(k, what):
        if k not in seen:
            seen[k] = (time.time() - t0, what)

    said = time.time()
    while True:
        try:
            for l in log.poll():
                if re.search(r"\] phase (ROM files, then the core|Starting the core)$", l):
                    mark("start", l)
                if released in l:
                    mark("released", l)
            screen = ctl.c64_screen()
            ready = "READY." in screen
            if ready and "start" not in seen:
                mark("ready0", "READY. on the screen before the core's start: the C64 of the power-up")
            if not ready and "start" in seen:
                mark("lost", "the screen without READY. (after the core's start)")
            if ready and "lost" in seen and "released" in seen:
                break
            if time.time() > deadline:
                raise Failed("no READY. from the core the application started, %d s after ue2emu's start (seen: %s); the screen:\n%s"
                             % (time.time() - t0, "; ".join("%.1f s %s" % v for _, v in sorted(seen.items(), key=lambda kv: kv[1][0])) or "nothing",
                                "\n".join("  |" + l for l in screen if l.strip())))
            if time.time() - said >= 30:
                say("waiting for READY: %d s" % (time.time() - t0))
                said = time.time()
            ctl.cmd("wait 200")
        except (Failed, OSError) as e:
            if proc.poll() is None:
                raise
            raise Failed("ue2emu ended before READY (exit %s; control: %s)" % (proc.returncode, e))
    mark("ready", "READY. on the screen:")
    say("READY. from the core the application started, %.1f s after ue2emu's start (wall clock):" % seen["ready"][0])
    for _, (t, what) in sorted(seen.items(), key=lambda kv: kv[1][0]):
        say("  %5.1f s  %s" % (t, what))
    for l in screen:
        if l.strip():
            say("           |" + l)


# ---- REST -----------------------------------------------------------------------------------------------------------------------------
class Rest:
    """The application's REST service on the forwarded TCP 80. No connection yet and 503 "starting" (rest.cc's start gate) are
    waited out until the deadline; a 401 is answered with a token from POST /v1/ui/grant (busy: 503, waited out too), which the
    service hands out while no token file exists."""

    def __init__(self, port, deadline):
        self.base = "http://127.0.0.1:%d" % port
        self.deadline = deadline
        self.token = None
        self.opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))

    def _call(self, method, path):
        url = self.base + path + ((("&" if "?" in path else "?") + "t=" + self.token) if self.token else "")
        req = urllib.request.Request(url, data=b"" if method == "POST" else None, method=method)
        try:
            with self.opener.open(req, timeout=20) as r:
                return r.status, r.read()
        except urllib.error.HTTPError as e:
            return e.code, e.read()
        except (OSError, http.client.HTTPException) as e:
            return None, str(e).encode()

    def _until(self, method, path):
        while True:
            status, body = self._call(method, path)
            starting = status == 503 and (method == "POST" or b'"starting":true' in body)
            if (status is not None and not starting) or time.time() > self.deadline:
                return status, body
            time.sleep(0.5)

    def get(self, path):
        status, body = self._until("GET", path)
        if status == 401 and self.token is None:
            g, gbody = self._until("POST", "/v1/ui/grant")
            if g != 200:
                raise Failed("GET %s asks for a token; POST /v1/ui/grant: %s %s" % (path, g, gbody[:300].decode("utf-8", "replace")))
            self.token = json.loads(gbody.decode("utf-8"))["token"]
            say("rest: the service asks for a token; POST /v1/ui/grant gave one")
            status, body = self._until("GET", path)
        if status != 200:
            raise Failed("GET %s: %s %s" % (path, status, body[:300].decode("utf-8", "replace")))
        return json.loads(body.decode("utf-8"))

    def paged(self, path, key):
        """All entries of a paged list: from=0, then the page's "next" until it is null; every page's count the same, all of it."""
        out, frm, counts, pages = [], 0, set(), 0
        while frm is not None:
            d = self.get("%s?from=%d" % (path, frm))
            out += d[key]
            counts.add(d["count"])
            frm = d["next"]
            pages += 1
        if len(counts) != 1 or len(out) != counts.pop():
            raise Failed("GET %s: %d entries in %d pages, counts %s" % (path, len(out), pages, sorted(counts)))
        return out, pages


def arg_text(a):
    t = a["type"]
    lo = hi = ml = 0
    labels = ""
    if t == "int":
        lo, hi = a["min"], a["max"]
    elif t == "bool":
        hi = 1
    elif t == "enum":
        hi = len(a["labels"]) - 1
        labels = "|".join(a["labels"])
    elif t in ("string", "path", "slot"):
        ml = a["maxlen"]
    return "%s:%s:%d:%d:%d:%s" % (a["name"], t, lo, hi, ml, labels)


def export(rest):
    info = rest.get("/v1/info")
    say("rest: /v1/info: core %s %s, app %s, host %r: %d ops, %d states, %d events"
        % (info.get("core"), info.get("version"), info.get("app"), info.get("host"), info.get("ops"), info.get("state"), info.get("events")))
    ops, op_pages = rest.paged("/v1/ops", "ops")
    states, st_pages = rest.paged("/v1/state", "state")
    lines = ["\t".join(["op", o["name"]] + [arg_text(a) for a in o["args"]]) for o in ops]
    lines += ["state\t%s" % s["name"] for s in states]
    say("rest: %d ops (%d page(s) of /v1/ops), %d states (%d page(s) of /v1/state)" % (len(ops), op_pages, len(states), st_pages))
    return lines, len(ops), len(states)


# ---- TCP 4312 -------------------------------------------------------------------------------------------------------------------------
def ws_send(sock, opcode, payload):
    mask = os.urandom(4)
    n = len(payload)
    head = bytes([0x80 | opcode])
    if n < 126:
        head += bytes([0x80 | n])
    elif n < 65536:
        head += bytes([0x80 | 126]) + struct.pack(">H", n)
    else:
        head += bytes([0x80 | 127]) + struct.pack(">Q", n)
    sock.sendall(head + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(payload)))


def ws_read(f, n):
    b = f.read(n)
    if len(b) != n:
        raise EOFError("closed after %d of %d bytes" % (len(b), n))
    return b


def ws_message(f, sock):
    """The next text or binary message; a ping is answered, a close ends it."""
    parts = []
    while True:
        b0, b1 = ws_read(f, 2)
        n = b1 & 0x7F
        if n == 126:
            n = struct.unpack(">H", ws_read(f, 2))[0]
        elif n == 127:
            n = struct.unpack(">Q", ws_read(f, 8))[0]
        mask = ws_read(f, 4) if b1 & 0x80 else None
        data = ws_read(f, n)
        if mask:
            data = bytes(c ^ mask[i % 4] for i, c in enumerate(data))
        op = b0 & 0x0F
        if op == 9:
            ws_send(sock, 10, data)
        elif op == 8:
            raise EOFError("the server closed the socket (%s)" % (struct.unpack(">H", data[:2])[0] if len(data) >= 2 else "no code"))
        elif op != 10:
            parts.append(data)
            if b0 & 0x80:
                return b"".join(parts)


def rpc_once(port, method):
    """One JSON-RPC 2.0 call over the WebSocket of TCP 4312; the answer to it, as text."""
    sock = socket.create_connection(("127.0.0.1", port), timeout=20)
    try:
        f = sock.makefile("rb")
        key = base64.b64encode(os.urandom(16)).decode()
        sock.sendall(("GET /?av=0 HTTP/1.1\r\nHost: 127.0.0.1:%d\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: %s\r\n"
                      "Sec-WebSocket-Version: 13\r\n\r\n" % (port, key)).encode())
        head = []
        while True:
            line = f.readline()
            if not line:
                raise EOFError("closed during the handshake")
            if line in (b"\r\n", b"\n"):
                break
            head.append(line.decode("latin-1").strip())
        want = base64.b64encode(hashlib.sha1((key + WS_GUID).encode()).digest()).decode()
        accept = [h.split(":", 1)[1].strip() for h in head[1:] if h.lower().startswith("sec-websocket-accept:")]
        if not head or " 101 " not in head[0] + " " or accept != [want]:
            raise EOFError("no WebSocket upgrade: %s" % " / ".join(head))
        ws_send(sock, 1, json.dumps({"jsonrpc": "2.0", "id": 1, "method": method}).encode())
        while True:
            text = ws_message(f, sock).decode("utf-8", "replace")
            try:
                if json.loads(text).get("id") == 1:
                    break
            except ValueError:
                continue
        try:
            ws_send(sock, 8, struct.pack(">H", 1000))
        except OSError:
            pass
        return text
    finally:
        sock.close()


def ping(port, deadline):
    """The answer to ping. No server yet, no upgrade, or the server's busy refusal (an id-null error, then close 1013) are tried
    again until the deadline; an error answer to the ping itself is not."""
    last = None
    while True:
        try:
            text = rpc_once(port, "ping")
            if "result" not in json.loads(text):
                raise Failed("TCP 4312 (127.0.0.1:%d) answered ping with: %s" % (port, text))
            return text
        except (OSError, EOFError, ValueError) as e:
            last = str(e)
        if time.time() > deadline:
            raise Failed("TCP 4312 (127.0.0.1:%d) gave no ping result; last: %s" % (port, last))
        time.sleep(1)


# ---- the run --------------------------------------------------------------------------------------------------------------------------
def run(args, work, state):
    """The run; `state` gets the log, the process and the control connection as they come, for finish() whatever happens."""
    man, recs, cor = read_cor(args.cor)
    cid, name, version = man["id"], man["name"], man["version"]
    say("cor: %s: %s (id %s, version %s), %d bytes, sha256 %s; records %s; record 6 %s, 7 %s, 8 %s"
        % (args.cor, name, cid, version, len(cor), hashlib.sha256(cor).hexdigest(), " ".join(str(t) for t in sorted(recs)),
           *("%d bytes" % len(recs[t]) if t in recs else "none" for t in (6, 7, 8))))
    say("work:", work)
    image = run_image(args.app, man, recs[3], work)

    roms = os.path.join(work, "roms")
    shutil.copytree(args.roms, roms)
    stick = os.path.join(work, "stick")
    core = os.path.join(stick, "cores", cid, "roms")
    os.makedirs(core)
    for src, dst in CORE_ROMS:
        shutil.copyfile(os.path.join(roms, src), os.path.join(core, dst))
    with open(os.path.join(stick, "cores", cid + ".cor"), "wb") as f:      # the bytes checked above, whatever the file holds by now
        f.write(cor)

    base = [args.ue2emu, "run"] + BOARD + ["--firmware", image, "--roms", roms, "--c64-roms", roms]
    sd = make_sd(base, work)

    ctl_port, rest_port, mon_port = free_ports(3)
    cmd = base + ["--flash", os.path.join(work, "flash.bin"), "--usb-dir", stick, "--usb-dir-work", os.path.join(work, "usbwork"),
                  "--sd", sd, "--headless", "--speed", "max", "--control", "127.0.0.1:%d" % ctl_port, "--net", "user",
                  "--hostfwd", "tcp:127.0.0.1:%d:80,tcp:127.0.0.1:%d:4312" % (rest_port, mon_port),
                  "--handover", "path=/Usb0/cores/%s.cor,core=%s,id=%s,version=%s" % (cid, name, cid, version),
                  "--max-seconds", str(args.timeout + 60)]
    say("ue2emu:", shlex.join(cmd))
    say("ports: control 127.0.0.1:%d, REST 127.0.0.1:%d -> 80, monitor 127.0.0.1:%d -> 4312" % (ctl_port, rest_port, mon_port))
    log = os.path.join(work, "emu.log")
    state["log"] = log
    with open(log, "w") as f:
        t0 = time.time()
        state["proc"] = proc = subprocess.Popen(cmd, stdout=f, stderr=subprocess.STDOUT, cwd=work, start_new_session=True)
    deadline = t0 + args.timeout
    state["ctl"] = ctl = Control(ctl_port, deadline, proc)
    wait_ready(ctl, proc, Log(log), name, t0, deadline)
    state["lines"], state["ops"], state["states"] = export(Rest(rest_port, deadline))
    answer = ping(mon_port, deadline)
    say("ping (TCP 4312 at 127.0.0.1:%d), %.1f s after ue2emu's start: %s" % (mon_port, time.time() - t0, answer))


def finish(state):
    """Ends ue2emu (control quit, then its process group) and prints its log's key lines."""
    proc, ctl, log = state.get("proc"), state.get("ctl"), state.get("log")
    if ctl:
        ctl.close()
    if proc:
        say("ue2emu ended, exit %s" % stop_group(proc, 30))
    if log and os.path.exists(log):
        say("ue2emu log (%s), the start:" % log)
        for l in log_tail(log, 100000):
            if KEY_LINES.search(l) and not NOT_KEY.search(l):
                say("  " + l.rstrip())


def check(lines, golden):
    want = [l for l in open(golden, encoding="utf-8").read().splitlines() if l.strip() and not l.startswith("#")]
    have, gold = set(lines), set(want)
    missing, extra = sorted(gold - have), sorted(have - gold)
    say("check against %s (%d lines; the app's list %d lines):" % (golden, len(want), len(lines)))
    say("missing from the app: %d" % len(missing))
    for l in missing:
        say("  " + l)
    say("not in the golden: %d" % len(extra))
    for l in extra:
        say("  " + l)
    return not missing and not extra


def main():
    p = argparse.ArgumentParser(description="The operations and states trx64u's application registers, read from it in ue2emu.")
    p.add_argument("--cor", required=True, help="the bundle, e.g. software/trx64u/build/cor/trx64u.cor")
    p.add_argument("--app", required=True, help="its app: build/app/app.elf (with symbols) or app.bin")
    p.add_argument("--ue2emu", default=UE2EMU, help="the emulator (default %(default)s)")
    p.add_argument("--roms", default=ROMS, help="the emulator's ROM folder, copied into the work directory (default %(default)s)")
    p.add_argument("--work", help="a new or empty directory for the run (default: a temporary one)")
    p.add_argument("--out", help="the list (default: stdout)")
    p.add_argument("--check", metavar="GOLDEN", help="compare the list with this one (e.g. software/trx64u/base/ops_trx64u.txt)")
    p.add_argument("--timeout", type=int, default=300, help="seconds from ue2emu's start to the ping's answer (default %(default)s)")
    p.add_argument("--keep", action="store_true", help="keep the work directory")
    args = p.parse_args()
    for path, what in ((args.cor, "--cor"), (args.app, "--app"), (args.ue2emu, "--ue2emu")):
        if not os.path.isfile(path):
            p.error("%s: no file %s" % (what, path))
    for src, _ in CORE_ROMS:
        if not os.path.isfile(os.path.join(args.roms, src)):
            p.error("--roms: no %s in %s" % (src, args.roms))
    if args.check and not os.path.isfile(args.check):
        p.error("--check: no file %s" % args.check)
    if args.work:
        work = os.path.abspath(args.work)
        if os.path.exists(work) and (not os.path.isdir(work) or os.listdir(work)):
            p.error("--work: %s exists and is not an empty directory" % work)
        os.makedirs(work, exist_ok=True)
    else:
        work = tempfile.mkdtemp(prefix="emu_ops-")

    state, failed = {}, None
    try:
        try:
            run(args, work, state)
        except Failed as e:
            failed = str(e)
        except KeyboardInterrupt:
            failed = "interrupted"
        except Exception:                                          # a fault of this script: its traceback, and exit 2 (1 is the check's)
            traceback.print_exc()
            failed = "an exception in emu_ops.py (above)"
        finally:
            finish(state)
        if failed:
            say("FAILED:", failed)
            tail = log_tail(state["log"]) if "log" in state else []
            if tail:
                say("ue2emu log, the last %d lines:" % len(tail))
                for l in tail:
                    say("  " + l)
            return 2
        text = "\n".join(state["lines"]) + "\n"
        if args.out:
            with open(args.out, "w", encoding="utf-8") as f:
                f.write(text)
            say("wrote %s: %d ops, %d states" % (args.out, state["ops"], state["states"]))
        else:
            sys.stdout.write(text)
            sys.stdout.flush()
        return 1 if args.check and not check(state["lines"], args.check) else 0
    finally:
        if args.keep:
            say("kept:", work)
        else:
            shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
