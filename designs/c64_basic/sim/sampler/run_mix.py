#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# T25.6: mixer-level check (tb_mix): the sampler lanes take their own gain bytes exactly as the UltiSID lanes do,
# and four lanes together are the sum of the four alone.
import os, subprocess, sys
import numpy as np
here = os.path.dirname(os.path.abspath(__file__))
tb = os.path.join(here, "build", "tb_mix")
out = os.path.join(here, "build", "mix.txt")
def run(mask, g, g2, n=60000):
    subprocess.run(["nice", "-n", "15", tb, "%x" % mask, "%08x" % g, "%08x" % g2, str(n), out], check=True)
    return np.loadtxt(out)
fails = 0
def check(name, ok):
    global fails
    print(("PASS " if ok else "FAIL ") + name)
    fails += 0 if ok else 1
# gain bytes: g = {U2 L, U2 R, U1 L, U1 R}, g2 = {SmpR L, SmpR R, SmpL L, SmpL R}
a = run(1, 0x00002050, 0)             # UltiSID 1: L 0x20, R 0x50
b = run(4, 0, 0x00002050)             # sampler L, same bytes
check("sampler L lane = UltiSID 1 lane (L 0x20, R 0x50), %d samples, peak %d" % (len(a), abs(a).max()), (a == b).all() and abs(a).max() > 1000)
a = run(2, 0x30400000, 0)             # UltiSID 2: L 0x30, R 0x40 (sine inverted)
b = run(8, 0, 0x30400000)             # sampler R
check("sampler R lane = UltiSID 2 lane (L 0x30, R 0x40), peak %d" % abs(a).max(), (a == b).all() and abs(a).max() > 1000)
# the left output of a lane scales with its L byte, the right with its R byte
c = run(4, 0, 0x00008000); d = run(4, 0, 0x00004000)
check("L byte 0x80 : 0x40 = 2 : 1 (%d / %d)" % (abs(c[:, 0]).max(), abs(d[:, 0]).max()), abs(abs(c[:, 0]).max() / abs(d[:, 0]).max() - 2) < 0.01 and abs(c[:, 1]).max() == 0)
c = run(8, 0, 0x80000000); d = run(8, 0, 0x00800000)
check("sampler R: L byte (31:24) feeds L only, R byte (23:16) feeds R only", abs(c[:, 0]).max() > 1000 and abs(c[:, 1]).max() == 0 and abs(d[:, 1]).max() > 1000 and abs(d[:, 0]).max() == 0)
# four lanes together
g, g2 = 0x40607050, 0x28303820
parts = [run(1 << i, g, g2) for i in range(4)]
all4 = run(15, g, g2)
# the truncations of the filter chain differ by a fraction of an LSB per stage; the DC blocker (gain 4096 at DC)
# turns that into a slowly growing offset, so compare what is left after removing a straight line
df = all4 - sum(parts)
x = np.arange(len(df))
res = np.array([df[:, c] - np.polyval(np.polyfit(x, df[:, c], 1), x) for c in range(2)])
dmax = abs(res).max()
check("four lanes = sum of the four alone (max difference %.1f of %d, after removing the DC drift; limit 0.05 %%)" % (dmax, abs(all4).max()), dmax <= abs(all4).max() * 5e-4)
# saturation: loud lanes clip at 24 bit, not wrap
sat = run(5, 0xFFFFFFFF, 0xFFFFFFFF)
check("two lanes at gain 0xFF clip at 24 bit (peak %d)" % abs(sat).max(), abs(sat).max() <= 2 ** 23 and abs(sat).max() > 2 ** 22)
print("run_mix %s" % ("FAIL" if fails else "PASS"))
sys.exit(1 if fails else 0)
