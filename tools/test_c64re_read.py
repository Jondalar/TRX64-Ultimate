# SPDX-License-Identifier: GPL-3.0-or-later
"""The .c64re format 2 container, built here from the spec and read back.

The bytes are laid out from docs/specs/U8-c64re-v2-state-and-medium.md §3 alone,
not from the firmware's writer, so a disagreement between the spec and
tools/c64re_read.py shows up here instead of on the board. The firmware's own
writer is then checked against that reader on a real file (U8 §8 A1, A4).

    python3 tools/test_c64re_read.py
"""
import hashlib
import io
import json
import os
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
READER = os.path.join(HERE, "c64re_read.py")
BLOCK_WORDS = 58


def pad4(n):
    return b"\0" * ((4 - (n & 3)) & 3)


def le16(v):
    return bytes((v & 0xFF, (v >> 8) & 0xFF))


def le32(v):
    return bytes((v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF, (v >> 24) & 0xFF))


def block_section(x=104, flags=1, pack=0x12345678, words=BLOCK_WORDS):
    b = b"".join(le32(0x11110000 + i) for i in range(words))
    return b + le16(x) + le16(flags) + le32(pack)


def build(sections, header=None, break_sha=False, trailer=b""):
    """A format 2 file: head, header JSON, section table, payloads."""
    h = header if header is not None else {
        "kind": "c64re", "version": 2, "writer": "trx64u", "machine": "c64-pal",
        "blockWords": BLOCK_WORDS, "frame": 2540, "cycles": 49944903, "pc": 2300,
        "cartridgeType": 17, "source": "/SD/C64/wasteland_ef_v1.0.1.crt",
        "machineState": {
            "cartridge": {"type": 17, "bytes": 1048576, "require": 2, "prohibit": 1,
                          "source": "/SD/C64/wasteland_ef_v1.0.1.crt"},
            "drive": {"image": "/SD/C64/game.d64", "mode": "ro"},
            "settings": {"uci": False, "reu": 0, "reuSize": 0, "sampler": True},
        },
    }
    hj = (h if isinstance(h, str) else json.dumps(h, separators=(",", ":"))).encode()
    body = io.BytesIO()
    body.write(le32(len(hj)))
    body.write(hj)
    body.write(pad4(len(hj)))
    body.write(le16(len(sections)))
    body.write(le16(0))
    for sid, payload in sections:
        body.write(bytes((sid, 0)) + le16(0) + le32(len(payload)))
    for _sid, payload in sections:
        body.write(payload)
        body.write(pad4(len(payload)))
    body.write(trailer)
    raw = body.getvalue()
    digest = hashlib.sha256(raw).digest()
    if break_sha:
        digest = bytes((digest[0] ^ 0xFF,)) + digest[1:]
    return b"C64RESNP" + bytes((2,)) + digest + raw


def whole_machine():
    """What a freeze of Wasteland holds: state plus the 1 MiB EasyFlash."""
    return [
        (1, block_section()),
        (2, bytes(65536)),
        (3, bytes(1024)),
        (4, bytes(65536)),
        (5, bytes(1048576)),
        (6, bytes(2100)),
    ]


class C64reFormat2(unittest.TestCase):
    def read(self, data, name="t.c64re"):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, name)
            with open(p, "wb") as f:
                f.write(data)
            r = subprocess.run([sys.executable, "-I", READER, p],
                               capture_output=True, text=True)
            return r.returncode, r.stdout + r.stderr

    def test_a_whole_machine_reads_back(self):
        code, out = self.read(build(whole_machine()))
        self.assertEqual(code, 0, out)
        for name in ("BLOCK", "RAM", "COLOR", "CARTRAM", "CARTROM", "DRIVE"):
            self.assertIn(name, out)
        self.assertIn("sha256 of the body matches", out)
        self.assertIn("turbo yes", out)
        self.assertIn("pack 0x12345678", out)

    def test_the_file_is_the_size_the_spec_says(self):
        # 1 MiB cartridge against about 135 KB of machine (U8 §3.3), against the
        # 2 987 081 bytes format 1 wrote for the same freeze
        data = build(whole_machine())
        self.assertGreater(len(data), 1_150_000)
        self.assertLess(len(data), 1_250_000)

    def test_a_machine_without_a_cartridge_has_no_medium(self):
        secs = [(1, block_section()), (2, bytes(65536)), (3, bytes(1024))]
        data = build(secs)
        self.assertLess(len(data), 70_000)
        code, out = self.read(data)
        self.assertEqual(code, 0, out)
        self.assertNotIn("CARTROM", out)

    def test_a_changed_byte_is_caught_by_the_digest(self):
        data = bytearray(build(whole_machine()))
        data[-1] ^= 0x01
        code, out = self.read(bytes(data))
        self.assertEqual(code, 1, out)
        self.assertIn("sha256 of the body", out)

    def test_a_broken_digest_is_caught(self):
        code, out = self.read(build(whole_machine(), break_sha=True))
        self.assertEqual(code, 1, out)

    def test_a_truncated_file_does_not_read_past_its_end(self):
        code, out = self.read(build(whole_machine())[:200_000])
        self.assertEqual(code, 1, out)

    def test_an_unknown_section_is_skipped_by_its_length(self):
        secs = whole_machine() + [(99, b"a later writer's own section")]
        code, out = self.read(build(secs))
        self.assertEqual(code, 0, out)
        self.assertIn("id 99", out)

    def test_a_ram_section_of_the_wrong_length_is_refused(self):
        secs = [(1, block_section()), (2, bytes(32768)), (3, bytes(1024))]
        code, out = self.read(build(secs))
        self.assertEqual(code, 1, out)
        self.assertIn("should be 65536", out)

    def test_a_block_that_does_not_match_blockwords_is_refused(self):
        secs = [(1, block_section(words=40)), (2, bytes(65536)), (3, bytes(1024))]
        code, out = self.read(build(secs))
        self.assertEqual(code, 1, out)
        self.assertIn("blockWords", out)

    def test_bytes_after_the_last_section_are_refused(self):
        code, out = self.read(build(whole_machine(), trailer=b"junk"))
        self.assertEqual(code, 1, out)
        self.assertIn("after the last section", out)

    def test_padding_puts_every_section_on_four_bytes(self):
        for n in (2097, 2098, 2099, 2100):
            secs = [(1, block_section()), (2, bytes(65536)), (3, bytes(1024)), (6, bytes(n))]
            code, out = self.read(build(secs))
            self.assertEqual(code, 0, "drive section of %d bytes: %s" % (n, out))

    def test_a_header_with_an_unexpanded_conversion_is_refused(self):
        # What the firmware wrote on 2026-11-07: small_printf.cc has no 'l'
        # length modifier, so %lu went into the header verbatim and every field
        # behind it took the value before it - blockWords carried caps2 (U8 Sec. 9).
        hdr = ('{"kind":"c64re","version":2,"writer":"trx64u","machine":"c64-pal",'
               '"bitstreamId":%lu,"caps":844649044,"caps2":255,"blockWords":247,'
               '"frame":%lu,"cycles":%llu,"pc":1921}')
        data = build(whole_machine(), header=hdr)
        code, out = self.read(data)
        self.assertEqual(code, 1, out)
        self.assertIn("not JSON", out)

    def test_the_machine_the_state_needs_is_read_back(self):
        # U9 D1: without this the load has to assume someone set the machine up
        code, out = self.read(build(whole_machine()))
        self.assertEqual(code, 0, out)
        self.assertIn("cartridge  type 17, 1048576 bytes", out)
        self.assertIn("require 2, prohibit 1", out)
        self.assertIn("/SD/C64/game.d64 (ro)", out)
        self.assertIn("uci False", out)

    def test_a_file_from_before_u9_is_read_and_says_so(self):
        h = {"kind": "c64re", "version": 2, "writer": "trx64u", "machine": "c64-pal",
             "blockWords": BLOCK_WORDS, "cartridgeType": 17}
        code, out = self.read(build(whole_machine(), header=h))
        self.assertEqual(code, 0, out)
        self.assertIn("written before U9", out)

    def test_a_gmod2_state_carries_its_eeprom(self):
        # U9 D3a: the saved game is in the 93C86, outside the ROM window, so a
        # file without this section would hold a cartridge it cannot put back
        secs = whole_machine() + [(7, bytes(2048))]
        code, out = self.read(build(secs))
        self.assertEqual(code, 0, out)
        self.assertIn("EEPROM", out)

    def test_an_eeprom_section_of_the_wrong_length_is_refused(self):
        secs = whole_machine() + [(7, bytes(1024))]
        code, out = self.read(build(secs))
        self.assertEqual(code, 1, out)
        self.assertIn("should be 2048", out)

    def test_a_file_that_is_not_a_c64re_is_refused(self):
        code, out = self.read(b"PRG" + bytes(500))
        self.assertEqual(code, 1, out)
        self.assertIn("magic", out)


if __name__ == "__main__":
    unittest.main(verbosity=2)
