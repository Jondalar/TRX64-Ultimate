#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# T25.3: coefficient design for the audio decimator (docs/specs/T25.3-lp-filter.md),
# with SciPy (BSD-3). Written from DSP textbook methods (CIC: Hogenauer;
# halfband and Kaiser-window FIR; polyphase fractional resampling with linear
# interpolation between phases); no HDL or code of others.
#
# Chain at our PAL clocks (same 50 MHz crystal for C64 and audio):
#   f_phi2 = 985,243.06 Hz  -> CIC R=4 N=6        -> 246,310.8 Hz
#                           -> halfband HB1 /2     -> 123,155.4 Hz
#                           -> halfband HB2 /2     ->  61,577.7 Hz
#                           -> fractional FIR      ->  48,003.07 Hz  (ratio 2400/49259 x 16, exact)
# Every stage only guards the bands that fold into 0..28 kHz (or 0..20 kHz at
# the last); the next stage removes the rest.
#
#   design_decim.py <outdir> [--check]
#     outdir/decim_hb1.mem, decim_hb2.mem   halfband taps (18-bit, unique half, centre last)
#     outdir/decim_fir.mem                  final prototype, P phases x T taps, 18-bit, P*T/2 (symmetric)
#     --check: float simulation of the whole chain, prints pass band and worst alias
import os
import sys

import numpy as np
from scipy import signal

F_PHI2 = 50e6 * 28.375 / 2 / 22.5 / 32          # 985,243.06 Hz
FS_OUT = 50e6 * 20 / 81.375 / 256                # 48,003.07 Hz
R_CIC, N_CIC = 4, 6
P = 32                                           # phases of the final filter
T = 56                                           # taps per phase (input samples)
COEF_BITS = 18

fs1 = F_PHI2 / R_CIC
fs2 = fs1 / 2
fs3 = fs2 / 2


def halfband(numtaps, fp, fs):
    """Equiripple halfband (remez on the symmetric band layout), exact zeros forced."""
    h = signal.remez(numtaps, [0, fp, fs / 2 - fp, fs / 2], [1, 0], fs=fs)
    c = (numtaps - 1) // 2
    for k in range(numtaps):
        if (k - c) % 2 == 0 and k != c:
            h[k] = 0.0
    h[c] = 0.5
    return h / np.sum(h)


def cic_resp(f):
    x = np.pi * f / F_PHI2
    num = np.sin(R_CIC * x)
    den = R_CIC * np.sin(x)
    r = np.where(np.abs(den) < 1e-12, 1.0, num / np.where(np.abs(den) < 1e-12, 1.0, den))
    return np.abs(r) ** N_CIC


hb1 = halfband(15, 20e3, fs1)     # stop from 103 kHz: guards 95..151 kHz -> 0..28 kHz
hb2 = halfband(35, 20e3, fs2)     # stop from 41.6 kHz: guards 33.6..89.6 kHz -> 0..28 kHz


def final_proto():
    """Kaiser low-pass at P*fs3, pass 20 kHz, stop 28 kHz, with the droop of
    CIC, HB1 and HB2 compensated in the pass band (frequency sampling of the
    inverse, then windowed)."""
    L = P * T
    beta = signal.kaiser_beta(95.0)
    fc = 23e3
    n = np.arange(L) - (L - 1) / 2
    h = (2 * fc / (P * fs3)) * np.sinc(2 * fc / (P * fs3) * n) * np.kaiser(L, beta)
    # droop compensation: multiply by a short correction filter's response
    f = np.linspace(0, 20e3, 64)
    droop = cic_resp(f) * np.abs(np.polyval(hb1[::-1], np.exp(-2j * np.pi * f / fs1))) * \
        np.abs(np.polyval(hb2[::-1], np.exp(-2j * np.pi * f / fs2)))
    # 3-tap symmetric correction at fs3: g(f) = a + b*cos(2 pi f / fs3), fitted to 1/droop
    A = np.stack([np.ones_like(f), np.cos(2 * np.pi * f / fs3)], axis=1)
    a, b = np.linalg.lstsq(A, 1 / droop, rcond=None)[0]
    corr = np.zeros(2 * P + 1)
    corr[P] = a
    corr[0] = corr[2 * P] = b / 2
    h = np.convolve(h, corr)[P:P + L]
    return h * P / np.sum(h)


fir = final_proto()


def quant(h, bits=COEF_BITS, scale=None):
    s = scale if scale else (2 ** (bits - 1) - 1) / np.max(np.abs(h))
    return np.round(h * s).astype(int), s


def write_mem(path, vals, bits, title):
    with open(path, "w") as f:
        f.write("// %s (design_decim.py)\n" % title)
        for v in vals:
            f.write("%0*x\n" % ((bits + 3) // 4, int(v) & ((1 << bits) - 1)))


def check():
    """Float run of the whole chain over sine sweeps."""
    rng = np.random.default_rng(1)
    worst_alias, pb = -999.0, []
    n_out = 4096
    n_in = int(n_out * F_PHI2 / FS_OUT) + 4000
    t = np.arange(n_in) / F_PHI2
    test = list(np.linspace(100, 20e3, 12)) + list(np.linspace(28e3, 490e3, 60))
    for f0 in test:
        x = np.sin(2 * np.pi * f0 * t + rng.uniform(0, 6.28))
        y = run_chain(x)[200:200 + 2048]
        w = np.hanning(len(y))
        spec = np.abs(np.fft.rfft(y * w)) / (np.sum(w) / 2)
        fr = np.fft.rfftfreq(len(y), 1 / FS_OUT)
        if f0 <= 20e3:
            k = np.argmin(np.abs(fr - f0))
            pb.append(20 * np.log10(np.max(spec[max(k - 2, 0):k + 3])))
        else:
            band = fr <= 20e3
            worst_alias = max(worst_alias, 20 * np.log10(np.max(spec[band]) + 1e-15))
    print("pass band 100 Hz..20 kHz: %.3f .. %.3f dB" % (min(pb), max(pb)))
    print("worst alias into 0..20 kHz for 28..490 kHz: %.1f dB" % worst_alias)


def run_chain(x):
    # CIC as boxcar^N then /R
    y = x
    box = np.ones(R_CIC) / R_CIC
    for _ in range(N_CIC):
        y = np.convolve(y, box)[:len(x)]
    y = y[::R_CIC]
    y = np.convolve(y, hb1)[:len(y)][::2]
    y = np.convolve(y, hb2)[:len(y)][::2]
    # fractional: output k at input position k * fs3/FS_OUT, phases linearly interpolated
    step = fs3 / FS_OUT
    out = []
    hp = np.concatenate([fir, [0.0]])
    pos = float(T)
    while pos < len(y) - 1:
        n = int(np.floor(pos))
        mu = pos - n                          # 0..1: fraction of an input sample
        # tap j uses prototype index (j + mu) * P, linear between neighbours
        idx = (np.arange(T) + mu) * P
        i0 = np.floor(idx).astype(int)
        fr = idx - i0
        i0 = np.clip(i0, 0, P * T - 1)
        c = hp[i0] * (1 - fr) + hp[np.minimum(i0 + 1, P * T)] * fr
        seg = y[n - T + 1:n + 1][::-1]
        out.append(np.dot(c, seg) / P)
        pos += step
    return np.array(out)


if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else "."
    os.makedirs(out, exist_ok=True)
    # halfbands: h[2m], m = 0..(c-1)/2 (the centre is 1/2, not stored), scale 2^17
    q1, _ = quant(hb1[0:(len(hb1) - 1) // 2:2], scale=2 ** (COEF_BITS - 1))
    q2, _ = quant(hb2[0:(len(hb2) - 1) // 2:2], scale=2 ** (COEF_BITS - 1))
    write_mem(os.path.join(out, "decim_hb1.mem"), q1, COEF_BITS, "HB1 %d taps: h[2m], centre 1/2" % len(hb1))
    write_mem(os.path.join(out, "decim_hb2.mem"), q2, COEF_BITS, "HB2 %d taps: h[2m], centre 1/2" % len(hb2))
    # final prototype: first half (symmetric), power-of-two scale 2^FIR_SHIFT
    shift = int(np.floor(np.log2((2 ** (COEF_BITS - 1) - 1) / np.max(np.abs(fir)))))
    qf, _ = quant(fir[: P * T // 2], scale=2 ** shift)
    write_mem(os.path.join(out, "decim_fir.mem"), qf, COEF_BITS,
              "final FIR, first half of %d phases x %d taps, scale 2^%d" % (P, T, shift))
    print("hb1 %d taps %s" % (len(hb1), list(q1)))
    print("hb2 %d taps %s" % (len(hb2), list(q2)))
    print("fir %d x %d, scale 2^%d (FIR_SHIFT)" % (P, T, shift))
    if "--check" in sys.argv:
        check()
