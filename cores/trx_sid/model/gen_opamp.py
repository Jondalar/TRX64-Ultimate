#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# U12 D1 (a): the 6581's mixing node as a table, for trx_sid_engine.v and the model (sidref.cpp), which both
# read the same file and interpolate it the same way, so the two stay bit exact.
#
#   gen_opamp.py <out.mem> [drive]
#
# The curve: the 6581's op-amp voltage transfer function, 33 points measured on a chip marked MOS 6581R4AR 0687 14,
# from libresidfp's FilterModelConfig6581.cpp (GPL-2.0-or-later; this file is GPL-3.0-or-later, which that permits).
# The amplifier inverts, saturates at 10.31 V and 0.81 V, and has its working point at 4.54 V where vi = vo.
# libresidfp solves it by Newton-Raphson at every summing node; U12 §5 Q7 (a) takes the curve itself as a
# waveshaper around the working point at the mixer, scaled so that small signals pass with unity gain.
#
# Units: x is the mixer sum in DCA units (full scale 0x800 * 0xff = 522240, the engine's 28 bit mix), clamped to
# 24 bits signed (+-16 full scale; at drive 0.5 V the curve is flat from -7.5 and +11.5 full scale on). `drive` is
# how many volts of op-amp input full scale reaches (0.5 V fitted with filt_cmp at Digis Level Medium, U12 D1f).
#
# Table: 1024 knots, x_k = -2^23 + 16384 k, value y_k (22 bit signed, hex two's complement). Lookup, as the
# engine does it: u = x + 2^23, i = u >> 14, f = u & 0x3fff, y = T[i] + (((T[min(i+1, 1023)] - T[i]) * f) >> 14).
import sys

OPAMP = [
    (0.81, 10.31), (2.40, 10.31), (2.60, 10.30), (2.70, 10.29), (2.80, 10.26),
    (2.90, 10.17), (3.00, 10.04), (3.10, 9.83), (3.20, 9.58), (3.30, 9.32),
    (3.50, 8.69), (3.70, 8.00), (4.00, 6.89), (4.40, 5.21), (4.54, 4.54),
    (4.60, 4.19), (4.80, 3.00), (4.90, 2.30), (4.95, 2.03), (5.00, 1.88),
    (5.05, 1.77), (5.10, 1.69), (5.20, 1.58), (5.40, 1.44), (5.60, 1.33),
    (5.80, 1.26), (6.00, 1.21), (6.40, 1.12), (7.00, 1.02), (7.50, 0.97),
    (8.50, 0.89), (10.00, 0.81), (10.31, 0.81),
]
VI0 = 4.54                      # vi = vo
SLOPE0 = -5.1                   # (4.19 - 5.21) / (4.60 - 4.40)
FULL = 2048.0 * 255.0
N, STEP, LO = 1024, 1 << 14, -(1 << 23)
YMAX = (1 << 21) - 1


def vo_of(vi):
    if vi <= OPAMP[0][0]:
        return OPAMP[0][1]
    for (x0, y0), (x1, y1) in zip(OPAMP, OPAMP[1:]):
        if vi <= x1:
            return y0 + (vi - x0) / (x1 - x0) * (y1 - y0)
    return OPAMP[-1][1]


def table(drive):
    t = []
    for k in range(N):
        x = LO + STEP * k
        y = (vo_of(VI0 + x * drive / FULL) - VI0) / (SLOPE0 * drive / FULL)
        y = int(round(y))
        assert -YMAX - 1 <= y <= YMAX, ("drive too small for 22 bits", drive, k, y)
        t.append(y)
    assert t[N // 2] == 0, t[N // 2]
    return t


def main():
    out = sys.argv[1]
    drive = float(sys.argv[2]) if len(sys.argv) > 2 else 0.5
    t = table(drive)
    with open(out, "w") as f:
        f.write("// U12 D1 (a): 6581 mixing node, drive %.3f V, 1024 x 22 bit (gen_opamp.py)\n" % drive)
        for y in t:
            f.write("%06x\n" % (y & 0x3fffff))


if __name__ == "__main__":
    main()
