#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# T25.6: step timing check (tb_spur). A voice loops an 8 bit sine at a rate n; the sampler's output of every C64 cycle
# is compared with the ideal: the time average over the cycle of a zero order hold whose steps sit exactly on the
# 6.25 MHz / n grid. The ideal is fitted in latency and phase only (one parameter, tau). Reports the error against the
# signal and the largest spectral line of the error in 0..20 kHz relative to the fundamental of the signal.
#   spur.py                 the checks: one voice at rates 16, 35, 142 (PAL) and 35 (NTSC) must stay below -70 dB;
#                           then the same with the other voices running (information: a step that waits for the handler
#                           is a whole cycle late, the blend does not cure that)
#   spur.py rate [vmode]    one run, numbers only
import os, subprocess, sys
import numpy as np
here = os.path.dirname(os.path.abspath(__file__))
tb = os.path.join(here, "build", "tb_spur")
out = os.path.join(here, "build", "spur.txt")
TINC = {0: 1624 / 256, 1: 1565 / 256}          # 6.25 MHz ticks per C64 cycle (tinc of trx_sampler.v, PAL / NTSC)
FC = {0: 985248.6, 1: 1022684.0}               # C64 cycles per second
WAVE = np.array([int(np.int8(round(120 * np.sin(2 * np.pi * i / 50.0)))) for i in range(1000)], float) * 256 * 63 * 166 / 2048
def F(t, rate, wave):
    # integral of the held wave from 0 to t (t in ticks, >= 0)
    N = len(wave); cs = np.concatenate([[0.0], np.cumsum(wave)])
    k = np.floor(t / rate).astype(np.int64)
    q, r = np.divmod(k, N)
    return rate * (q * cs[N] + cs[r]) + wave[r] * (t - k * rate)
def ideal(rate, tinc, tau, n):
    j = np.arange(n)
    t0 = j * tinc - tau
    t0 = np.maximum(t0, 0.0)
    t1 = np.maximum(j * tinc - tau + tinc, 0.0)
    return (F(t1, rate, WAVE) - F(t0, rate, WAVE)) / tinc
def run(rate, n, vmode, extra, erate=0):
    subprocess.run(["nice", "-n", "15", tb, str(rate), str(n), out, str(vmode), str(extra), str(erate)], check=True)
    return np.loadtxt(out)
def measure(rate, vmode=0, extra=0, n=32768 + 4096, erate=0):
    x = run(rate, n, vmode, extra, erate)
    tinc = TINC[vmode]
    # tau: the step grid is anchored at the enable write; unknown shift of up to 12 cycles
    m = 600
    taus = np.arange(-2 * tinc, 14 * tinc, tinc / 128)
    sc = [np.sum((x[:m] - ideal(rate, tinc, -tau, m)) ** 2) for tau in taus]   # tau of ideal() is a delay: use -(-tau)
    i = int(np.argmin(sc))
    fine = np.arange(taus[i] - tinc / 64, taus[i] + tinc / 64, tinc / 4096)
    sc2 = [np.sum((x[:m] - ideal(rate, tinc, -t, m)) ** 2) for t in fine]
    tau = fine[int(np.argmin(sc2))]
    N = 32768
    xs = x[4096:4096 + N]
    id_ = ideal(rate, tinc, -tau, n)[4096:4096 + N]
    e = xs - id_
    w = np.hanning(N)
    fr = np.fft.rfftfreq(N, 1 / FC[vmode])
    S = np.abs(np.fft.rfft((id_ - id_.mean()) * w)); E = np.abs(np.fft.rfft((e - e.mean()) * w))
    band = fr < 20000
    fund = S[band].max()
    snr = 10 * np.log10(np.var(id_) / np.mean(e ** 2)) if np.mean(e ** 2) > 0 else 999.0
    spur = 20 * np.log10(E[band].max() / fund)
    fs = fr[band][int(np.argmax(E[band]))]
    return snr, spur, fs, -tau / tinc
if __name__ == "__main__":
    if len(sys.argv) > 1:
        snr, spur, fs, lat = measure(int(sys.argv[1]), int(sys.argv[2]) if len(sys.argv) > 2 else 0)
        print("rate %s: error %.1f dB below the signal, worst error line %.1f dB at %.0f Hz (latency %.2f cycles)" % (sys.argv[1], snr, spur, fs, lat))
        sys.exit(0)
    fails = 0
    LIM = float(os.environ.get("SPUR_LIMIT", "-70"))
    for rate, vm in [(16, 0), (35, 0), (142, 0), (35, 1)]:
        snr, spur, fs, lat = measure(rate, vm, 0)
        ok = spur < LIM
        fails += 0 if ok else 1
        print("%s rate %3d %s, one voice: error %.1f dB below the signal, worst error line %.1f dB at %.0f Hz (limit %.0f), latency %.2f cycles" % ("PASS" if ok else "FAIL", rate, "NTSC" if vm else "PAL ", snr, spur, fs, LIM, lat))
    for rate, ex, er in [(142, 7, 110), (35, 3, 110), (16, 2, 0)]:
        snr, spur, fs, lat = measure(rate, 0, ex, erate=er)
        print("INFO rate %3d with %d more voices running (rate %s): error %.1f dB below the signal, worst error line %.1f dB at %.0f Hz" % (rate, ex, er if er else "14..17", snr, spur, fs))
    print("spur %s" % ("FAIL" if fails else "PASS"))
    sys.exit(1 if fails else 0)
