#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# T25.1: register-op logs for the SID model tests (compare_resid, the RTL
# benches). Line format: "<idle> W <reg> <val>" / "<idle> R <reg>": idle in decimal (C64 cycles without an op
# before this one), reg and val in hex.
#   gen_logs.py <outdir>
import os
import random
import sys

out = sys.argv[1] if len(sys.argv) > 1 else "."
os.makedirs(out, exist_ok=True)


class Log:
    def __init__(self, name):
        self.f = open(os.path.join(out, name + ".log"), "w")
        self.idle = 0

    def wait(self, n):
        self.idle += n

    def w(self, r, v):
        self.f.write("%d W %02x %02x\n" % (self.idle, r, v & 0xff))
        self.idle = 0

    def r(self, r):
        self.f.write("%d R %02x\n" % (self.idle, r))
        self.idle = 0

    def end(self):
        self.f.write("%d R 1f\n" % self.idle)
        self.f.close()


def voice(lg, v, freq=None, pw=None, ad=None, sr=None, ctrl=None):
    b = 7 * v
    if freq is not None:
        lg.w(b + 0, freq)
        lg.w(b + 1, freq >> 8)
    if pw is not None:
        lg.w(b + 2, pw)
        lg.w(b + 3, pw >> 8)
    if ad is not None:
        lg.w(b + 5, ad)
    if sr is not None:
        lg.w(b + 6, sr)
    if ctrl is not None:
        lg.w(b + 4, ctrl)


def osc_probe(lg, n, step):
    for _ in range(n):
        lg.wait(step)
        lg.r(0x1b)
        lg.r(0x1c)


# 1. tones: every waveform value with gate, three voices, OSC3 probes
lg = Log("tones")
lg.w(0x18, 0x0f)
for wf in range(16):
    for v in range(3):
        voice(lg, v, freq=0x1000 + 0x0731 * v + wf * 97, pw=0x0800 - 0x100 * v, ad=0x00, sr=0xf0, ctrl=(wf << 4) | 1)
    osc_probe(lg, 40, 61)
    lg.wait(3000)
    for v in range(3):
        voice(lg, v, ctrl=(wf << 4))
    lg.wait(500)
lg.end()

# 2. ADSR: every attack/decay/release rate, sustain levels, gate toggles at
# odd times (rate counter "ADSR bug"), ENV3 probes
lg = Log("adsr")
for a in range(16):
    voice(lg, 2, freq=0x2000, ad=(a << 4) | (15 - a), sr=((a * 5) & 15) << 4 | (a ^ 5), ctrl=0x11)
    osc_probe(lg, 30, 37 + 11 * a)
    voice(lg, 2, ctrl=0x10)
    osc_probe(lg, 30, 29 + 13 * a)
rnd = random.Random(1)
for i in range(400):
    voice(lg, rnd.randrange(3), ad=rnd.randrange(256), sr=rnd.randrange(256), ctrl=0x20 | rnd.randrange(2))
    lg.wait(rnd.choice([0, 1, 2, 3, 5, 9, 17, 100, 1000, 9000]))
    lg.r(0x1c)
lg.end()

# 3. filter registers (digital compare only sees them as bus writes)
lg = Log("filter")
voice(lg, 0, freq=0x0800, pw=0x0800, ad=0x00, sr=0xf0, ctrl=0x41)
voice(lg, 1, freq=0x1100, ad=0x00, sr=0xf0, ctrl=0x21)
voice(lg, 2, freq=0x0301, ad=0x00, sr=0xf0, ctrl=0x81)
for res in range(16):
    lg.w(0x17, (res << 4) | 0x07)
    for mode in (0x1f, 0x2f, 0x4f, 0x5f, 0x9f):
        lg.w(0x18, mode)
        for fc in range(0, 2048, 97):
            lg.w(0x15, fc & 7)
            lg.w(0x16, fc >> 3)
            lg.wait(40)
lg.end()

# 4. test bit and noise: lock-up through combined noise, test-bit resets,
# long test (shift register reset timer), write-back on waveform changes
lg = Log("testbit")
lg.w(0x18, 0x0f)
voice(lg, 2, freq=0x8000, pw=0x0800, ad=0x00, sr=0xf0, ctrl=0x81)
osc_probe(lg, 50, 17)
for wf in range(8, 16):
    voice(lg, 2, ctrl=(wf << 4) | 1)
    osc_probe(lg, 20, 23)
    voice(lg, 2, ctrl=0x89)          # test on
    lg.wait(100)
    voice(lg, 2, ctrl=0x81)          # test off: shift phase 2
    osc_probe(lg, 20, 13)
for a in range(8, 16):
    for b in range(8, 16):
        voice(lg, 2, ctrl=(a << 4) | 0x09)
        lg.wait(7)
        voice(lg, 2, ctrl=(b << 4) | 0x01)
        osc_probe(lg, 4, 9)
voice(lg, 2, ctrl=0x89)
osc_probe(lg, 120, 10000)            # 1.2 M cycles in test: SR reset and fade
voice(lg, 2, ctrl=0x81)
osc_probe(lg, 50, 21)
lg.end()

# 5. ring modulation and sync, every combination of the three voices
lg = Log("ringsync")
lg.w(0x18, 0x0f)
rnd = random.Random(5)
for i in range(300):
    for v in range(3):
        voice(lg, v, freq=rnd.choice([0, 1, 0x100, 0x1234, 0x8000, 0xffff, rnd.randrange(65536)]),
              ctrl=rnd.choice([0x10, 0x20, 0x40, 0x30, 0x50, 0x60, 0x70]) | rnd.choice([0, 2, 4, 6]) | 1)
    osc_probe(lg, 8, rnd.choice([1, 3, 50, 333]))
lg.end()

# 6. $D418 digis: 4-bit samples at ~8 kHz
lg = Log("digi")
for i in range(4000):
    lg.w(0x18, (i * 7 + (i >> 3)) & 15)
    lg.wait(122)
lg.end()

# 7. floating DAC: waveform 0 after each waveform, past the fade times
lg = Log("floating")
for wf in (1, 2, 4, 3, 6, 8):
    voice(lg, 2, freq=0x3333, pw=0x0400, ad=0, sr=0xf0, ctrl=(wf << 4) | 1)
    lg.wait(1000)
    voice(lg, 2, ctrl=0x01)
    osc_probe(lg, 100, 9000)
lg.end()

# 8. fuzz: random writes and reads, random gaps
for seed in range(4):
    lg = Log("fuzz%d" % seed)
    rnd = random.Random(100 + seed)
    for i in range(20000):
        if rnd.random() < 0.15:
            lg.r(rnd.randrange(32))
        else:
            r = rnd.randrange(0x19)
            if r % 7 == 4 and r < 21:
                v = rnd.randrange(256) if rnd.random() < 0.3 else (rnd.choice([1, 2, 4, 8, 3, 5, 6, 7, 9, 12, 15]) << 4) | rnd.randrange(16)
            else:
                v = rnd.randrange(256)
            lg.w(r, v)
        lg.wait(rnd.choice([0, 0, 1, 2, 3, 7, 20, 63, 200, 1000]))
    lg.end()
