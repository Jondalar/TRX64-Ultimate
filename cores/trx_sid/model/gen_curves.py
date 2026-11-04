#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# T25.1: the app's filter curve tables as the app writes them into the curve
# RAM (0x10185000 / 0x10185800), for the benches and the RAM's power-up
# content. Data source: the app's software/io/audio/sid_coeff.c (the arrays)
# and u64_config.cc setFilter (mul/div per setting); the app is GPL-3.
#
#   gen_curves.py <sid_coeff.c> <outdir> [<init.mem>]
#     outdir/curve<n>.mem   n = 0..6, 1024 x 16 bit hex, as the app scales them
#     init.mem              2048 x 16 bit: curve 0 ("8580 Lo") for both UltiSIDs
import os
import re
import sys

src = open(sys.argv[1]).read()
out = sys.argv[2]
os.makedirs(out, exist_ok=True)


def array(name):
    m = re.search(r"%s\[\]\s*=\s*\{(.*?)\};" % name, src, re.S)
    v = [int(x, 0) for x in re.findall(r"0x[0-9a-fA-F]+|\d+", m.group(1))]
    assert len(v) == 1024, (name, len(v))
    return v


# u64_config.cc setFilter: (table, mul, div) per config value
SETTINGS = [
    ("sid8580_filter_coefficients", 7, 32),   # 0 8580 Lo
    ("sid8580_filter_coefficients", 2, 7),    # 1 8580 Hi
    ("sid6581_filter_coefficients", 1, 4),    # 2 6581
    ("sid6581_filter_coefficients_temp", 1, 4),  # 3 6581 Alt
    ("sid_curve_original", 2, 3),             # 4 U2 Low
    ("sid_curve_original", 1, 1),             # 5 U2 Mid
    ("sid_curve_original", 3, 2),             # 6 U2 High
]
curves = []
for n, (name, mul, div) in enumerate(SETTINGS):
    c = [(x * mul // div) & 0xffff for x in array(name)]
    curves.append(c)
    with open(os.path.join(out, "curve%d.mem" % n), "w") as f:
        f.write("// app filter curve %d: %s * %d / %d\n" % (n, name, mul, div))
        for x in c:
            f.write("%04x\n" % x)
if len(sys.argv) > 3:
    with open(sys.argv[3], "w") as f:
        f.write("// T25.1 filter curve RAM power-up content: the app's curve 0 (8580 Lo) twice (gen_curves.py)\n")
        for x in curves[0] + curves[0]:
            f.write("%04x\n" % x)
