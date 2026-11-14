// SPDX-License-Identifier: GPL-3.0-or-later
// U12: our analog output against libresidfp's, per C64 cycle.
//
// compare_resid checks the digital part - accumulator, LFSR, waveform, envelope
// - and stops there, so our filter and mixer have never been measured against
// anything but themselves. This does that: the same register log into
// libresidfp's SID and into ours, both sampled every C64 cycle, levels matched,
// and the residual reported in dB.
//
// The number is not an equivalence proof and cannot be: libresidfp runs the
// C64's external RC filter over its output and we decimate instead (T25.3), so
// a part of what is left is that difference and not the filter's. What the
// number is good for is **change**: build something into the model, run this
// again, and see whether the distance shrinks.
//
//   filt_cmp <memdir> <curve.mem|-> <log> [cycles] [6581|8580]
//   env: DIGI=0..3 (default 2), RES=0|1 (default 0)
//        OURS=<file>: our side from that file, one sample per C64 cycle (reDIP's output from
//        designs/c64_basic/sim/redip/tb_cmp, U12), instead of the model; memdir and curve are then unused
#include "sidref.h"
#include "SID.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

int main(int argc, char** argv)
{
    if (argc < 4) {
        fprintf(stderr, "filt_cmp <memdir> <curve.mem|-> <log> [cycles] [6581|8580]\n");
        return 2;
    }
    static trxsid::Tables tab;
    if (!tab.load(argv[1])) { fprintf(stderr, "cannot load tables from %s\n", argv[1]); return 2; }

    std::vector<uint16_t> curve;
    if (strcmp(argv[2], "-")) {
        FILE* cf = fopen(argv[2], "r");
        if (!cf) { fprintf(stderr, "cannot open %s\n", argv[2]); return 2; }
        char line[64];
        unsigned v;
        while (fgets(line, sizeof(line), cf))
            if (sscanf(line, "%x", &v) == 1) curve.push_back((uint16_t)v);
        fclose(cf);
        if (curve.size() < 1024) { fprintf(stderr, "%s: %zu entries, 1024 wanted\n", argv[2], curve.size()); return 2; }
    }
    const long ncyc = argc > 4 ? atol(argv[4]) : 400000;
    const bool is8580 = argc > 5 && !strcmp(argv[5], "8580");
    const int digi = getenv("DIGI") ? atoi(getenv("DIGI")) & 3 : 2;
    const int res_high = getenv("RES") ? atoi(getenv("RES")) & 1 : 0;

    // ---- the two models
    reSIDfp::SID rs;
    rs.setChipModel(is8580 ? reSIDfp::CSG8580 : reSIDfp::MOS6581);
    rs.reset();
    // one output sample per C64 cycle, so nothing is resampled away before the
    // comparison; PAL phi2
    const double PHI2 = 985248.0;
    rs.setSamplingParameters(PHI2, reSIDfp::DECIMATE, PHI2);

    trxsid::Sid m(&tab);
    m.cfg.is8580 = is8580;
    m.cfg.res_high = res_high != 0;
    m.cfg.digi = digi;
    if (!curve.empty()) m.cfg.curve = curve.data();
    // U12 D1 (a): OPAMP=<table.mem> from gen_opamp.py (the drive is chosen there)
    static int32_t opamp[1024];
    if (getenv("OPAMP")) {
        if (!trxsid::load_opamp(getenv("OPAMP"), opamp)) { fprintf(stderr, "cannot read %s\n", getenv("OPAMP")); return 1; }
        m.cfg.opamp = opamp;
    }
    m.cfg.wav_sat = getenv("WAVSAT") && atoi(getenv("WAVSAT")) != 0;
    m.cfg.voice_dc = getenv("VDC") ? atof(getenv("VDC")) : 0.0;
    m.reset();

    FILE* f = fopen(argv[3], "r");
    if (!f) { fprintf(stderr, "cannot open %s\n", argv[3]); return 2; }

    std::vector<double> a, b;               // ours, libresidfp's
    a.reserve(ncyc); b.reserve(ncyc);
    short buf[64];
    long cyc = 0, ops = 0;
    char line[256];
    bool eof = false;

    FILE* ours = getenv("OURS") ? fopen(getenv("OURS"), "r") : nullptr;
    if (getenv("OURS") && !ours) { fprintf(stderr, "cannot open %s\n", getenv("OURS")); return 2; }
    auto step = [&]() {
        if (ours) {
            long v = 0;
            if (fscanf(ours, "%ld", &v) != 1) v = 0;
            a.push_back((double)v);
        } else {
            m.clock();
            a.push_back((double)m.out);
        }
        int n = rs.clock(1, buf);
        for (int i = 0; i < n; i++) b.push_back((double)buf[i]);
        cyc++;
    };

    while (cyc < ncyc) {
        if (!eof) {
            if (!fgets(line, sizeof(line), f)) { eof = true; continue; }
            long idle; char rw; unsigned reg, val;
            int k = sscanf(line, "%lx %c %x %x", &idle, &rw, &reg, &val);
            if (k < 3) continue;
            for (long i = 0; i < idle && cyc < ncyc; i++) step();
            if (cyc >= ncyc) break;
            if (rw == 'W' || rw == 'w') {
                if (!ours) m.write(reg & 0x1f, (uint8_t)val);
                rs.write(reg & 0x1f, (uint8_t)val);
            } else {
                if (!ours) m.read(reg & 0x1f);
                rs.read(reg & 0x1f);
            }
            ops++;
            step();
        } else {
            step();
        }
    }
    fclose(f);

    // ---- level match and residual. Both are AC signals of different scale;
    // the fair comparison is after removing the mean and matching the rms.
    size_t n = a.size() < b.size() ? a.size() : b.size();
    if (n < 1000) { printf("filt_cmp: only %zu samples, nothing to say\n", n); return 1; }
    const size_t skip = 2000 < n / 4 ? 2000 : n / 4;    // let both settle
    // EXTFILT: the C64's own output stage, which libresidfp runs and we do not
    // (T25.3 decimates instead). Same time constants as ExternalFilter.h - a
    // 16 kHz RC low-pass and a 1.6 Hz DC blocker - so that known difference is
    // taken out of the number instead of being blamed on the filter.
    if (getenv("EXTFILT") && atoi(getenv("EXTFILT"))) {
        const double dt = 1.0 / PHI2;
        const double alp = dt / (dt + 10e3 * 1000e-12);
        const double ahp = dt / (dt + 10e3 * 10e-6);
        double ylp = 0, yhp = 0;
        for (size_t i = 0; i < a.size(); i++) {
            ylp += alp * (a[i] - ylp);
            yhp += ahp * (ylp - yhp);
            a[i] = ylp - yhp;
        }
    }

    // Optional boxcar before comparing: libresidfp runs the C64's external RC
    // filter and we do not, so the two differ most above the audio band. SMOOTH
    // averages that away instead of pretending it is not there.
    const int sm = getenv("SMOOTH") ? atoi(getenv("SMOOTH")) : 1;
    auto smooth = [&](std::vector<double>& v) {
        if (sm <= 1) return;
        std::vector<double> o(v.size(), 0.0);
        double acc = 0;
        for (size_t i = 0; i < v.size(); i++) {
            acc += v[i];
            if (i >= (size_t)sm) acc -= v[i - sm];
            o[i] = acc / (double)sm;
        }
        v.swap(o);
    };
    smooth(a); smooth(b);

    // Level match last: every transform above changes the scale, so the mean
    // and the rms are taken from the signals as they are compared.
    double ma = 0, mb = 0;
    for (size_t i = skip; i < n; i++) { ma += a[i]; mb += b[i]; }
    ma /= (double)(n - skip); mb /= (double)(n - skip);
    double sa = 0, sb = 0;
    for (size_t i = skip; i < n; i++) { sa += (a[i] - ma) * (a[i] - ma); sb += (b[i] - mb) * (b[i] - mb); }
    sa = sqrt(sa / (double)(n - skip)); sb = sqrt(sb / (double)(n - skip));
    if (sa < 1e-9 || sb < 1e-9) {
        printf("filt_cmp: one side is silent (rms ours %.3g, resid %.3g)\n", sa, sb);
        return 1;
    }

    // A comparison that is not aligned measures nothing: find the lag with the
    // best correlation first, and say what the correlation is, so a residual
    // near 0 dB cannot be mistaken for a verdict about the filter.
    const int maxlag = getenv("MAXLAG") ? atoi(getenv("MAXLAG")) : 256;
    size_t len = n - skip - (size_t)maxlag - 1;
    double best_r = -2;
    int best_lag = 0;
    for (int lag = -maxlag; lag <= maxlag; lag++) {
        double num = 0;
        for (size_t i = 0; i < len; i++) {
            size_t ia = skip + i, ib = (size_t)((long)skip + (long)i + lag);
            num += (a[ia] - ma) * (b[ib] - mb);
        }
        double r = num / ((double)len * sa * sb);
        if (r > best_r) { best_r = r; best_lag = lag; }
    }

    double err = 0, worst = 0;
    for (size_t i = 0; i < len; i++) {
        size_t ia = skip + i, ib = (size_t)((long)skip + (long)i + best_lag);
        double x = (a[ia] - ma) / sa, y = (b[ib] - mb) / sb;
        double e = x - y;
        err += e * e;
        if (fabs(e) > worst) worst = fabs(e);
    }
    err = sqrt(err / (double)len);
    printf("filt_cmp %s %s: %zu samples, %ld ops, rms ours %.0f resid %.0f, "
           "best lag %d, correlation %.4f, residual %.2f dB, worst %.2f dB "
           "(digi %d, res %s, smooth %d, curve %s)\n",
           argv[3], is8580 ? "8580" : "6581", len, ops, sa, sb, best_lag, best_r,
           20 * log10(err), 20 * log10(worst), digi, res_high ? "high" : "low", sm,
           strcmp(argv[2], "-") ? argv[2] : "none");
    return 0;
}
