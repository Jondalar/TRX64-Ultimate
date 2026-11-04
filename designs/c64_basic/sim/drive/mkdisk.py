#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""A small test D64 and its GCR tracks, as the app hands them to drive A.

    mkdisk.py out.d64 out.gcr [in.d64]

Without in.d64 a disk "T05 TEST DISK" (id T5) with two one-block PRGs,
HELLO and SECOND, is made. out.gcr holds 35 tracks, each a little-endian
32-bit length and the raw GCR bytes (as on the disk, MSB first): sync, header,
gap, sync, data, gap per sector, the rest of the track filled with gap
bytes, at the usual lengths per speed zone.
"""
import struct
import sys

SECTORS = [21] * 17 + [19] * 7 + [18] * 6 + [17] * 5
TRACK_LEN = [7692] * 17 + [7142] * 7 + [6666] * 6 + [6250] * 5
GCR = [0x0A, 0x0B, 0x12, 0x13, 0x0E, 0x0F, 0x16, 0x17,
       0x09, 0x19, 0x1A, 0x1B, 0x0D, 0x1D, 0x1E, 0x15]


def offset(t, s):
    return (sum(SECTORS[:t - 1]) + s) * 256


def gcr(data):
    out = bytearray()
    for i in range(0, len(data), 4):
        bits = 0
        for b in data[i:i + 4]:
            bits = (bits << 10) | (GCR[b >> 4] << 5) | GCR[b & 15]
        out += bits.to_bytes(5, "big")
    return bytes(out)


def make_disk():
    img = bytearray(offset(36, 0))
    used = {(18, 0), (18, 1)}
    files = [("HELLO", 17, 0, b'\x01\x08\x0b\x08\x0a\x00\x99\x22HI\x22\x00\x00\x00'),
             ("SECOND", 19, 0, b'\x01\x08' + bytes(range(64)))]
    for name, t, s, data in files:
        blk = bytearray(256)
        blk[0], blk[1] = 0, len(data) + 1
        blk[2:2 + len(data)] = data
        img[offset(t, s):offset(t, s) + 256] = blk
        used.add((t, s))
    d = bytearray(256)
    d[0], d[1] = 0, 0xFF
    for i, (name, t, s, data) in enumerate(files):
        e = bytearray(32)
        e[2], e[3], e[4] = 0x82, t, s
        e[5:21] = name.encode().ljust(16, b'\xa0')
        e[30] = 1
        d[i * 32:i * 32 + 32] = e
    d[0], d[1] = 0, 0xFF
    img[offset(18, 1):offset(18, 1) + 256] = d
    bam = bytearray(256)
    bam[0], bam[1], bam[2] = 18, 1, 0x41
    for t in range(1, 36):
        bits = 0
        free = 0
        for s in range(SECTORS[t - 1]):
            if (t, s) not in used:
                bits |= 1 << s
                free += 1
        bam[4 * t:4 * t + 4] = bytes([free, bits & 255, (bits >> 8) & 255, bits >> 16])
    bam[0x90:0xA0] = b"T05 TEST DISK".ljust(16, b'\xa0')
    bam[0xA0:0xAB] = b'\xa0\xa0T5\xa02A\xa0\xa0\xa0\xa0'
    img[offset(18, 0):offset(18, 0) + 256] = bam
    return bytes(img)


def track_gcr(img, t):
    id1, id2 = img[offset(18, 0) + 0xA2], img[offset(18, 0) + 0xA3]
    n = SECTORS[t - 1]
    per = 5 + 10 + 9 + 5 + 325
    gap = (TRACK_LEN[t - 1] - n * per) // n
    out = bytearray()
    for s in range(n):
        hdr = bytes([0x08, s ^ t ^ id2 ^ id1, s, t, id2, id1, 0x0F, 0x0F])
        data = img[offset(t, s):offset(t, s) + 256]
        chk = 0
        for b in data:
            chk ^= b
        out += b'\xff' * 5 + gcr(hdr) + b'\x55' * 9
        out += b'\xff' * 5 + gcr(bytes([0x07]) + data + bytes([chk, 0, 0])) + b'\x55' * gap
    out += b'\x55' * (TRACK_LEN[t - 1] - len(out))
    return bytes(out)


def main(argv):
    img = open(argv[2], "rb").read()[:offset(36, 0)] if len(argv) > 2 else make_disk()
    open(argv[0], "wb").write(img)
    with open(argv[1], "wb") as f:
        for t in range(1, 36):
            g = track_gcr(img, t)
            f.write(struct.pack("<I", len(g)) + g)


if __name__ == "__main__":
    main(sys.argv[1:])
