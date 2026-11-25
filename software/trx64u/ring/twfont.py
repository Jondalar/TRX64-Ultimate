#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""twfont.h, the font of Timewarp's bar (twbar.cc): ASCII 32 to 126 of the 8 x 8 font of c64_basic's overlay.

    python3 twfont.py <designs/c64_basic/mem/ovl_font8.mem> <twfont.h>

ovl_font8.mem holds 128 characters of 8 rows in ASCII order, one hex byte per line, bit 7 the left pixel: the font of Gideon's
character generator (char_generator_rom_pkg.vhd, GPL-3), written by tools/font2mem.py (PROVENANCE.md). The ring library's
Makefile runs this; the file is written only when its text changes.
"""
import os
import sys


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__.strip().splitlines()[2].strip())
    src, out = sys.argv[1], sys.argv[2]
    rows = [int(w, 16) for w in open(src).read().split()]
    if len(rows) != 128 * 8 or any(r > 0xFF for r in rows):
        sys.exit("twfont.py: %s has %d rows, 1024 bytes wanted" % (src, len(rows)))
    lines = ["// Made by twfont.py from %s: ASCII 32 to 126 of c64_basic's overlay font, 8 rows per character, bit 7 left."
             % os.path.basename(src),
             "#ifndef RING_TWFONT_H", "#define RING_TWFONT_H", "", "static const uint8_t tw_font8[95][8] = {"]
    for c in range(32, 127):
        b = ", ".join("0x%02X" % r for r in rows[c * 8:c * 8 + 8])
        lines.append("    { %s },  // %r" % (b, chr(c)))
    lines += ["};", "", "#endif", ""]
    text = "\n".join(lines)
    if os.path.exists(out) and open(out).read() == text:
        return
    with open(out, "w") as f:
        f.write(text)


if __name__ == "__main__":
    main()
