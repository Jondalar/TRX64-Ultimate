#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""U16: the HDMI modes of c64_basic (trx64g / trx64u), derived, proven and written as designs/c64_basic/hdmi_modes.vh.

  tools/hdmi_modes.py table      print the rows (spec U16 D2)
  tools/hdmi_modes.py gen        write designs/c64_basic/hdmi_modes.vh
  tools/hdmi_modes.py check      every limit of every row, the frame lock proven exactly, the CDCE bytes decoded from the file,
                                 the scaler's ring margins, the 1080p rows equal to what the design shipped before U16, the file current
  tools/hdmi_modes.py selftest   the negative controls: each mutation must be caught

Ported from TRX64U-MultiCore tools/hdmi_modes.py (its host edition, T164 WP1, at 2ac4548b0; both repositories are Alex Damhuis'):
the clock chain, the CDCE register rules (regs, decode_regs, chip_check), cdce_exact, lock_for, Gideon's mode table and the index
scheme are that tool's. This edition differs (spec U16 D1-D3): every row is locked to the VIC frame (no frame buffer), the rows are
0, 1, 2, 6, 7, 8, 9, row 6 (480p) has 800 x 526 instead of CEA's 525 lines (525 does not lock), the pixel rate is given for both
System Modes of a family, and each row carries, for each of the three scales (Integer, Fit, Fill), its frame align line (hm_align)
and the scaler's and the text layer's geometry (hm_geom).

Source of the timings: Gideon's table, software/ultimate-app/software/u64/hdmi_scan.cc (sm_*), as MultiCore took it. The clock
chain (designs/c64_basic/top.v): dot4x -> mmcm_ref x31.25/26 -> CDCE (cdce_mode.v, I2C 0x64) -> GTP REFCLK = TMDS clock f_t ->
usrclk = f_t/2 -> mmcm_pix (x12, /6) -> pix_clk = f_t. Pixel repetition ("gear" g): f_t = g x the pixel clock; every pixel lasts g
pix_clk and tmds_rep.v sends each TMDS bit g times, so the wire carries one symbol per pixel (AVI PR = 0).

CDCE register rules (TI CDCEL913/CDCE913 data sheet, PLL multiplier/divider definition; the control: the two blobs cdce_i2c.v
shipped before U16, 55/14 and 990/263, byte for byte): fvco = fin x N/M; P = 4 - floor(log2(fvco/fin)); N' = N x 2^P; Q = floor(N'/M),
16 <= Q <= 63; R = N' - M x Q, 0 <= R <= 51; 1 <= M <= 511, 1 <= N <= 4095; VCO range bits: <125 MHz 00, 125-150 01, 150-175 10,
>=175 11. Registers 0x18..0x1B: N[11:4]; N[3:0],R[8:5]; R[4:0],Q[5:3]; Q[2:0],P,range.
"""
import math, os, re, sys
from fractions import Fraction as F

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VH = os.path.join(ROOT, "designs/c64_basic/hdmi_modes.vh")

# mode_ctl.v: dot4x of the four System Modes (mmcm_sys from 50 MHz: D, M, O0) and their family (0 = 63 x 312, 1 = 65 x 263)
DOT4X = {0: F(50_000_000) * F(5675, 100) / 3 / 30,      # PAL      31,527,778 Hz
         1: F(50_000_000) * F(47125, 1000) / 3 / 24,    # NTSC     32,725,694 Hz
         2: F(50_000_000) * F(4325, 100) / 3 / 22,      # PAL-60   32,765,152 Hz
         3: F(50_000_000) * F(35375, 1000) / 2 / 28}    # NTSC-50  31,584,821 Hz
VM_FAM = {0: 0, 1: 1, 2: 1, 3: 0}
VM_PRIMARY = {0: 0, 1: 1}                       # hm_rate: PAL for the 50 Hz rows, NTSC for the 60 Hz rows
VM_ALT = {0: 3, 1: 2}                           # hm_rate_alt: NTSC-50, PAL-60
REF_PER_DOT4X = F(3125, 100) / 26               # top.v mmcm_ref: the CDCE reference is 125/104 of dot4x
VIC_FRAME = {0: 63 * 312 * 32, 1: 65 * 263 * 32}  # dot4x clocks per VIC frame (8 dots x 4 per cycle)
VIC_LINES = {0: 312, 1: 263}
VIC_LINE = {0: 63 * 32, 1: 65 * 32}             # dot4x clocks per VIC line

HDMI_NAMES = ["576p/480p", "1280x720", "1920x1080", "800x600", "1024x768", "1280x1024"]
# (vic, pixel clock Hz, hact, hfp, hsync, hbp, hpol, vact, vfp, vsync, vbp, vpol, gear, AVI aspect) by index family * 6 + hdmi
# (MultiCore's MODES / HOST_MODES; row 6: vfp 11, one line more than CEA's 10, U16 D2)
MODES = {
    0: (17, 27067270, 720, 12, 64, 68, 0, 576, 5, 5, 39, 0, 5, 1),
    1: (19, 74434928, 1280, 440, 40, 220, 1, 720, 5, 5, 20, 1, 2, 2),
    2: (31, 148869856, 1920, 528, 44, 148, 1, 1080, 4, 5, 36, 1, 1, 2),
    6: (1, 25126964, 640, 16, 96, 48, 0, 480, 11, 2, 33, 0, 5, 1),
    7: (4, 74034804, 1280, 110, 40, 220, 1, 720, 5, 5, 20, 1, 2, 2),
    8: (16, 148069608, 1920, 88, 44, 148, 1, 1080, 4, 5, 36, 1, 1, 2),
    9: (0, 41028000, 800, 40, 124, 88, 1, 600, 20, 5, 25, 1, 3, 0),
    10: (0, 64995840, 1024, 24, 136, 160, 0, 768, 3, 6, 29, 0, 2, 0),     # not a row: no CDCE setting locks it (the selftest's control)
    11: (0, 106546560, 1280, 48, 112, 248, 1, 1024, 1, 3, 24, 1, 1, 0),   # not a row, as 10
}
NAMES = ("vic", "pclk", "ha", "hfp", "hs", "hbp", "hpol", "va", "vfp", "vs", "vbp", "vpol", "gear", "asp")
ROWS = {0, 1, 2, 6, 7, 8, 9}                    # U16 D2: 1024 x 768 and 1280 x 1024 do not lock, the 50 Hz PC rows do not exist
# CEA-861 timing of each VIC the table uses: (hact, vact, htot, vtot, aspect); row 6 is allowed one line more (U16 D2)
CEA = {1: (640, 480, 800, 525, 1), 17: (720, 576, 864, 625, 1), 4: (1280, 720, 1650, 750, 2), 19: (1280, 720, 1980, 750, 2),
       16: (1920, 1080, 2200, 1125, 2), 31: (1920, 1080, 2640, 1125, 2)}
CEA_EXTRA_LINES = {6: 1}
GEARS = (1, 2, 3, 5)
GTP_VCO = (1.6e9, 3.3e9)                        # GTP PLL VCO, DS181 (7 Series GTP, -1); the wizard's N = 20, M = 1, D = 4 (build.tcl)
GTP_NMUL = 20
GTP_MIN_LINE = 0.5e9
MMCM_VCO = (600e6, 1200e6)                      # MMCME2, -1
MMCM_M = 12                                     # mmcm_pix (top.v): usrclk x 12, /6 -> f_t
CDCE_VCO = (80e6, 200e6)                        # CDCEL824 (SCAS945A)
HDMI_MIN_PIX = 25e6
# The VIC window the scaler shows (vic_scaler.v X0, Y0, Y0_60) and its size per family; the 1080p picture of today (1600 x 1080)
X0, Y0 = 96, {0: 16, 1: 31}
WIN = {0: (400, 270), 1: (400, 240)}
PIC_1080 = (1600, 1080)
TEXT = (320, 200, 40, 35)                       # the C64 text area (pixels) and its place in the 50 Hz window (x 40, y 35)
RING = 64                                       # vic_scaler.v: 64 VIC lines of ring
MARGIN = 3                                      # lines: the read follows the write by at least this much, and leaves the slot this much early
SHIPPED = {2: dict(cdce=(0x03, 0x70, 0x33, 0xED), align=20, rate={0: 148869143, 3: 149138494}),   # cdce_i2c.v, vic_scaler.v ALIGN,
           8: dict(cdce=(0x3D, 0xE0, 0xF3, 0xCD), align=35, rate={1: 148062470, 2: 148240988})}   # top.v hdmi_rate before U16

def regs(N, P, Q, R, fvco):
    rng = 0 if fvco < 125e6 else 1 if fvco < 150e6 else 2 if fvco < 175e6 else 3
    return (N >> 4, ((N & 15) << 4) | (R >> 5), ((R & 31) << 3) | (Q >> 3), ((Q & 7) << 5) | (P << 2) | rng)

def decode_regs(b):
    """Registers 0x18..0x1B back to (N, R, Q, P, range) - the inverse of regs(), read from the bytes only."""
    N = (b[0] << 4) | (b[1] >> 4)
    R = ((b[1] & 15) << 5) | (b[2] >> 3)
    Q = ((b[2] & 7) << 3) | (b[3] >> 5)
    return N, R, Q, (b[3] >> 2) & 7, b[3] & 3

def chip_check(b, n, ref):
    """What the CDCEL824 does with the four bytes, decoded from the bytes alone, for one reference: M = (N x 2^P - R) / Q an integer,
    N >= M, P = 4 - int(log2 N/M), 16 <= Q <= 63, 0 <= R <= 511, fVCO in 80..200 MHz, the range bits as the data sheet's thresholds.
    Returns the failures and (N, M, fvco)."""
    bad = []
    N, R, Q, P, rng = decode_regs(b)
    num = N * (1 << P) - R
    if Q == 0 or num % Q: return ["%s: bytes %s give no integer M (N %d P %d Q %d R %d)" % (n, b, N, P, Q, R)], None
    M = num // Q
    if M < 1 or M > 511: return ["%s: M %d out of 1..511" % (n, M)], None
    fvco = ref * N / M
    want_rng = 0 if fvco < 125e6 else 1 if fvco < 150e6 else 2 if fvco < 175e6 else 3
    if N < M: bad.append("%s: N %d < M %d" % (n, N, M))
    if P != max(0, 4 - int(math.floor(math.log2(N / M)))): bad.append("%s: P %d is not 4 - int(log2(N/M))" % (n, P))
    if not 16 <= Q <= 63: bad.append("%s: Q %d out of 16..63" % (n, Q))
    if R > 511: bad.append("%s: R %d" % (n, R))
    if not CDCE_VCO[0] <= fvco <= CDCE_VCO[1]: bad.append("%s: CDCE VCO %.3f MHz out of 80..200" % (n, float(fvco) / 1e6))
    if rng != want_rng: bad.append("%s: VCO range bits %d but fVCO %.3f MHz needs %d" % (n, rng, float(fvco) / 1e6, want_rng))
    return bad, (N, M, fvco)

def cdce_exact(ratio, ref):
    """CDCE setting for exactly N/M = ratio (scaled by k = 1, 2, ... while N <= 4095 and M <= 511): the first the register rules accept."""
    for k in range(1, 512):
        N, M = ratio.numerator * k, ratio.denominator * k
        if N > 4095 or M > 511: break
        fvco = ref * N / M
        if not CDCE_VCO[0] <= fvco <= CDCE_VCO[1]: continue
        P = max(0, min(4, 4 - int(math.floor(math.log2(float(fvco / ref))))))
        Q, R = divmod(N * (1 << P), M)
        if 16 <= Q <= 63 and 0 <= R <= 51:
            return dict(N=N, M=M, P=P, Q=Q, R=R, fvco=fvco, bytes=regs(N, P, Q, R, fvco))
    return None

def lock_for(fam, gear, htot, vtot):
    """The CDCE setting that makes the row's frame exactly the VIC frame, or None. Frame in TMDS clocks C = g x htot x vtot; the VIC frame is
    V dot4x clocks; f_t = dot4x x 125/104 x N/M, so C / f_t = V / dot4x needs N/M = C / V x 104/125, exactly (Fractions)."""
    return cdce_exact(F(gear * htot * vtot, VIC_FRAME[fam]) / REF_PER_DOT4X, DOT4X[VM_PRIMARY[fam]] * REF_PER_DOT4X)

SCALES = ("Integer", "Fit", "Fill")           # U16 D3: the Scale setting (0x4F01), power-up Fit

def steps(fam, ha, va, sc):
    """U16 D3: the picture's steps per output pixel / line (VIC pixels h, VIC lines v) and the text layer's vertical step tv for scale sc.
    Fit: the largest factor s with today's 1600 x 1080 proportion (4 s per VIC pixel, vertically 4 s at 50 Hz and 4.5 s at 60 Hz).
    Integer: the largest whole k, the same on both axes, whose 400 x 270 (240) window fits; where that is 1 (576p, 480p: the 64-line
    ring cannot hold a window read at 1 line per VIC line, U16 D3, Alex 2026-11-14) Integer is Fit. Fill: hact / 400 and vact / 270 (240).
    The text area (U16 D6) follows the picture horizontally and, vertically, the factor the 50 Hz window (270 lines) would have at this
    scale, in both families (1080p60 Fit: 4 x 4 as before U16, not 4.5)."""
    wpx, wln = WIN[fam]
    k = min(ha // wpx, va // wln)
    if sc == 1 or (sc == 0 and k < 2):
        s_ = min(F(ha, PIC_1080[0]), F(va, PIC_1080[1]))
        h, v, tv = 1 / (4 * s_), 1 / ((F(9, 2) if fam else 4) * s_), 1 / (4 * s_)
    elif sc == 0:
        h = v = tv = F(1, k)
    else:
        h, v, tv = F(wpx, ha), F(wln, va), F(WIN[0][1], va)
    return h, v, tv

def geom(fam, ha, va, htot, vtot, sc, align):
    """The window (first pixel / line relative to the active origin, size), the text area (centred), and the margins in VIC lines at
    the frame align `align` (the VIC line at which the active origin starts): how long after the VIC wrote it every window pixel is read
    first (m_read), and how long before the VIC writes its ring slot again it is read last (m_ring)."""
    h, v, tv = steps(fam, ha, va, sc)
    for f in (h, v, tv):
        if f.denominator > 63 or f.numerator > 63 or f > 1: raise SystemExit("row %dx%d scale %s: step %s does not fit 6 bits / is > 1" % (ha, va, SCALES[sc], f))
    wpx, wln = WIN[fam]
    ww, wh = math.ceil(wpx / h), math.ceil(wln / v)      # output pixel x shows VIC pixel floor(x h): the window ends where that reaches wpx
    xw0, yw0 = (ha - ww) // 2, (va - wh) // 2
    tw, th = math.ceil(TEXT[0] / h), math.ceil(TEXT[1] / tv)
    xt0, yt0 = (ha - tw) // 2, (va - th) // 2
    Lc = VIC_LINE[fam]
    Lo = F(VIC_LINES[fam] * Lc, vtot)                    # one output line in dot4x clocks (the frame is the VIC frame)
    mf = mo = None
    for k in range(wln):
        L = Y0[fam] + k
        y1, y2 = yw0 + math.ceil(k / v), yw0 + math.ceil((k + 1) / v) - 1
        for c in (0, wpx - 1):
            x1, x2 = xw0 + math.ceil(c / h), xw0 + math.ceil((c + 1) / h) - 1
            rf = align * Lc + y1 * Lo + x1 * Lo / htot           # first read of (k, c)
            rl = align * Lc + y2 * Lo + (x2 + 1) * Lo / htot     # end of its last read
            a_ = (rf - (L * Lc + (X0 + c + 1) * 4)) / Lc          # after the VIC wrote it
            b_ = (((L + RING) * Lc + (X0 + c) * 4) - rl) / Lc     # before the VIC writes that ring slot again
            mf = a_ if mf is None else min(mf, a_)
            mo = b_ if mo is None else min(mo, b_)
    return dict(hn=h.numerator, hd=h.denominator, vn=v.numerator, vd=v.denominator, tvn=tv.numerator, tvd=tv.denominator,
                ww=ww, wh=wh, xw0=xw0, yw0=yw0, tw=tw, th=th, xt0=xt0, yt0=yt0, m_read=mf, m_ring=mo)

def align_for(fam, ha, va, htot, vtot, sc):
    """U16 D3: one align line per row and scale (a scale change re-aligns the output frame once): the smallest at which every window
    pixel is read at least MARGIN lines after the VIC wrote it."""
    for A in range(0, 2 * VIC_LINES[fam]):
        if geom(fam, ha, va, htot, vtot, sc, A)["m_read"] >= MARGIN: return A
    raise SystemExit("row %dx%d scale %s: no align line" % (ha, va, SCALES[sc]))

def compute():
    rows = []
    for m in sorted(ROWS):
        fam, hd = divmod(m, 6)
        vic, pclk, ha, hfp, hs, hbp, hpol, va, vfp, vs, vbp, vpol, gear, asp = MODES[m]
        htot, vtot = ha + hfp + hs + hbp, va + vfp + vs + vbp
        lk = lock_for(fam, gear, htot, vtot)
        rate = {vm: (None if lk is None else int(round(float(DOT4X[vm] * REF_PER_DOT4X * lk["N"] / lk["M"] / gear))))
                for vm in DOT4X if VM_FAM[vm] == fam}
        rows.append(dict(idx=m, fam=fam, name=HDMI_NAMES[hd], vic=vic, asp=asp, ha=ha, hfp=hfp, hs=hs, hbp=hbp, hpol=hpol, va=va,
                         vfp=vfp, vs=vs, vbp=vbp, vpol=vpol, gear=gear, pclk=pclk, htot=htot, vtot=vtot, lk=lk, rate=rate,
                         align=[align_for(fam, ha, va, htot, vtot, sc) for sc in range(3)]))
        r = rows[-1]
        r["align_hw"] = [a % VIC_LINES[fam] for a in r["align"]]
        r["gs"] = [geom(fam, ha, va, htot, vtot, sc, r["align"][sc]) for sc in range(3)]
    return rows

def nm(r): return "%s %d Hz" % (r["name"], 50 if r["fam"] == 0 else 60)

def limits(rows):
    bad = []
    if {r["idx"] for r in rows} != ROWS: bad.append("rows %s are not the 7 of U16 D2" % sorted(r["idx"] for r in rows))
    for r in rows:
        n = nm(r)
        if r["lk"] is None: bad.append("%s: no CDCE setting makes the frame exactly the VIC frame (cannot lock)" % n); continue
        C = r["gear"] * r["htot"] * r["vtot"]
        if F(r["lk"]["N"], r["lk"]["M"]) * REF_PER_DOT4X * VIC_FRAME[r["fam"]] != C:
            bad.append("%s: N/M %d/%d does not give the %d TMDS clocks of the frame per VIC frame" % (n, r["lk"]["N"], r["lk"]["M"], C))
        for vm, rate in r["rate"].items():
            ft = float(DOT4X[vm] * REF_PER_DOT4X * r["lk"]["N"] / r["lk"]["M"])
            if not GTP_VCO[0] <= GTP_NMUL * ft <= GTP_VCO[1]: bad.append("%s vmode %d: GTP PLL VCO %.3f GHz out of 1.6..3.3" % (n, vm, GTP_NMUL * ft / 1e9))
            if 10 * ft < GTP_MIN_LINE: bad.append("%s vmode %d: GTP line rate below 500 Mb/s" % (n, vm))
            if not MMCM_VCO[0] <= MMCM_M * ft / 2 <= MMCM_VCO[1]: bad.append("%s vmode %d: mmcm_pix VCO %.0f MHz out of 600..1200" % (n, vm, MMCM_M * ft / 2e6))
            if rate < HDMI_MIN_PIX: bad.append("%s vmode %d: pixel clock %.3f MHz below the HDMI minimum 25 MHz" % (n, vm, rate / 1e6))
            if abs(rate - r["pclk"]) / r["pclk"] > 0.01: bad.append("%s vmode %d: pixel clock %.3f MHz more than 1 %% from the mode's" % (n, vm, rate / 1e6))
        if r["hfp"] + r["hs"] + r["hbp"] < 120: bad.append("%s: horizontal blanking too short for the data island and the layer triggers" % n)
        if r["vfp"] + r["vs"] + r["vbp"] < 4 or r["vfp"] < 1: bad.append("%s: vertical blanking" % n)
        if r["gear"] not in GEARS: bad.append("%s: gear %d not in %s" % (n, r["gear"], GEARS))
        if r["htot"] > 4095 or r["vtot"] > 2047: bad.append("%s: raster counters" % n)
        if r["vic"]:
            g = CEA.get(r["vic"])
            if g is None: bad.append("%s: VIC %d is not in the CEA table of this tool" % (n, r["vic"]))
            else:
                if (r["ha"], r["va"], r["htot"], r["vtot"] - CEA_EXTRA_LINES.get(r["idx"], 0)) != g[:4]:
                    bad.append("%s: raster %dx%d (total %dx%d) is not CEA VIC %d" % (n, r["ha"], r["va"], r["htot"], r["vtot"], r["vic"]))
                if r["asp"] != g[4]: bad.append("%s: AVI aspect %d, VIC %d wants %d" % (n, r["asp"], r["vic"], g[4]))
        elif r["asp"] != 0: bad.append("%s: a PC row (VIC 0) carries no aspect" % n)
        for sc, g in enumerate(r["gs"]):
            ns = "%s %s" % (n, SCALES[sc])
            if g["xw0"] < 0 or g["yw0"] < 0 or g["xw0"] + g["ww"] > r["ha"] or g["yw0"] + g["wh"] > r["va"]:
                bad.append("%s: the window %dx%d at (%d, %d) is not inside the active area" % (ns, g["ww"], g["wh"], g["xw0"], g["yw0"]))
            if g["xt0"] < 0 or g["yt0"] < 0: bad.append("%s: the text area is larger than the active area" % ns)
            if g["m_read"] < MARGIN: bad.append("%s: a window pixel is read %.2f lines after the VIC wrote it (< %d)" % (ns, float(g["m_read"]), MARGIN))
            if g["m_ring"] < MARGIN: bad.append("%s: the ring is overtaken: the last read of a line is %.2f lines before the VIC writes its slot again" % (ns, float(g["m_ring"])))
        g = r["gs"][1]
        sh = SHIPPED.get(r["idx"])
        if sh:
            if tuple(r["lk"]["bytes"]) != sh["cdce"]: bad.append("%s: CDCE bytes %s, the design shipped %s" % (n, r["lk"]["bytes"], sh["cdce"]))
            if r["align"][1] != sh["align"]: bad.append("%s: align line %d at Fit, vic_scaler.v had %d" % (n, r["align"][1], sh["align"]))
            for vm, want in sh["rate"].items():
                if r["rate"][vm] != want: bad.append("%s vmode %d: rate %d, top.v had %d" % (n, vm, r["rate"][vm], want))
            if (g["ww"], g["wh"], g["xw0"], g["yw0"], g["xt0"], g["yt0"], g["tw"], g["th"]) != (1600, 1080, 160, 0, 320, 140, 1280, 800):
                bad.append("%s: the 1080p window or text area moved (Fit)" % n)
    return bad

def chip_limits(rows):
    bad = []
    for r in rows:
        if r["lk"] is None: continue
        for vm in r["rate"]:
            bad += chip_check(r["lk"]["bytes"], "%s vmode %d" % (nm(r), vm), DOT4X[vm] * REF_PER_DOT4X)[0]
    return bad

def vh(rows):
    L = ["// GENERATED by tools/hdmi_modes.py gen - do not edit. U16: the HDMI modes of c64_basic, every one locked to the VIC frame.",
         "// mode index m = family * 6 + hdmi: family 0 = 50 Hz, 1 = 60 Hz; hdmi 0 576p/480p, 1 720p, 2 1080p, 3 800x600 (4 1024x768 and 5 1280x1024 do not lock)",
         "// hm_valid / hm_locked: the 7 rows of U16 D2 (rows 3, 4, 5, 10, 11 do not exist: every function answers 0 for them)",
         "// hm_row: {vic[6:0], gear[3:0], hact[11:0], hfp[11:0], hsync[11:0], hbp[11:0], hpol, vact[10:0], vfp[10:0], vsync[10:0], vbp[10:0], vpol}",
         "// hm_aspect: AVI picture aspect M1:M0 (0 none, 1 4:3, 2 16:9).",
         "// hm_cdce_lock: CDCE registers 0x18..0x1B (N[11:4]; N[3:0],R[8:5]; R[4:0],Q[5:3]; Q[2:0],P,range): the frame is exactly the VIC frame.",
         "// hm_rate: the pixel clock in Hz (TMDS clock / gear, for N/CTS) in the family's first System Mode (PAL, NTSC); hm_rate_alt in its",
         "// second (NTSC-50, PAL-60).",
         "// hm_align(m, sc): the VIC line at which vic_scaler aligns the frame (one per row and scale: a scale change re-aligns it once).",
         "// hm_geom(m, sc), sc = scale 0 Integer, 1 Fit, 2 Fill (U16 D3): {hn[5:0], hd[5:0], vn[5:0], vd[5:0], tvn[5:0], tvd[5:0], xw0[11:0],",
         "// yw0[10:0], xt0[11:0], yt0[10:0]}: vic_scaler steps hn/hd VIC pixels per output pixel and vn/vd VIC lines per output line from the",
         "// window's first pixel / line (xw0, yw0, relative to the active origin); c64_text's text area from (xt0, yt0), hn/hd C64 pixels per",
         "// output pixel and tvn/tvd C64 lines per output line.", ""]
    def block(head, rows_, line):
        L.extend([head, "    case (m)"] + [line(r) for r in rows_])
        L.extend(["    default: %s = 0;" % head.split("(")[0].split()[-1], "    endcase", "endfunction", ""])
    c = lambda r: "  // " + nm(r)
    block("function hm_valid(input [3:0] m);", rows, lambda r: "    4'd%d: hm_valid = 1'b1;%s" % (r["idx"], c(r)))
    block("function [104:0] hm_row(input [3:0] m);", rows,
          lambda r: "    4'd%d: hm_row = {7'd%d, 4'd%d, 12'd%d, 12'd%d, 12'd%d, 12'd%d, 1'b%d, 11'd%d, 11'd%d, 11'd%d, 11'd%d, 1'b%d};%s" %
                    (r["idx"], r["vic"], r["gear"], r["ha"], r["hfp"], r["hs"], r["hbp"], r["hpol"], r["va"], r["vfp"], r["vs"], r["vbp"], r["vpol"], c(r)))
    block("function [1:0] hm_aspect(input [3:0] m);", rows, lambda r: "    4'd%d: hm_aspect = 2'd%d;%s" % (r["idx"], r["asp"], c(r)))
    block("function hm_locked(input [3:0] m);", rows, lambda r: "    4'd%d: hm_locked = 1'b1;%s" % (r["idx"], c(r)))
    block("function [31:0] hm_cdce_lock(input [3:0] m);", rows,
          lambda r: "    4'd%d: hm_cdce_lock = 32'h%02X%02X%02X%02X;  // N %d M %d P %d Q %d R %d: %d TMDS clocks per VIC frame exactly" %
                    ((r["idx"],) + tuple(r["lk"]["bytes"]) + (r["lk"]["N"], r["lk"]["M"], r["lk"]["P"], r["lk"]["Q"], r["lk"]["R"], r["gear"] * r["htot"] * r["vtot"])))
    block("function [31:0] hm_rate(input [3:0] m);", rows, lambda r: "    4'd%d: hm_rate = 32'd%d;%s" % (r["idx"], r["rate"][VM_PRIMARY[r["fam"]]], c(r)))
    block("function [31:0] hm_rate_alt(input [3:0] m);", rows, lambda r: "    4'd%d: hm_rate_alt = 32'd%d;%s" % (r["idx"], r["rate"][VM_ALT[r["fam"]]], c(r)))
    L.extend(["function [8:0] hm_align(input [3:0] m, input [1:0] sc);", "    case ({sc, m})"])
    for sc in range(3):
        for r in rows:
            L.append("    {2'd%d, 4'd%d}: hm_align = 9'd%d;  // %s %s" % (sc, r["idx"], r["align_hw"][sc], nm(r), SCALES[sc]))
    L.extend(["    default: hm_align = 0;", "    endcase", "endfunction", ""])
    L.extend(["function [81:0] hm_geom(input [3:0] m, input [1:0] sc);", "    case ({sc, m})"])
    for sc in range(3):
        for r in rows:
            g = r["gs"][sc]
            L.append("    {2'd%d, 4'd%d}: hm_geom = {6'd%d, 6'd%d, 6'd%d, 6'd%d, 6'd%d, 6'd%d, 12'd%d, 11'd%d, 12'd%d, 11'd%d};  // %s %s: window %dx%d, text %dx%d, margins %.1f / %.1f lines" %
                     (sc, r["idx"], g["hn"], g["hd"], g["vn"], g["vd"], g["tvn"], g["tvd"], g["xw0"], g["yw0"], g["xt0"], g["yt0"], nm(r), SCALES[sc],
                      g["ww"], g["wh"], g["tw"], g["th"], float(g["m_read"]), float(g["m_ring"])))
    L.extend(["    default: hm_geom = 0;", "    endcase", "endfunction", ""])
    return "\n".join(L)

def table(rows):
    print("| # | mode | VIC | g | timing | lock N/M | TMDS MHz (first System Mode) | pixel rates Hz | align | scale | window at | steps h, v (text v) | text at | margins |")
    print("|---|---|---|---|---|---|---|---|---|---|---|---|---|---|")
    for r in rows:
        lk = r["lk"]
        for sc, g in enumerate(r["gs"]):
            head = ("%d | %s | %d | %d | %dx%d, %dx%d | %d/%d | %.6f | %s" %
                    (r["idx"], nm(r), r["vic"], r["gear"], r["ha"], r["va"], r["htot"], r["vtot"], lk["N"], lk["M"],
                     float(DOT4X[VM_PRIMARY[r["fam"]]] * REF_PER_DOT4X * lk["N"] / lk["M"]) / 1e6,
                     " / ".join(str(v) for _, v in sorted(r["rate"].items())))) if sc == 0 else " | | | | | | | "
            print("| %s | %d | %s | %dx%d at (%d, %d) | %d/%d, %d/%d (%d/%d) | %dx%d at (%d, %d) | %.1f / %.1f |" %
                  (head, r["align_hw"][sc], SCALES[sc], g["ww"], g["wh"], g["xw0"], g["yw0"], g["hn"], g["hd"], g["vn"], g["vd"], g["tvn"], g["tvd"],
                   g["tw"], g["th"], g["xt0"], g["yt0"], float(g["m_read"]), float(g["m_ring"])))

def decode(text):
    """What the gateware reads from hdmi_modes.vh, decoded from the file alone: per row the lock bytes -> N/M (for both System Modes of
    the family, the chip rules) -> exactly the VIC frame for gear x htot x vtot; the rates are the pixel clocks of those bytes."""
    bad = []
    def fn(name): return {int(m): v for m, v in re.findall(r"4'd(\d+): " + name + r" = ([^;]*);", text)}
    rowf, lck, rate, alt, lkd = fn("hm_row"), fn("hm_cdce_lock"), fn("hm_rate"), fn("hm_rate_alt"), fn("hm_locked")
    aln = {(int(a), int(m)) for a, m in re.findall(r"\{2'd(\d), 4'd(\d+)\}: hm_align = ", text)}
    if any(set(d) != ROWS for d in (rowf, lck, rate, alt, lkd, fn("hm_valid"), fn("hm_aspect"))) or aln != {(sc, m) for sc in range(3) for m in ROWS}:
        return ["hdmi_modes.vh: the functions do not name exactly the rows of U16 D2"]
    for m in sorted(ROWS):
        n = "mode %d" % m
        f = [int(x) for x in re.findall(r"\d+'[db](\d+)", rowf[m])]
        gear, htot, vtot, fam = f[1], f[2] + f[3] + f[4] + f[5], f[7] + f[8] + f[9] + f[10], m // 6
        if "1'b1" not in lkd[m]: bad.append("%s: not locked" % n)
        b = int(lck[m].split("'h")[1], 16)
        b = (b >> 24 & 255, b >> 16 & 255, b >> 8 & 255, b & 255)
        for vm in (VM_PRIMARY[fam], VM_ALT[fam]):
            fails, got = chip_check(b, "%s vmode %d" % (n, vm), DOT4X[vm] * REF_PER_DOT4X)
            bad += fails
            if got is None: continue
            if F(got[0], got[1]) * REF_PER_DOT4X * VIC_FRAME[fam] != gear * htot * vtot:
                bad.append("%s: hm_cdce_lock N/M %d/%d does not give exactly the VIC frame (%d TMDS clocks)" % (n, got[0], got[1], gear * htot * vtot))
            want = int(round(float(got[2] / gear)))
            have = int(re.findall(r"\d+", (rate if vm == VM_PRIMARY[fam] else alt)[m].split("'", 1)[1])[0])
            if have != want: bad.append("%s vmode %d: rate %d is not the pixel clock %d of the lock bytes" % (n, vm, have, want))
    return bad

def known_blobs():
    """The control for regs(): the two blobs cdce_i2c.v shipped, 55/14 and 990/263, reproduced by the same rules."""
    out = []
    for N, M, vm, want in ((55, 14, 0, (0x03, 0x70, 0x33, 0xED)), (990, 263, 1, (0x3D, 0xE0, 0xF3, 0xCD))):
        ref = DOT4X[vm] * REF_PER_DOT4X
        fvco = ref * N / M
        P = 4 - int(math.floor(math.log2(float(fvco / ref))))
        Q, R = divmod(N * (1 << P), M)
        out.append((N, M, regs(N, P, Q, R, fvco), want))
    return out

def check():
    rows = compute()
    bad = limits(rows) + chip_limits(rows)
    for N, M, got, want in known_blobs():
        if tuple(got) != want: bad.append("control: %d/%d gives %s, cdce_i2c.v shipped %s" % (N, M, got, want))
    try: cur = open(VH).read()
    except OSError: cur = ""
    bad += decode(cur)
    if cur != vh(rows): bad.append("designs/c64_basic/hdmi_modes.vh is not what `gen` writes")
    for b in bad: print("FAIL:", b)
    if not bad:
        print("PASS hdmi_modes: 7 rows locked exactly to the VIC frame (both System Modes of each family), inside every limit "
              "(GTP, mmcm_pix, CDCE rules decoded from the file, CEA raster, gear), window inside the active area, ring margins >= %d lines, "
              "1080p rows as shipped, hdmi_modes.vh current" % MARGIN)
    return 1 if bad else 0

def selftest():
    """One mutation per rule; each must be caught by the checks `check` runs."""
    global MODES, ROWS, RING, VIC_FRAME
    caught = tests = 0
    def run(name, mutate, restore):
        nonlocal caught, tests
        tests += 1
        mutate()
        try: bad = limits(compute())
        except SystemExit as e: bad = [str(e)]
        finally: restore()
        ok = bool(bad)
        print("  mutation %-52s %s" % (name, "caught" if ok else "NOT CAUGHT")); caught += ok
    def setmode(m, **kw):
        old = MODES[m]
        def mut():
            v = dict(zip(NAMES, MODES[m])); v.update(kw); MODES[m] = tuple(v[x] for x in NAMES)
        def res(): MODES[m] = old
        return mut, res
    run("480p with CEA's 525 lines (cannot lock)", *setmode(6, vfp=10))
    run("1080p50 with a 1024 x 768 PC raster", *setmode(2, ha=1024, hfp=24, hs=136, hbp=160, va=768, vfp=3, vs=6, vbp=29))
    run("720p60 at gear 4", *setmode(7, gear=4))
    run("1080p60 at gear 2 (TMDS 296 MHz)", *setmode(8, gear=2))
    run("576p with aspect 16:9", *setmode(0, asp=2))
    run("800x600 with an aspect", *setmode(9, asp=1))
    run("720p60 with the 50 Hz VIC 19", *setmode(7, vic=19))
    run("horizontal blanking 100", *setmode(1, hfp=40, hs=40, hbp=20))
    def ring_small(): globals().update(RING=20)
    def ring_back(): globals().update(RING=64)
    run("ring of 20 lines (overtaken)", ring_small, ring_back)
    def rows_add(): ROWS.add(10)
    def rows_back(): ROWS.discard(10)
    run("1024 x 768 added (cannot lock)", rows_add, rows_back)
    old = dict(VIC_FRAME)
    def vf(): VIC_FRAME[0] = 63 * 313 * 32
    def vf_back(): VIC_FRAME.update(old)
    run("VIC frame one line longer", vf, vf_back)
    old_s = dict(SHIPPED[2])
    def sh(): SHIPPED[2] = dict(old_s, align=21)
    def sh_back(): SHIPPED[2] = old_s
    run("1080p50 align other than the shipped 20", sh, sh_back)
    txt = vh(compute())
    def vcase(name, new):
        nonlocal caught, tests
        tests += 1
        ok = bool(decode(new)) and not decode(txt)
        print("  mutation %-52s %s" % (name, "caught" if ok else "NOT CAUGHT")); caught += ok
    vcase("lock bytes of 1080p50 with N + 16", txt.replace("4'd2: hm_cdce_lock = 32'h037033ED", "4'd2: hm_cdce_lock = 32'h047033ED"))
    vcase("1080p60 lock bytes in the 720p60 slot (gear 2 frame)", re.sub(r"4'd7: hm_cdce_lock = 32'h[0-9A-F]{8}", "4'd7: hm_cdce_lock = 32'h3DE0F3CE", txt))
    vcase("hm_rate of 720p50 = the TMDS clock", re.sub(r"4'd1: hm_rate = 32'd\d+", "4'd1: hm_rate = 32'd148869143", txt))
    vcase("hm_locked 0 on 576p", txt.replace("4'd0: hm_locked = 1'b1", "4'd0: hm_locked = 1'b0"))
    tests += 1
    good = regs
    globals()["regs"] = lambda N, P, Q, R, fvco: good(N, P, R, Q, fvco)
    try: ok = any(got != want for _, _, got, want in known_blobs())
    finally: globals()["regs"] = good
    print("  mutation %-52s %s" % ("regs() with Q and R swapped", "caught" if ok else "NOT CAUGHT")); caught += ok
    print("selftest: %d of %d mutations caught" % (caught, tests))
    return 0 if caught == tests else 1

if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else "table"
    if cmd == "table": table(compute())
    elif cmd == "gen":
        open(VH, "w").write(vh(compute())); print("wrote", os.path.relpath(VH, ROOT))
    elif cmd == "check": sys.exit(check())
    elif cmd == "selftest": sys.exit(selftest())
    else: sys.exit(__doc__)
