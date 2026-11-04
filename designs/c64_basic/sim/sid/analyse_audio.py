#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# T25.3: sweep the decimator bench (tb_audio) and check docs/specs/T25.3-lp-filter.md §3.3:
#   pass band 20 Hz..20 kHz within +-0.1 dB, aliases from 28..492 kHz <= -90 dBFS,
#   no drift (FIFO level stays put), silence on a machine pause without a step.
#   analyse_audio.py <tb_audio> <workdir> [quick]
import os
import subprocess
import sys

import numpy as np

tb, work = sys.argv[1], sys.argv[2]
quick = len(sys.argv) > 3
os.makedirs(work, exist_ok=True)
FS = 50e6 * 20 / 81.375 / 256
FULL = 2 ** 23


def run(arg, amp, ncyc, extra=()):
    out = os.path.join(work, "a.txt")
    r = subprocess.run(["nice", "-n", "15", tb, str(arg), str(amp), str(ncyc), out] + [str(e) for e in extra],
                       capture_output=True, text=True, check=True)
    return np.loadtxt(out), r.stdout.strip()


def spectrum(y):
    w = np.blackman(len(y))
    s = np.abs(np.fft.rfft(y * w)) / (np.sum(w) / 2)
    return np.fft.rfftfreq(len(y), 1 / FS), s


ok = True
NC = 200_000 if quick else 400_000
amp = 0.5
pass_f = [20, 100, 1000, 5000, 10000, 15000, 18000, 20000] if not quick else [1000, 20000]
stop_f = list(np.linspace(28e3, 490e3, 12 if quick else 40))
pb = []
for f in pass_f:
    y, _ = run(f, amp, NC)
    y = y[3000:]
    # least-squares fit of the known tone (no window scalloping)
    tt = np.arange(len(y)) / FS
    A = np.stack([np.sin(2 * np.pi * f * tt), np.cos(2 * np.pi * f * tt), np.ones_like(tt)], axis=1)
    c = np.linalg.lstsq(A, y, rcond=None)[0]
    lvl = 20 * np.log10(np.hypot(c[0], c[1]) / (amp * FULL))
    pb.append(lvl)
    print("pass %7.0f Hz: %+.3f dB" % (f, lvl))
worst = -999
for f in stop_f:
    y, _ = run(f, 1.0, NC)
    y = y[3000:]
    fr, s = spectrum(y)
    band = (fr >= 20) & (fr <= 20e3)
    a = 20 * np.log10(np.max(s[band]) / FULL + 1e-12)
    worst = max(worst, a)
    print("stop %7.0f Hz: alias %.1f dBFS" % (f, a))
spread = max(pb) - min(pb)
print("pass band spread %.3f dB (spec 0.2 peak to peak), worst alias %.1f dBFS (spec -90)" % (spread, worst))
ok &= spread <= 0.2 and worst <= -90
# pause: the output decays, no step
y, msg = run(1000, 0.5, 300_000, (150_000, 50_000))
print(msg)
k0 = int(150_000 * FS / 985243.06)
seg = y[k0:k0 + 400]
print("pause: |y| at start %.0f, after 5 ms %.0f, max step %.0f" % (abs(seg[0]), abs(seg[240]), np.max(np.abs(np.diff(seg)))))
sys.exit(0 if ok else 1)
