#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""UE2 session helper (T53 board session, Ultimate 64 Elite II, XC7A100T).

    tools/ue2_session.sh doctor            what this Mac has (openFPGALoader, bridge bitstream, pyftdi, package)
    tools/ue2_session.sh fetch [VERSION]   package from Gitea (default main-latest) into ~/trx64-till/pkg
    tools/ue2_session.sh detect            JTAG IDCODE must be the XC7A100T, flash JEDEC EF 40/70 18; read-only
    tools/ue2_session.sh backup [--label X]
                                           whole flash read twice, compared, sha256, stored; read-only
    tools/ue2_session.sh sram              our 100T bitstream into SRAM, our app staged, REST up; flash untouched
    tools/ue2_session.sh flash             app at 0x3C0000, bitstream at 0x0; needs a backup of THIS chip; type FLASH
    tools/ue2_session.sh restore           the backup back, chip must end byte-identical; type RESTORE
    tools/ue2_session.sh reset             reconfigure the FPGA from flash (what a power cycle does)
    tools/ue2_session.sh checks ...        the on-site checks (tools/ue2_checks.py)

    --dry-run (before the subcommand)      everything against a fake board (tools/ue2_fake.py): no USB, no hardware

Written by the flash commands, and only by them: the bytes of the two ranges of `flash` (one 4 KiB-aligned
range inside 0x000000-0x3BFFFF for the bitstream, one inside 0x3C0000-0x57FFFF for the app) and, in
`restore`, the sectors that differ from the backup. Never written: the flash status registers
(openFPGALoader is never given --unprotect-flash, --protect-flash, --bulk-erase or a quad switch; the code
refuses to start it with one), the rest of the chip, the FAT drive at 0x580000, the config at 0xFE8000.
All flash access goes through openFPGALoader's spiOverJtag bridge (the FPGA design is gone while it runs),
with --skip-reset, so no firmware runs between two steps and changes the flash under the tool.
Layout: docs/specs/T53-ue2-build.md, docs/c64u-flash-layout.md (the 100T table in w25q_flash.cc).
"""
import argparse
import datetime
import hashlib
import json
import os
import re
import shlex
import shutil
import socket
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TOOLS = ROOT / "tools"
HOME = Path(os.environ.get("UE2_HOME", str(Path.home() / "trx64-till")))
PKG = HOME / "pkg"
CABLE = "usb-blaster"
PART = "xc7a100tfgg484"
IDCODE_100T = 0x03631093            # revision nibble masked off
IDCODE_50T = 0x0362C093
JEDEC_OK = {0xEF4018, 0xEF7018}     # W25Q128 FV / JV (Gideon's driver wants EF 40 18)
SECTOR = 0x1000
CHUNK = 0x400000                    # read granularity: a stalled USB costs 4 MiB, not the chip
BIT_LIMIT = 0x3C0000                # bitstream window 0x000000-0x3BFFFF (w25q_flash.cc, FPGA type 3)
APP_AT = 0x3C0000                   # app slot of the 100T (boot ROM type 3, T15)
APP_LIMIT = 0x580000                # 0x3C0000 + 0x1C0000; the FAT flash drive starts here
FORBIDDEN = {"--unprotect-flash", "--protect-flash", "--bulk-erase", "--enable-quad", "--disable-quad",
             "--target-flash", "--external-flash", "--flash-sector", "--misc-device"}
PKG_PATH = "/api/packages/TREX/generic/trx64-ultimate"
PKG_FILES = ["c64_basic_100t.bit", "ultimate.app", "manifest.json", "manifest_100t.json", "SHA256SUMS"]
NICE = ["nice", "-n", "15"]

DRY = False
FAKE = [sys.executable, str(TOOLS / "ue2_fake.py")]


class Fail(Exception):
    pass


def say(msg=""):
    print(msg, flush=True)


def log(line):
    HOME.mkdir(parents=True, exist_ok=True)
    with open(HOME / "session.log", "a") as f:
        f.write(f"{datetime.datetime.now().isoformat(timespec='seconds')} {line}\n")


def sha(data):
    return hashlib.sha256(data).hexdigest()


def ofl_cmd():
    if DRY:
        return FAKE + ["ofl"]
    return shlex.split(os.environ.get("UE2_OFL", "openFPGALoader"))


def trxdbg_cmd():
    if DRY:
        return FAKE + ["trxdbg"]
    return shlex.split(os.environ.get("UE2_TRXDBG", f"{sys.executable} {TOOLS / 'ue2_trxdbg.py'}"))


def busy_guard():
    """Another process that owns the USB-Blaster. Bracket patterns: pgrep must not match itself."""
    if DRY:
        return
    r = subprocess.run(["pgrep", "-fl", r"[t]rxdbg\.py|[o]penFPGALoader|[l]orenz\.py|[f]lashgate"],
                       capture_output=True, text=True)
    if r.returncode == 0 and r.stdout.strip():
        raise Fail("something else holds the USB-Blaster:\n" + r.stdout)


def ofl(args, capture=False):
    """openFPGALoader. capture=True returns (rc, text); otherwise the tool talks to the terminal (progress bar)."""
    bad = FORBIDDEN & set(args)
    if bad:
        raise Fail(f"refusing to start openFPGALoader with {sorted(bad)}")
    busy_guard()
    cmd = NICE + ofl_cmd() + ["-c", CABLE] + [str(a) for a in args]
    log("run " + " ".join(cmd))
    if capture:
        r = subprocess.run(cmd, capture_output=True, text=True)
        text = r.stdout + r.stderr
        log(f"rc {r.returncode}: " + " | ".join(text.strip().splitlines()[-3:]))
        return r.returncode, text
    r = subprocess.run(cmd)
    log(f"rc {r.returncode}")
    return r.returncode, ""


# ---------------------------------------------------------------------------
# Detect
# ---------------------------------------------------------------------------

def detect(quiet=False):
    """IDCODE (chain of exactly one device, the XC7A100T) and flash JEDEC id. Reads only."""
    rc, text = ofl(["--detect"], capture=True)
    ids = [int(m, 16) for m in re.findall(r"idcode\s+0x([0-9a-fA-F]+)", text)]
    if rc != 0 or not ids:
        raise Fail("no JTAG answer. Is the UE2 switched on, the USB-Blaster plugged into the board's JTAG "
                   "header and into the Mac (plug it AFTER the UE2 is on)?\n" + text[-600:])
    if len(ids) != 1:
        raise Fail(f"{len(ids)} devices in the JTAG chain ({', '.join(f'0x{i:08X}' for i in ids)}); expected one")
    idc = ids[0]
    if idc & 0x0FFFFFFF == IDCODE_50T:
        raise Fail(f"IDCODE 0x{idc:08X} is an XC7A50T (C64U or UE2-50T), not the 100T. Nothing was touched.")
    if idc & 0x0FFFFFFF != IDCODE_100T:
        raise Fail(f"IDCODE 0x{idc:08X} is not an XC7A100T (0x?3631093). Nothing was touched.")
    rc, text = ofl(["--detect", "-f", "--fpga-part", PART, "--skip-reset"], capture=True)
    m = re.search(r"JEDEC ID:\s*0x([0-9a-fA-F]{6})", text)
    if rc != 0 or not m:
        raise Fail("the flash did not answer through the bridge (JEDEC id missing):\n" + text[-800:])
    jedec = int(m.group(1), 16)
    if jedec not in JEDEC_OK and os.environ.get("UE2_ALLOW_JEDEC", "").lower() != f"{jedec:06x}":
        raise Fail(f"flash JEDEC id {jedec:06X}: not the W25Q128 (EF4018/EF7018) of the C64U/UE2 layout. "
                   f"Nothing was touched. (Override only on purpose: UE2_ALLOW_JEDEC={jedec:06x})")
    size = 1 << (jedec & 0xFF)
    info = {"idcode": idc, "jedec": jedec, "size": size}
    if not quiet:
        say(f"JTAG IDCODE 0x{idc:08X}: XC7A100T, alone in the chain")
        say(f"flash JEDEC {jedec:06X}: W25Q128, {size >> 20} MiB")
    log(f"detect {info}")
    return info


# ---------------------------------------------------------------------------
# Flash read / write
# ---------------------------------------------------------------------------

def read_range(offset, length, retries=3):
    """Flash bytes [offset, offset+length) through the bridge, in CHUNK pieces, each retried."""
    out = bytearray()
    tmp = HOME / "work"
    tmp.mkdir(parents=True, exist_ok=True)
    pos = offset
    end = offset + length
    while pos < end:
        n = min(CHUNK, end - pos)
        part = tmp / f"read_{pos:06X}.bin"
        for attempt in range(retries):
            if part.exists():
                part.unlink()
            say(f"  read 0x{pos:06X}-0x{pos + n - 1:06X} ({n >> 10} KiB)" + (f" retry {attempt}" if attempt else ""))
            rc, _ = ofl(["--dump-flash", "--file-size", n, "--offset", pos, "--fpga-part", PART,
                         "--skip-reset", part])
            if rc == 0 and part.exists() and part.stat().st_size == n:
                break
        else:
            raise Fail(f"reading 0x{pos:06X}+{n:#x} failed {retries} times")
        out += part.read_bytes()
        part.unlink()
        pos += n
    return bytes(out)


def write_range(offset, data):
    if offset % SECTOR or len(data) % SECTOR:
        raise Fail("internal: write range not sector aligned")
    f = HOME / "work" / f"write_{offset:06X}.bin"
    f.parent.mkdir(parents=True, exist_ok=True)
    f.write_bytes(data)
    say(f"  write 0x{offset:06X}-0x{offset + len(data) - 1:06X} ({len(data) >> 10} KiB)")
    rc, _ = ofl(["-f", "--file-type", "bin", "--offset", offset, "--fpga-part", PART, "--skip-reset", f])
    f.unlink()
    if rc != 0:
        raise Fail(f"openFPGALoader failed writing 0x{offset:06X} (rc {rc}). If it said 'block protection is set': "
                   f"the flash is write protected; this tool never unprotects. The chip may be half written: "
                   f"`restore` puts the backup back.")


def diff_report(a, b, base=0, limit=8):
    d = [i for i in range(min(len(a), len(b))) if a[i] != b[i]]
    if len(a) != len(b):
        return f"lengths {len(a)} / {len(b)}"
    return f"{len(d)} differing bytes, first at " + ", ".join(f"0x{base + i:06X}" for i in d[:limit])


# ---------------------------------------------------------------------------
# Backups
# ---------------------------------------------------------------------------

def backup_sanity(data):
    notes = []
    if len(set(data[:4096])) <= 1:
        raise Fail("the backup looks like a failed read (constant bytes at the start): not accepted")
    if b"\xaa\x99\x55\x66" in data[:512]:
        notes.append("bitstream sync word at the start of the flash")
    else:
        notes.append("WARNING: no bitstream sync word in the first 512 bytes (flash holds no bitstream?)")
    hdr = [int.from_bytes(data[APP_AT + 4 * i:APP_AT + 4 * i + 4], "little") for i in range(3)]
    if hdr[1] != 0xFFFFFFFF and 0 < hdr[1] < 0x1C0000:
        notes.append(f"app header at 0x3C0000: dest 0x{hdr[0]:X} length 0x{hdr[1]:X} run 0x{hdr[2]:X}")
    else:
        notes.append(f"WARNING: app header at 0x3C0000 is {hdr[0]:08X} {hdr[1]:08X} {hdr[2]:08X}")
    return notes


def newest_backup(path=None):
    if path:
        d = Path(path).expanduser()
    else:
        c = sorted(HOME.glob("backup-2*"))
        c = [x for x in c if (x / "backup.json").exists()]
        if not c:
            raise Fail("no backup in " + str(HOME) + ". Run `backup` first.")
        d = c[-1]
    try:
        meta = json.loads((d / "backup.json").read_text())
    except (OSError, ValueError):
        raise Fail(f"{d}: no valid backup.json")
    if not meta.get("verified"):
        raise Fail(f"{d}: not a verified backup")
    data = (d / "flash.bin").read_bytes()
    if sha(data) != meta["sha256"]:
        raise Fail(f"{d}/flash.bin does not match its recorded sha256: the backup file is damaged")
    return d, meta, data


def cmd_backup(args):
    info = detect()
    size = info["size"]
    now = datetime.datetime.now()
    d = HOME / f"backup-{now:%Y%m%d-%H%M%S}" f"{'-' + args.label if args.label else ''}"
    d.mkdir(parents=True)
    say(f"\nRead 1 of 2 ({size >> 20} MiB) ...")
    a = read_range(0, size)
    say("Read 2 of 2 ...")
    b = read_range(0, size)
    if a != b:
        (d / "FAILED-read1.bin").write_bytes(a)
        (d / "FAILED-read2.bin").write_bytes(b)
        raise Fail("the two reads DIFFER: " + diff_report(a, b) + f"\nBoth kept in {d}. NOT a backup. "
                   "Check the cable/contacts and run `backup` again. Do not flash.")
    notes = backup_sanity(a)
    (d / "flash.bin").write_bytes(a)
    digest = sha(a)
    (d / "flash.bin.sha256").write_text(f"{digest}  flash.bin\n")
    meta = {"when": now.isoformat(timespec="seconds"), "label": args.label or "", "idcode": f"0x{info['idcode']:08X}",
            "jedec": f"{info['jedec']:06X}", "size": size, "sha256": digest, "verified": True,
            "verified_by": "two complete reads, byte-identical", "notes": notes}
    (d / "backup.json").write_text(json.dumps(meta, indent=2) + "\n")
    link = HOME / "backup-latest"
    if link.is_symlink() or link.exists():
        link.unlink()
    link.symlink_to(d.name)
    log(f"backup {d} sha256 {digest}")
    say(f"\nBACKUP OK: {d}\n  sha256 {digest}\n  " + "\n  ".join(notes))
    if args.mirror:
        m = Path(args.mirror).expanduser() / d.name
        shutil.copytree(d, m)
        say(f"  mirrored to {m}")
    say("\nCopy this folder to a second place NOW (NAS over the VPN, iCloud, a stick). It is the only way back.")
    say("The UE2's own firmware is not running (the FPGA holds the bridge). `reset` or a power cycle brings it back.")
    return 0


# ---------------------------------------------------------------------------
# Package: bitstream, app
# ---------------------------------------------------------------------------

def bit_to_bin(bit):
    """Xilinx .bit -> raw configuration stream (what `write_cfgmem`/the flash holds): the data of field 'e'."""
    if bit[:2] != b"\x00\x09":
        raise Fail("c64_basic_100t.bit: not a Xilinx .bit header")
    p = 2 + 9
    p += 2                                  # 0x0001
    for tag in b"abcd":
        if bit[p] != tag:
            raise Fail(f"bit header: field {chr(tag)} missing")
        n = int.from_bytes(bit[p + 1:p + 3], "big")
        p += 3 + n
    if bit[p] != ord("e"):
        raise Fail("bit header: field e missing")
    n = int.from_bytes(bit[p + 1:p + 5], "big")
    data = bit[p + 5:]
    if n != len(data):
        raise Fail(f"bit header says {n} data bytes, the file has {len(data)}")
    return data


def check_bitstream(stream):
    m = re.search(rb"\x30\x01\x80\x01(....)", stream[:4096], re.S)
    if not m:
        raise Fail("no IDCODE write in the bitstream")
    idc = int.from_bytes(m.group(1), "big")
    if idc & 0x0FFFFFFF != IDCODE_100T:
        raise Fail(f"the bitstream is built for IDCODE 0x{idc:08X}, not the XC7A100T")
    if b"\xaa\x99\x55\x66" not in stream[:4096]:
        raise Fail("no sync word in the bitstream")
    if len(stream) >= BIT_LIMIT:
        raise Fail(f"bitstream {len(stream)} bytes does not fit 0x{BIT_LIMIT:X}")
    ax = re.search(rb"\x30\x01\xa0\x01(....)", stream[:4096], re.S)
    return int.from_bytes(ax.group(1), "big") if ax else None


def check_app(app):
    if len(app) < 16:
        raise Fail("ultimate.app too short")
    dest, length, run = (int.from_bytes(app[4 * i:4 * i + 4], "little") for i in range(3))
    if not (0x10000 <= dest and 0 < length <= APP_LIMIT - APP_AT - 12 and dest <= run < dest + length
            and dest + length <= 0x04000000 and dest % 4 == 0):
        raise Fail(f"ultimate.app header unusable: dest 0x{dest:X} length 0x{length:X} run 0x{run:X}")
    if len(app) < 12 + length:
        raise Fail(f"ultimate.app: header says {length} bytes, file has {len(app) - 12}")
    return dest, length, run


def pad(data):
    return data + b"\xff" * (-len(data) % SECTOR)


def expected_sums(pkg):
    """sha256 per file: SHA256SUMS (50T stage files, ultimate.app) plus manifest_100t.json (the 100T bitstream)."""
    want = {}
    if (pkg / "SHA256SUMS").exists():
        want.update(dict((l.split()[1].lstrip("*"), l.split()[0]) for l in (pkg / "SHA256SUMS").read_text().splitlines() if l.strip()))
    if (pkg / "manifest_100t.json").exists():
        try:
            want.update({k: v for k, v in json.loads((pkg / "manifest_100t.json").read_text()).get("files", {}).items()})
            want = {k: (v["sha256"] if isinstance(v, dict) else v) for k, v in want.items()}
        except ValueError:
            pass
    return want


def load_pkg(pkg=None):
    pkg = Path(pkg) if pkg else PKG
    need = ["c64_basic_100t.bit", "ultimate.app"]
    for n in need:
        if not (pkg / n).exists():
            raise Fail(f"{pkg / n} missing. Run `fetch` (needs the VPN) or copy the package folder here.")
    want = expected_sums(pkg)
    for n in need:
        if n in want and sha((pkg / n).read_bytes()) != want[n]:
            raise Fail(f"{n} does not match the recorded sha256 (SHA256SUMS / manifest_100t.json): damaged download")
    verified = [n for n in need if n in want]
    bit = (pkg / "c64_basic_100t.bit").read_bytes()
    app = (pkg / "ultimate.app").read_bytes()
    stream = bit_to_bin(bit)
    stamp = check_bitstream(stream)
    dest, length, run = check_app(app)
    info = {}
    if (pkg / "INFO.txt").exists():
        info["info"] = (pkg / "INFO.txt").read_text().strip()
    info["verified"] = verified
    return dict(pkg=pkg, bit=bit, app=app, stream=stream, stamp=stamp, app_hdr=(dest, length, run), meta=info)


def describe_pkg(p):
    say(f"package {p['pkg']}")
    if p["meta"].get("info"):
        say("  " + p["meta"]["info"].replace("\n", "\n  "))
    say(f"  sha256 checked against the manifests: {', '.join(p['meta'].get('verified', [])) or 'NOTHING (no manifest)'}")
    say(f"  bitstream: {len(p['bit'])} bytes .bit, stream {len(p['stream'])} bytes, sha256 {sha(p['bit'])[:16]}, "
        f"build stamp {('%08X' % p['stamp']) if p['stamp'] is not None else '(none)'}, for the XC7A100T")
    d, l, r = p["app_hdr"]
    say(f"  app: {len(p['app'])} bytes, dest 0x{d:X} length 0x{l:X} run 0x{r:X}, sha256 {sha(p['app'])[:16]}")


# ---------------------------------------------------------------------------
# Board: REST, discovery
# ---------------------------------------------------------------------------

def http_json(host, path, timeout=5):
    with urllib.request.urlopen(f"http://{host}{path}", timeout=timeout) as r:
        return json.loads(r.read().decode("latin-1"))


def rest_info(host):
    try:
        return http_json(host, "/v1/info")
    except (urllib.error.URLError, OSError, ValueError):
        return None


def udp_ident(timeout=2.0):
    """The Ultimate's ident service (UDP 64): 'json' broadcast, every device answers. Returns {ip: reply}."""
    found = {}
    port = int(os.environ.get("UE2_IDENT_PORT", "64"))
    targets = ["255.255.255.255"] + [x for x in os.environ.get("UE2_IDENT_TARGETS", "").split(",") if x]
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    s.settimeout(0.3)
    for t in targets:
        try:
            s.sendto(b"json", (t, port))
        except OSError:
            pass
    end = time.time() + timeout
    while time.time() < end:
        try:
            data, addr = s.recvfrom(2048)
        except socket.timeout:
            continue
        except OSError:
            break
        try:
            found[addr[0]] = json.loads(data.decode("latin-1"))
        except ValueError:
            pass
    s.close()
    return found


def console_ip():
    rc = subprocess.run(NICE + trxdbg_cmd() + ["console"], capture_output=True, text=True)
    text = rc.stdout
    ips = re.findall(r"Status update IP = (\d+\.\d+\.\d+\.\d+)", text)
    ips = [i for i in ips if i != "0.0.0.0"]
    crash = re.search(r"GURU|ASSERTION FAIL|Exception", text)
    return (ips[-1] if ips else None), (crash.group(0) if crash else None), text


def find_host(hint=None, timeout=150):
    """The UE2's address: the hint, the app's console line over JTAG, the UDP ident broadcast, else ask once."""
    end = time.time() + timeout
    asked = False
    while True:
        cands = []
        if hint:
            cands.append(hint)
        ip, crash, _ = console_ip()
        if crash:
            raise Fail(f"the app's console shows '{crash}'. Reload the bitstream and the app (`sram` again) before blaming anything.")
        if ip:
            cands.append(ip)
        if not DRY:
            cands += [i for i, r in udp_ident(1.5).items() if r.get("trx64")]
        for c in cands:
            if rest_info(c):
                return c
        if time.time() > end:
            if asked:
                raise Fail("the app's REST does not answer")
            asked = True
            if sys.stdin.isatty():
                typed = input("The UE2's IP is not found yet. Type it (menu: shown on screen / router list) or Enter to wait more: ").strip()
                if typed:
                    hint = typed
            end = time.time() + 60
        else:
            say("  waiting for the app's network ...")
            time.sleep(4)


def print_info(host):
    inf = rest_info(host) or {}
    say(f"  REST at {host}: {inf.get('product')}  hostname {inf.get('hostname')}  id {inf.get('unique_id')}")
    say(f"  app {inf.get('firmware_version')} {inf.get('git_commit_hash')}  fpga {inf.get('fpga_version')}  trx64 {inf.get('trx64')}")
    return inf


def wait_rest(hint=None):
    host = find_host(hint)
    HOME.mkdir(parents=True, exist_ok=True)
    (HOME / "host.txt").write_text(host + "\n")
    inf = print_info(host)
    if not inf.get("trx64"):
        raise Fail("REST answers but it is not our core (no 'trx64' block): this is Gideon's firmware, or the app did not find our core")
    return host


# ---------------------------------------------------------------------------
# Commands
# ---------------------------------------------------------------------------

def cmd_detect(args):
    detect()
    say("nothing was written. The UE2's own firmware is stopped while the bridge is loaded; `reset` restarts it.")
    return 0


def cmd_reset(args):
    rc, _ = ofl(["--reset"])
    if rc:
        raise Fail("reconfiguration failed")
    say("The FPGA configures from the flash now (about 10 s). For a real power-on test: unplug the Blaster's USB, "
        "power off, power on (the Blaster on the header at power-on can stop the configuration).")
    return 0


def cmd_sram(args):
    p = load_pkg(args.pkg)
    describe_pkg(p)
    detect()
    bit = Path(p["pkg"]) / "c64_basic_100t.bit"
    ok = False
    for attempt in range(3):
        rc, text = ofl(["-m", bit], capture=True)
        if rc == 0 and not re.search(r"error|fail", text, re.I):
            ok = True
            break
        time.sleep(1)
    if not ok:
        raise Fail("loading the bitstream into SRAM failed\n" + text[-500:])
    say("bitstream loaded into SRAM (flash untouched)")
    time.sleep(2)
    app = Path(p["pkg"]) / "ultimate.app"
    r = subprocess.run(NICE + trxdbg_cmd() + ["boot", "staged", str(app)])
    if r.returncode:
        raise Fail("`trxdbg boot staged` failed (boot ROM did not take the app). Run `sram` again.")
    c = subprocess.run(NICE + trxdbg_cmd() + ["boot", "result"], capture_output=True, text=True).stdout
    say(c.strip())
    host = wait_rest(args.host)
    try:
        urllib.request.urlopen(urllib.request.Request(
            f"http://{host}/v1/configs/Speaker%20Mixer/Speaker%20Enable?value=Disabled", method="PUT"), timeout=5).read()
    except OSError:
        pass
    say(f"\nSRAM session up: http://{host}/  (host saved in {HOME / 'host.txt'}). Next: `checks`.")
    say("A power cycle brings back whatever is in the flash. SRAM runs only until then.")
    return 0


def confirm(word, what):
    say(what)
    try:
        typed = input(f"Type {word} to continue (anything else aborts): ")
    except EOFError:
        typed = ""
    if typed.strip() != word:
        raise Fail("not confirmed; nothing was written")


def cmd_flash(args):
    p = load_pkg(args.pkg)
    describe_pkg(p)
    bdir, meta, backup = newest_backup(args.backup)
    info = detect()
    if meta["size"] != info["size"] or int(meta["jedec"], 16) != info["jedec"]:
        raise Fail("the backup is of a different flash chip")
    plan = [("app", APP_AT, pad(p["app"])), ("bitstream", 0, pad(p["stream"]))]
    spans = []
    for name, off, data in plan:
        lo, hi = (0, BIT_LIMIT) if name == "bitstream" else (APP_AT, APP_LIMIT)
        if off % SECTOR or not (lo <= off and off + len(data) <= hi):
            raise Fail(f"{name} range 0x{off:X}+0x{len(data):X} leaves its window 0x{lo:X}-0x{hi:X}")
        spans.append((off, off + len(data)))
    say(f"\nbackup {bdir.name} (sha256 {meta['sha256'][:16]})")
    say("Check that this chip is the one in the backup: reading both ranges ...")
    for name, off, data in plan:
        cur = read_range(off, len(data))
        if cur != backup[off:off + len(data)]:
            raise Fail(f"the flash at 0x{off:06X} differs from the backup ({diff_report(cur, backup[off:off + len(data)], off)}). "
                       "Either the backup is of another UE2 or the flash changed since. Run `backup` again for this chip.")
    say("  the chip matches the backup in both ranges")
    lines = [f"\nWILL WRITE (nothing else, the status register is never touched):"]
    for name, off, data in plan:
        lines.append(f"  {name:9s} 0x{off:06X}-0x{off + len(data) - 1:06X}  {len(data)} bytes")
    lines.append("  order: app first, bitstream last. If anything fails: `restore`.")
    confirm("FLASH", "\n".join(lines))
    log("flash confirmed")
    for name, off, data in plan:
        write_range(off, data)
        got = read_range(off, len(data))
        if got != data:
            raise Fail(f"VERIFY FAILED for the {name} at 0x{off:06X}: {diff_report(got, data, off)}. "
                       "The flash is in an unknown state. Run `restore` now.")
        say(f"  {name} verified by readback")
    say("Reading the whole chip once more: everything outside the two ranges must equal the backup ...")
    full = read_range(0, info["size"])
    want = bytearray(backup)
    for name, off, data in plan:
        want[off:off + len(data)] = data
    if full != bytes(want):
        raise Fail("the chip differs from backup + new ranges: " + diff_report(full, bytes(want)) + ". Run `restore`.")
    log(f"flash done sha256 {sha(full)}")
    say(f"\nFLASH OK. chip sha256 {sha(full)[:16]}. Ranges verified, all other bytes identical to the backup.")
    if not args.no_reset:
        say("\nReconfiguring from flash (what a power cycle does) ...")
        if ofl(["--reset"])[0]:
            raise Fail("reconfiguration failed (flash was written correctly)")
        time.sleep(12)
        try:
            wait_rest(args.host)
            say("The flash-booted core and app answer. Now do a REAL power cycle (Blaster USB unplugged), then `checks`.")
        except Fail as e:
            say(f"After the reconfiguration: {e}\nPower-cycle without the Blaster and look at the screen; "
                "`restore` is the way back.")
    return 0


def sector_runs(cur, want, gap=0x10000):
    """Runs of 4 KiB sectors where cur != want, runs closer than `gap` merged (fewer bridge loads)."""
    runs = []
    for s in range(0, len(want), SECTOR):
        if cur[s:s + SECTOR] != want[s:s + SECTOR]:
            if runs and s - runs[-1][1] <= gap:
                runs[-1][1] = s + SECTOR
            else:
                runs.append([s, s + SECTOR])
    return runs


def cmd_restore(args):
    bdir, meta, backup = newest_backup(args.backup)
    info = detect()
    if meta["size"] != info["size"] or int(meta["jedec"], 16) != info["jedec"]:
        raise Fail("the backup is of a different flash chip")
    say(f"\nbackup {bdir.name}, sha256 {meta['sha256'][:16]}, {meta['size'] >> 20} MiB")
    say("Reading the whole chip to see what differs ...")
    cur = read_range(0, info["size"])
    runs = [[0, len(backup)]] if args.full else sector_runs(cur, backup)
    if not runs:
        say("The flash already equals the backup byte for byte. Nothing to write.")
    else:
        tot = sum(b - a for a, b in runs)
        confirm("RESTORE", f"\nWILL WRITE BACK {tot >> 10} KiB in {len(runs)} range(s) from the backup"
                           f"{' (whole chip)' if args.full else ' (only sectors that differ; the rest is already identical)'}:\n"
                + "\n".join(f"  0x{a:06X}-0x{b - 1:06X}" for a, b in runs[:20]) + ("\n  ..." if len(runs) > 20 else ""))
        for a, b in runs:
            write_range(a, backup[a:b])
    say("Verifying the whole chip against the backup ...")
    full = read_range(0, info["size"])
    if full != backup:
        raise Fail("RESTORE NOT VERIFIED: " + diff_report(full, backup) + ". Run `restore` again; if it persists check cable and power.")
    log(f"restore ok sha256 {sha(full)}")
    say(f"\nRESTORE OK: the chip is byte-identical to the backup (sha256 {sha(full)[:16]}).")
    if not args.no_reset:
        ofl(["--reset"])
        say("The FPGA reconfigures from the restored flash (the UE2's own firmware). Power-cycle once with the Blaster unplugged.")
    return 0


def tea_value(key):
    """The first `key:` of the tea login's config ("" without one)."""
    cfg = Path.home() / "Library/Application Support/tea/config.yml"
    if cfg.exists():
        for line in cfg.read_text().splitlines():
            if line.strip().startswith(key + ":"):
                return line.split(key + ":", 1)[1].strip()
    return ""


def gitea_token():
    return os.environ.get("GITEA_TOKEN", "") or tea_value("token")


def gitea_urls():
    """The Gitea servers to try in order: GITEA_URL (one or more, space-separated, e.g. the name and the address for a VPN without DNS),
    then the url: of the tea login. The public tree names no host (U20 A8)."""
    urls = os.environ.get("GITEA_URL", "").split() + [tea_value("url")]
    return [u.rstrip("/") for i, u in enumerate(urls) if u and u not in urls[:i]]


def cmd_fetch(args):
    ver = args.version or "main-latest"
    tok = gitea_token()
    dest = HOME / (f"pkg-{ver}")
    dest.mkdir(parents=True, exist_ok=True)
    base = None
    last = Fail("no Gitea server: set GITEA_URL (or log in with tea)")
    for u in gitea_urls():
        try:
            req = urllib.request.Request(f"{u}{PKG_PATH}/{ver}/SHA256SUMS", headers={"Authorization": f"token {tok}"})
            with urllib.request.urlopen(req, timeout=15) as r:
                (dest / "SHA256SUMS").write_bytes(r.read())
            base = u
            break
        except (urllib.error.URLError, OSError) as e:
            last = e
    if not base:
        raise Fail(f"Gitea is not reachable (VPN up?): {last}")
    for n in PKG_FILES[:-1]:
        req = urllib.request.Request(f"{base}{PKG_PATH}/{ver}/{n}", headers={"Authorization": f"token {tok}"})
        try:
            with urllib.request.urlopen(req, timeout=120) as r:
                (dest / n).write_bytes(r.read())
            say(f"  {n}")
        except urllib.error.HTTPError as e:
            if n == "c64_basic_100t.bit":
                raise Fail(f"package {ver} has no 100T bitstream (HTTP {e.code}). The 100T stage runs only for a dispatch with ue2=true. "
                           "Only when this prints 0 (one waiting run is the most the queue keeps, a new one replaces it):\n"
                           "  tea api --login nas \"/repos/TREX/TRX64-Ultimate/actions/runs?status=waiting\" | grep -c head_sha\n"
                           "then:\n"
                           "  tea api --login nas -X POST /repos/TREX/TRX64-Ultimate/actions/workflows/pipeline.yml/dispatches "
                           "-d '{\"ref\":\"main\",\"inputs\":{\"ue2\":\"true\"}}'\n"
                           "The 100T stage alone took 35 min on 2026-10-04 (T53 §7), plus the earlier stages and the queue; then `fetch main-latest` (or main-<8 digit sha>).")
            if n.startswith("manifest_100t"):
                continue
            raise Fail(f"{n}: HTTP {e.code}")
    mf = {}
    for n in ("manifest.json", "manifest_100t.json"):
        if (dest / n).exists():
            try:
                mf[n] = json.loads((dest / n).read_text())
            except ValueError:
                pass
    m100 = mf.get("manifest_100t.json", {})
    m = mf.get("manifest.json", {})
    (dest / "INFO.txt").write_text(
        f"version {ver}, fetched {datetime.datetime.now():%Y-%m-%d %H:%M}\n"
        f"trx64 commit {m100.get('trx64_commit') or m.get('trx64_commit')}, app commit {m.get('app_commit')}, "
        f"100T WNS {m100.get('wns_ns')}\n")
    link = PKG
    if link.is_symlink() or link.exists() and not link.is_dir():
        link.unlink()
    if link.is_dir() and not link.is_symlink():
        shutil.move(str(link), str(HOME / f"pkg-old-{datetime.datetime.now():%H%M%S}"))
    link.symlink_to(dest.name)
    p = load_pkg()
    describe_pkg(p)
    say(f"\npkg -> {dest}")
    return 0


def cmd_doctor(args):
    bad = 0

    def line(ok, msg):
        nonlocal bad
        say(("ok    " if ok else "MISSING ") + msg)
        bad += 0 if ok else 1
    exe = shutil.which(ofl_cmd()[0]) if not DRY else "fake"
    line(bool(exe), f"openFPGALoader: {exe}")
    if exe and not DRY:
        v = subprocess.run([exe, "-V"], capture_output=True, text=True).stdout.strip()
        say(f"      {v}")
        share = Path(os.path.realpath(exe)).parent.parent / "share/openFPGALoader"
        br = share / f"spiOverJtag_{PART}.bit.gz"
        line(br.exists(), f"spiOverJtag bridge for {PART}: {br}")
    try:
        import pyftdi  # noqa: F401
        line(True, "pyftdi importable (trxdbg, lorenz, checks)")
    except ImportError:
        line(DRY, f"pyftdi not importable in {sys.executable}: use tools/ue2_session.sh (it picks the JTAG venv)")
    tool = Path(os.environ.get("TRX_TOOLING", Path.home() / "Development/1541ultimate-tools/tooling"))
    line((tool / "u64ii_bscan.py").exists(), f"USB-Blaster driver of the tools fork: {tool}")
    line((TOOLS / "trxdbg.py").exists(), "tools/trxdbg.py")
    try:
        p = load_pkg()
        line(True, "package in " + str(p["pkg"]))
        describe_pkg(p)
    except Fail as e:
        line(False, str(e))
    bk = sorted(HOME.glob("backup-2*"))
    say(f"      backups: {', '.join(x.name for x in bk) or 'none yet'}")
    line(bool(gitea_token()), "tea token for Gitea (only `fetch` needs it)")
    lz = Path(os.environ.get("LORENZ_DIR", Path.home() / "Development/C64/Tools/C64ReverseEngineeringMCP/samples/vice-testprogs/lorenz-2.15"))
    line(lz.exists(), f"Lorenz disks for the subset: {lz}")
    return 1 if bad and not args.keep_going else 0


def main(argv):
    global DRY
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dry-run", action="store_true")
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("doctor")
    s.add_argument("--keep-going", action="store_true")
    s = sub.add_parser("fetch")
    s.add_argument("version", nargs="?")
    sub.add_parser("detect")
    s = sub.add_parser("backup")
    s.add_argument("--label")
    s.add_argument("--mirror", help="also copy the backup folder here")
    s = sub.add_parser("sram")
    s.add_argument("--pkg")
    s.add_argument("--host", help="UE2 address if the auto-discovery fails")
    s = sub.add_parser("flash")
    s.add_argument("--pkg")
    s.add_argument("--backup")
    s.add_argument("--host")
    s.add_argument("--no-reset", action="store_true")
    s = sub.add_parser("restore")
    s.add_argument("--backup")
    s.add_argument("--full", action="store_true", help="write the whole chip, not only the differing sectors")
    s.add_argument("--no-reset", action="store_true")
    sub.add_parser("reset")
    args = ap.parse_args(argv)
    DRY = args.dry_run
    srv = None
    if DRY:
        global HOME, PKG
        if "UE2_HOME" not in os.environ:
            HOME = Path.home() / "trx64-till-dryrun"
            PKG = HOME / "pkg"
        os.environ["UE2_HOME"] = str(HOME)
        os.environ["UE2_FAKE_DIR"] = str(HOME / "fake")
        subprocess.run(FAKE + ["prepare"], check=True)
        if args.cmd in ("sram", "flash"):
            srv = subprocess.Popen(FAKE + ["rest"], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
            port = srv.stdout.readline().strip()
            os.environ["UE2_FAKE_REST"] = f"127.0.0.1:{port}"
            args.host = f"127.0.0.1:{port}"
    try:
        return {"doctor": cmd_doctor, "fetch": cmd_fetch, "detect": cmd_detect, "backup": cmd_backup,
                "sram": cmd_sram, "flash": cmd_flash, "restore": cmd_restore, "reset": cmd_reset}[args.cmd](args)
    except Fail as e:
        print(f"\nSTOP: {e}", file=sys.stderr)
        log(f"STOP {e}")
        return 1
    except KeyboardInterrupt:
        print("\ninterrupted", file=sys.stderr)
        return 130
    finally:
        if srv is not None:
            srv.terminate()


if __name__ == "__main__":
    argv = sys.argv[1:]
    if "checks" in argv[:2]:
        import ue2_checks
        dry = "--dry-run" in argv[:1]
        sys.exit(ue2_checks.main(argv[argv.index("checks") + 1:], dry=dry))
    sys.exit(main(argv))
