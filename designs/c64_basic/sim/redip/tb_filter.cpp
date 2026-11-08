// SPDX-License-Identifier: GPL-3.0-or-later
//
// T25: how much the reDIP filter loses to its own word length.
//
// Lemon64 2026-11-06 (Bansai/Tony): "you want to ask the LLM to make the reDIP-SID filter math wider
// as it can suffer bad filter quantization errors due high resonance, low cutoff, etc. 16b of filter
// accumulator state is just not enough."  This bench measures that claim instead of believing it, and
// it answers the only open number: how many fractional bits the state needs.
//
// The filter is three integrators in a ring (sid_filter.sv:311-313):
//
//     vlp = vlp - w0*vbp        hardware: dv = (-w0_T_lsl17 * vbp) >>> 17, vlp += dv
//     vbp = vbp - w0*vhp        the same with vhp
//     vhp = 1/Q*vbp - vlp - vi  hardware: ((1/Q << 10)*vbp - (vlp << 10) - (vi << 3)) >> 10
//
// w0_T_lsl17 is w0*T in Q0.17 and the state is Q0, so `>>> 17` floors every integrator step onto the
// 16 bit grid: at a low cutoff w0 is small, and once |w0*v| < 1 the step is 0 and the integrator
// STOPS instead of creeping. High resonance feeds that error back through 1/Q*vbp.
//
// What the bench does, per operating point:
//   1. runs the real RTL (sid_filter, NS = 1) and an integer model of it in lockstep. The model must
//      be bit exact; that is the self check that the measurement below means anything at all.
//   2. runs the same filter in double precision. The difference RTL - double is the quantisation
//      error, reported in LSB of the 16 bit mixer input and in dBFS.
//   3. runs the integer model again with F extra fractional bits in the state (F = 2, 4, 6, 8) -
//      the proposed fix, costed in the reply, measured here without touching one line of RTL.
//
// Usage: tb_filter [passes per point] [--rtlf F] [--csv]
//   --rtlf is the FILTER_FRAC the RTL was built with; the model at that setting must match it.
//   One pass is one phi2 cycle. 20000 passes = 20 ms of audio per point.
//
#include "Vsid_filter.h"
#include "Vsid_filter___024root.h"
#include "verilated.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// ---------------------------------------------------------------------------
//  The hardware's arithmetic, as arithmetic
// ---------------------------------------------------------------------------

// sid_filter.sv:24-31 - clamp an s17 to 16 bits.
static inline int clamp16(int64_t x17)
{
    const unsigned u = (unsigned)(x17 & 0x1FFFF);
    switch ((u >> 15) & 3) {
    case 2: return -32768;
    case 1: return 32767;
    default: return (int16_t)(u & 0xFFFF);
    }
}

// Arithmetic right shift that floors, as >>> does on a signed value.  (declared below as well)
static inline int64_t sext_fwd(int64_t x, int n);

// clamp_v() of the RTL: the value wraps into DW = 17 + f bits first, then the top two bits decide,
// exactly as reDIP's own clamp does at f = 0. Not a magnitude saturation - the wrap is part of it.
static inline int64_t clamp_vf(int64_t x, int f)
{
    const int dw = 17 + f, vw = 16 + f;
    const uint64_t u = (uint64_t)x & ((((uint64_t)1) << dw) - 1);
    switch ((unsigned)((u >> (dw - 2)) & 3)) {
    case 2: return -(((int64_t)1) << (vw - 1));
    case 1: return (((int64_t)1) << (vw - 1)) - 1;
    default: return sext_fwd((int64_t)u, vw);
    }
}

// Arithmetic right shift that floors, as >>> does on a signed value.
static inline int64_t asr(int64_t x, int n) { return x >> n; }

// Sign-extend the low n bits of x.
static inline int64_t sext(int64_t x, int n)
{
    const int64_t m = (int64_t)1 << (n - 1);
    return ((x & (((int64_t)1 << n) - 1)) ^ m) - m;
}

static inline int64_t sext_fwd(int64_t x, int n) { return sext(x, n); }

static const int64_t MIXER_DC_6581 = -((int64_t)(1 << 20) / 18); // sid_filter.sv:65
static const int64_t MIXER_DC_8580 = 0;

// One phi2 pass of the filter, integer, F fractional bits in the state (F = 0 is the hardware).
// vlp/vbp/vhp are scaled by 2^F. Returns audio_o (20 bits, unmasked here).
struct IntFilter {
    int64_t vlp = 0, vbp = 0, vhp = 0;
    long stalls = 0;    // steps the grid swallowed: the true step was not zero, dv was
    long steps = 0;

    // `rnd` rounds every shift to nearest instead of flooring. A floored step in a feedback
    // integrator is a systematic negative bias, and widening the state makes that bias fire MORE
    // often (the hardware's stalled step is at least an honest zero), so extra bits alone can make
    // the error worse. F = 0 always runs the hardware's own arithmetic, floored.
    int64_t pass(int f, int w0, int invQ, int64_t vi, int64_t vd, int vol, int mode, bool is6581,
                 double w0f, double vbp_true, double vhp_true, bool rnd)
    {
        // The hardware itself: F = 0 and floored. Everything else is a candidate fix, and F = 0
        // with rounding is the cheapest of them - an add, no wider state, no wider multiplier.
        const bool hw = (f == 0 && !rnd);
        const int64_t h17 = rnd ? ((int64_t)1 << 16) : 0;
        const int64_t h10 = rnd ? ((int64_t)1 << 9) : 0;

        // --- vlp = vlp - w0*vbp
        const int64_t o1 = -(int64_t)w0 * vbp;
        const int64_t dv1 = hw ? sext(asr(o1, 17), 17) : asr(o1 + h17, 17);
        const int64_t vlp_n = hw ? clamp16(vlp + dv1) : clamp_vf(vlp + dv1, f);
        // Did the grid swallow a step that physically happened?
        steps++;
        if (dv1 == 0 && std::fabs(w0f * vbp_true) >= 0.5) stalls++;

        // --- vbp = vbp - w0*vhp
        const int64_t o2 = -(int64_t)w0 * vhp;
        const int64_t dv2 = hw ? sext(asr(o2, 17), 17) : asr(o2 + h17, 17);
        const int64_t vbp_n = hw ? clamp16(vbp + dv2) : clamp_vf(vbp + dv2, f);
        steps++;
        if (dv2 == 0 && std::fabs(w0f * vhp_true) >= 0.5) stalls++;

        // --- vhp = 1/Q*vbp - vlp - vi/128   (sid_filter.sv:456-462)
        const int64_t c = -(((vlp_n << 7) + (vi << f)) << 3);
        const int64_t o3 = c + (int64_t)invQ * vbp_n;
        const int64_t vhp_n = hw ? clamp16(sext(asr(o3, 10), 17)) : clamp_vf(asr(o3 + h10, 10), f);

        vlp = vlp_n;
        vbp = vbp_n;
        vhp = vhp_n;

        // --- the mixers (sid_filter.sv:377-396, 465-472)
        int64_t vf = 0;
        if (mode & 1) vf += vlp_n;
        if (mode & 2) vf += vbp_n;
        if (mode & 4) vf += vhp_n;
        // The hardware sums the selected outputs in DW = 17 + f bits, wrapping at every add. With
        // one output selected (this bench uses LP) a single sign-extension at the end is the same
        // thing; for two or three outputs the intermediate wrap would have to be modelled per add.
        vf = sext(vf, 17 + f);

        const int64_t vd1 = sext(vd + (is6581 ? MIXER_DC_6581 : MIXER_DC_8580), 24);
        int64_t amix;
        if (hw) {
            amix = clamp16(sext(asr(vd1, 7), 17) + vf);
        } else {
            // The direct path is in whole LSB, the filter outputs carry f fractional bits: scale
            // vd up, add on that grid, round back to whole LSB, then reDIP's own 17 -> 16 clamp.
            const int64_t w = sext((asr(vd1, 7) << f) + vf + (f ? ((int64_t)1 << (f - 1)) : 0), 17 + f);
            amix = clamp16(asr(w, f));
        }
        return (int64_t)vol * amix;
    }
};

// The same filter without a grid: the reference.
struct DblFilter {
    double vlp = 0, vbp = 0, vhp = 0;

    double pass(double w0f, double invQf, double vi, double vd, int vol, int mode, bool is6581)
    {
        vlp -= w0f * vbp;
        vbp -= w0f * vhp;
        vhp = invQf * vbp - vlp - vi / 128.0;

        double vf = 0;
        if (mode & 1) vf += vlp;
        if (mode & 2) vf += vbp;
        if (mode & 4) vf += vhp;

        const double vd1 = vd + (is6581 ? (double)MIXER_DC_6581 : 0.0);
        double amix = vd1 / 128.0 + vf;
        if (amix > 32767) amix = 32767;
        if (amix < -32768) amix = -32768;
        return (double)vol * amix;
    }
};

// ---------------------------------------------------------------------------
//  Driving sid_filter standalone (the schedule of sid_core.sv:80-101, NS = 1)
// ---------------------------------------------------------------------------

static const int VLAST = 17; // max(6*NS + 5, FLAST + 7) with FLAST = 5*NS + 5 = 10

struct Dut {
    Vsid_filter *d = new Vsid_filter;

    void settings(bool is6581, int res_hi, int fc_base, int fc_offset, int fc_sel, int bypass)
    {
        d->model = is6581 ? 0 : 1; // sid::MOS6581 = 0
        d->res_hi = res_hi;
        d->fc_base = fc_base;
        d->fc_offset = fc_offset;
        d->fc_sel = fc_sel;
        d->bypass = bypass;
        d->ext_i = 0;
    }

    // freg is a packed struct: fc_lo[31:24] fc_hi[23:16] res[15:12] filt[11:8] mode[7:4] vol[3:0]
    void regs(int fc, int res, int filt, int mode, int vol)
    {
        d->freg = ((uint32_t)(fc & 7) << 24) | ((uint32_t)((fc >> 3) & 0xFF) << 16) |
                  ((uint32_t)(res & 15) << 12) | ((uint32_t)(filt & 15) << 8) |
                  ((uint32_t)(mode & 15) << 4) | (uint32_t)(vol & 15);
    }

    // One phi2 pass. voices[0..2] are the voice DCA outputs (s22). Fills w0 / invQ with what the
    // RTL computed this pass, and returns audio_o.
    int64_t pass(const int64_t voices[3], int *w0, int *invQ)
    {
        int64_t audio = 0;
        for (int vc = 1; vc <= VLAST; vc++) {
            const int cyc = (vc >= 8 && vc <= 17) ? vc - 7 : 0;
            d->vcycle = vc;
            d->cycle = cyc;
            d->voice_i = (vc >= 8 && vc <= 10) ? (uint32_t)(voices[vc - 8] & 0x3FFFFF) : 0;
            d->clk = 0;
            d->eval(); // the combinational state of cycle `cyc`
            if (cyc == 4) {
                // what stage 4 latches into the multiplier
                *w0 = (int16_t)d->rootp->sid_filter__DOT__w0_T_lsl17;
                *invQ = (int)d->rootp->sid_filter__DOT___1_Q_lsl10;
            }
            if (cyc == 9) {
                audio = (int64_t)sext(d->audio_o, 20);
            }
            d->clk = 1;
            d->eval(); // the edge
        }
        return audio;
    }

    void reset()
    {
        for (int i = 0; i < 3; i++) d->rootp->sid_filter__DOT__v[i] = 0;
    }
};

// ---------------------------------------------------------------------------
//  One operating point
// ---------------------------------------------------------------------------

struct Result {
    int w0 = 0, invQ = 0;
    long rtl_vs_int = 0;      // must be 0: the model is the hardware
    double err_rms[2][5] = {};   // [0] floored, [1] rounded; F = 0, 2, 4, 6, 8, in LSB of the mixer input
    double err_max[2][5] = {};
    double stall_pct = 0;        // of the hardware (F = 0)
    double level_db = 0;      // RMS of the double reference, dBFS
};

static const int FBITS[5] = { 0, 2, 4, 6, 8 };

// FILTER_FRAC of the RTL under test, and the model variant that must reproduce it bit for bit.
static int g_rtlf = 0;
static int g_rtl_k = 0, g_rtl_q = 0;

static Result run_point(Dut &dut, bool is6581, int fc, int res, double freq, long passes, int mode)
{
    const int vol = 15;
    const int filt = 1; // voice 0 through the filter, voices 1 and 2 direct
    const double amp = 524288.0; // a quarter of the voice DCA range (s22)

    dut.settings(is6581, is6581 ? 0 : 1, 245, 0, 0, 0);
    dut.regs(fc, res, filt, mode, vol);
    dut.reset();

    IntFilter imod[2][5];
    DblFilter dref;

    Result r;
    double acc[2][5] = {}, amax[2][5] = {}, racc = 0;
    long n = 0;

    // Settle: the cutoff pipeline needs a few passes, and the integrators start at 0.
    const long warm = 64;

    for (long p = 0; p < passes + warm; p++) {
        const double ph = 2.0 * M_PI * freq * (double)p / 1000000.0; // phi2 = 1 MHz
        const int64_t s = (int64_t)std::llround(amp * std::sin(ph));
        const int64_t voices[3] = { s, 0, 0 };

        int w0 = 0, invQ = 0;
        const int64_t a_rtl = dut.pass(voices, &w0, &invQ);
        r.w0 = w0;
        r.invQ = invQ;

        const double w0f = (double)w0 / 131072.0;
        const double invQf = (double)invQ / 1024.0;
        const int64_t vi = s;      // only voice 0 is in the filter path
        const int64_t vd = 0;      // voices 1 and 2 are zero
        const double a_dbl = dref.pass(w0f, invQf, (double)vi, (double)vd, vol, mode, is6581);

        for (int q = 0; q < 2; q++) {
            for (int k = 0; k < 5; k++) {
                const int f = FBITS[k];
                IntFilter &m = imod[q][k];
                const int64_t a_int = m.pass(f, w0, invQ, vi, vd, vol, mode, is6581, w0f,
                                             (double)m.vbp / (double)((int64_t)1 << f),
                                             (double)m.vhp / (double)((int64_t)1 << f), q == 1);
                if (q == g_rtl_q && k == g_rtl_k && p >= warm && a_int != a_rtl) r.rtl_vs_int++;
                if (p >= warm) {
                    const double e = ((double)a_int - a_dbl) / (double)vol; // LSB of the mixer input
                    acc[q][k] += e * e;
                    if (std::fabs(e) > amax[q][k]) amax[q][k] = std::fabs(e);
                }
            }
        }
        if (p >= warm) {
            racc += (a_dbl / vol) * (a_dbl / vol);
            n++;
        }
    }

    for (int q = 0; q < 2; q++) {
        for (int k = 0; k < 5; k++) {
            r.err_rms[q][k] = n ? std::sqrt(acc[q][k] / (double)n) : 0;
            r.err_max[q][k] = amax[q][k];
        }
    }
    r.stall_pct = imod[0][0].steps ? 100.0 * (double)imod[0][0].stalls / (double)imod[0][0].steps : 0;
    r.level_db = (n && racc > 0) ? 20.0 * std::log10(std::sqrt(racc / (double)n) / 32768.0) : -999;
    return r;
}

static double to_db(double lsb) { return lsb > 0 ? 20.0 * std::log10(lsb / 32768.0) : -999; }

// ---------------------------------------------------------------------------

int main(int argc, char **argv)
{
    Verilated::commandArgs(argc, argv);
    long passes = 20000;
    bool csv = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--csv")) csv = true;
        else if (!strcmp(argv[i], "--rtlf") && i + 1 < argc) g_rtlf = atoi(argv[++i]);
        else passes = atol(argv[i]);
    }
    g_rtl_k = -1;
    for (int k = 0; k < 5; k++)
        if (FBITS[k] == g_rtlf) g_rtl_k = k;
    if (g_rtl_k < 0) {
        fprintf(stderr, "--rtlf %d: the bench only models F = 0, 2, 4, 6, 8\n", g_rtlf);
        return 2;
    }
    g_rtl_q = (g_rtlf > 0) ? 1 : 0;

    Dut dut;

    const int fcs[] = { 4, 16, 64, 256, 1024, 2047 };
    const int ress[] = { 0, 8, 15 };
    const double freqs[] = { 100.0, 500.0, 2000.0 };
    const int mode = 1; // LP, where a stalled integrator is plainly audible

    long bad_model = 0;
    double worst[2][5];
    for (int q = 0; q < 2; q++)
        for (int k = 0; k < 5; k++) worst[q][k] = -999;
    int worst_fc = 0, worst_res = 0;
    bool worst_6581 = false;
    double worst_freq = 0;

    if (csv) {
        printf("model,fc,res,freq,w0,invQ,level_dBFS,");
        for (int q = 0; q < 2; q++)
            for (int k = 0; k < 5; k++) printf("rms_F%d_%s,", FBITS[k], q ? "round" : "floor");
        for (int q = 0; q < 2; q++)
            for (int k = 0; k < 5; k++) printf("max_F%d_%s,", FBITS[k], q ? "round" : "floor");
        printf("stall_pct,model_mismatch\n");
    } else {
        printf("sid_filter precision, RTL built with FILTER_FRAC = %d, %ld passes per point "
               "(%.0f ms of audio), LP, voice 0 in the filter\n",
               g_rtlf, passes, (double)passes / 1000.0);
        printf("error = the filter minus the same filter in double precision, RMS in LSB of the 16 bit mixer input\n");
        printf("F0 = the hardware as it is. F2..F8 = that many fractional bits added to vlp/vbp/vhp,\n");
        printf("once with the shifts floored as the hardware does them, once rounded to nearest.\n\n");
        printf("%-5s %5s %4s %6s %6s %5s %8s | %7s | %7s %7s %7s %7s | %7s %7s %7s %7s %7s | %6s\n",
               "chip", "FC", "res", "f/Hz", "w0", "1/Q", "lvl/dBFS", "F0 hw",
               "F2 flr", "F4 flr", "F6 flr", "F8 flr",
               "F0 rnd", "F2 rnd", "F4 rnd", "F6 rnd", "F8 rnd", "stall%");
    }

    for (int mi = 0; mi < 2; mi++) {
        const bool is6581 = (mi == 0);
        for (size_t fi = 0; fi < sizeof(fcs) / sizeof(fcs[0]); fi++) {
            for (size_t ri = 0; ri < sizeof(ress) / sizeof(ress[0]); ri++) {
                for (size_t qi = 0; qi < sizeof(freqs) / sizeof(freqs[0]); qi++) {
                    const Result r = run_point(dut, is6581, fcs[fi], ress[ri], freqs[qi], passes, mode);
                    bad_model += r.rtl_vs_int;
                    for (int q = 0; q < 2; q++) {
                        for (int k = 0; k < 5; k++) {
                            const double db = to_db(r.err_rms[q][k]);
                            if (db > worst[q][k]) {
                                worst[q][k] = db;
                                if (q == 0 && k == 0) {
                                    worst_fc = fcs[fi];
                                    worst_res = ress[ri];
                                    worst_6581 = is6581;
                                    worst_freq = freqs[qi];
                                }
                            }
                        }
                    }
                    if (csv) {
                        printf("%s,%d,%d,%.0f,%d,%d,%.1f,", is6581 ? "6581" : "8580", fcs[fi], ress[ri],
                               freqs[qi], r.w0, r.invQ, r.level_db);
                        for (int q = 0; q < 2; q++)
                            for (int k = 0; k < 5; k++) printf("%.3f,", r.err_rms[q][k]);
                        for (int q = 0; q < 2; q++)
                            for (int k = 0; k < 5; k++) printf("%.1f,", r.err_max[q][k]);
                        printf("%.2f,%ld\n", r.stall_pct, r.rtl_vs_int);
                    } else {
                        printf("%-5s %5d %4d %6.0f %6d %5d %8.1f | %7.2f | %7.2f %7.2f %7.2f %7.2f | "
                               "%7.2f %7.2f %7.2f %7.2f %7.2f | %6.2f%s\n",
                               is6581 ? "6581" : "8580", fcs[fi], ress[ri], freqs[qi], r.w0, r.invQ,
                               r.level_db, r.err_rms[0][0],
                               r.err_rms[0][1], r.err_rms[0][2], r.err_rms[0][3], r.err_rms[0][4],
                               r.err_rms[1][0], r.err_rms[1][1], r.err_rms[1][2], r.err_rms[1][3],
                               r.err_rms[1][4], r.stall_pct, r.rtl_vs_int ? "  MODEL!" : "");
                    }
                    fflush(stdout);
                }
            }
        }
    }

    if (!csv) {
        printf("\nworst RMS error over all points, as dBFS of the mixer input:\n");
        printf("  F = 0 floored (the hardware):  %8.1f dBFS\n", worst[0][0]);
        printf("  F = 0 rounded (an add only):    %8.1f dBFS\n", worst[1][0]);
        for (int k = 1; k < 5; k++)
            printf("  F = %d  floored / rounded:      %8.1f / %8.1f dBFS\n", FBITS[k], worst[0][k], worst[1][k]);
        printf("worst point of the hardware: %s FC %d res %d, %.0f Hz\n", worst_6581 ? "6581" : "8580",
               worst_fc, worst_res, worst_freq);
        printf("\nmodel vs RTL: %s (%ld samples differ; the model at F = %d %s must be the RTL)\n",
               bad_model ? "FAIL" : "bit exact", bad_model, g_rtlf, g_rtl_q ? "rounded" : "floored");
    }
    return bad_model ? 1 : 0;
}
