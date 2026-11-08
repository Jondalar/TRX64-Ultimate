#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Read a .c64re and say what is in it.

An implementation of the container that shares no code with the firmware, so a
file the board writes is checked against something other than the writer that
made it (U8 §8 A4). Format 2 is the binary section table of
docs/specs/U8-c64re-v2-state-and-medium.md; format 1 is the gzipped JSON that
trx64 still writes, reported but not taken apart.

    tools/c64re_read.py <file.c64re> [--extract <dir>]

Exit code 0 when the container, the digest and every section length hold up.
"""
import sys
import json
import gzip
import hashlib

MAGIC = b"C64RESNP"
SEC_NAMES = {1: "BLOCK", 2: "RAM", 3: "COLOR", 4: "CARTRAM", 5: "CARTROM", 6: "DRIVE",
             7: "EEPROM"}
SEC_FIXED = {2: 65536, 3: 1024, 4: 65536, 7: 2048}  # lengths the format pins down
HEAD = 41


def u16(b, o):
    return b[o] | (b[o + 1] << 8)


def u32(b, o):
    return b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (b[o + 3] << 24)


def pad4(n):
    return (4 - (n & 3)) & 3


def read_v1(d):
    body = gzip.decompress(d[HEAD:])
    o = json.loads(body)
    print(f"  format 1, gzipped JSON: {len(d)} bytes on disk, {len(body)} unpacked "
          f"(ratio {len(d)/len(body):.3f})")
    man = o.get("manifest", {})
    print(f"  writer     {man.get('machine', {}).get('runtimeVersion', '?')}")
    cp = o.get("checkpoint", {})
    for k in ("cartBytes", "cartFlash"):
        if isinstance(cp.get(k), dict) and "b64" in cp[k]:
            print(f"  {k:10s} {len(cp[k]['b64'])*3//4} bytes")
    return 0


def read_v2(d, extract):
    bad = 0
    want = hashlib.sha256(d[HEAD:]).digest()
    if want != d[9:HEAD]:
        print(f"  !! sha256 of the body is {want.hex()[:16]}…, the head says {d[9:HEAD].hex()[:16]}…")
        bad = 1
    else:
        print(f"  sha256 of the body matches ({want.hex()[:16]}…)")

    o = HEAD
    hl = u32(d, o)
    o += 4
    hdr = d[o:o + hl].decode("ascii", "replace")
    o += hl + pad4(hl)
    print("  header     " + hdr)
    try:
        h = json.loads(hdr)
    except ValueError:
        print("  !! the header is not JSON")
        return 1

    # U9: the machine the state needs, if the file says. A file without it loads
    # only onto the machine it was taken on; one with it builds that machine.
    ms = h.get("machineState")
    if isinstance(ms, dict):
        c = ms.get("cartridge") or {}
        dr = ms.get("drive") or {}
        se = ms.get("settings") or {}
        if c:
            print(f"  cartridge  type {c.get('type')}, {c.get('bytes')} bytes, "
                  f"require {c.get('require')}, prohibit {c.get('prohibit')}")
            print(f"             from {c.get('source')}")
        print(f"  drive A    {dr.get('image', '-')} ({dr.get('mode', '-')})" if dr
              else "  drive A    -")
        print(f"  settings   uci {se.get('uci')}, reu {se.get('reu')}"
              f"/{se.get('reuSize')}, sampler {se.get('sampler')}")
    else:
        print("  machine    the file names none (written before U9)")

    ns = u16(d, o)
    o += 4
    print(f"  sections   {ns}")
    table = []
    for i in range(ns):
        sid = d[o + i * 8]
        ln = u32(d, o + i * 8 + 4)
        table.append((sid, ln))
    o += ns * 8

    total = 0
    for sid, ln in table:
        name = SEC_NAMES.get(sid, f"id {sid}")
        note = ""
        if sid in SEC_FIXED and ln != SEC_FIXED[sid]:
            note = f"  !! should be {SEC_FIXED[sid]}"
            bad = 1
        if sid == 1:
            n = h.get("blockWords", 0)
            if ln != 4 * n + 8:
                note = f"  !! should be {4*n+8} for blockWords {n}"
                bad = 1
        if o + ln > len(d):
            print(f"  {name:9s} {ln:>9d}  !! runs past the end of the file")
            return 1
        body = d[o:o + ln]
        print(f"  {name:9s} {ln:>9d}  sha {hashlib.sha256(body).hexdigest()[:16]}…{note}")
        if extract:
            with open(f"{extract}/{name.lower()}.bin", "wb") as f:
                f.write(body)
        if sid == 1:
            n = h.get("blockWords", 0)
            x = u16(body, 4 * n)
            flags = u16(body, 4 * n + 2)
            print(f"             x {x}, turbo {'yes' if flags & 1 else 'no'}"
                  f"{', pack 0x%08x' % u32(body, 4*n+4) if flags & 1 else ''}")
        o += ln + pad4(ln)
        total += ln

    if o != len(d):
        print(f"  !! {len(d) - o} bytes after the last section")
        bad = 1
    print(f"  payload    {total} bytes in sections, {len(d)} in the file "
          f"({100.0*total/len(d):.1f} % payload)")
    return bad


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    path = argv[1]
    extract = None
    if "--extract" in argv:
        extract = argv[argv.index("--extract") + 1]
    d = open(path, "rb").read()
    print(f"{path}: {len(d)} bytes")
    if len(d) < HEAD or d[:8] != MAGIC:
        print("  !! not a .c64re: the magic is missing")
        return 1
    ver = d[8]
    if ver == 2:
        return read_v2(d, extract)
    if ver == 1:
        return read_v1(d)
    print(f"  !! format {ver} is not one this reader knows")
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
