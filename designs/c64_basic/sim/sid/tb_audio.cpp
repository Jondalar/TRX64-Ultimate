// SPDX-License-Identifier: GPL-3.0-or-later
// T25.3 bench: trx_audio (mixer + CIC + halfbands + fractional FIR + FIFO)
// on its two real clocks (clk2x = 2 x dot4x, mclk = 12.2888 MHz), fed a sine
// per C64 cycle into UltiSID 1 (or white noise, or an impulse); writes the
// 48 kHz output (L channel) as text for analyse_audio.py.
//   tb_audio <freq Hz | noise | impulse> <amplitude 0..1> <C64 cycles> <out.txt> [pause_from pause_len]
// Also prints how many samples came out against how many the exact ratio
// 2400/49259 asks for. VMODE=0..3 (T40): the System Mode's phi2 and ratio.
#include "Vtrx_audio.h"
#include "Vtrx_audio___024root.h"
#include "verilated.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>

int main(int argc, char** argv)
{
    Verilated::commandArgs(argc, argv);
    if (argc < 5) return 2;
    const bool noise = !strcmp(argv[1], "noise"), impulse = !strcmp(argv[1], "impulse");
    const double f = (noise || impulse) ? 0 : atof(argv[1]);
    const double amp = atof(argv[2]);
    const long ncyc = atol(argv[3]);
    FILE* o = fopen(argv[4], "w");
    const long p_from = argc > 6 ? atol(argv[5]) : -1, p_len = argc > 6 ? atol(argv[6]) : 0;
    // T40 §9.2/§9.5: phi2 and outputs per input (x 16) per System Mode
    const int vm = getenv("VMODE") ? atoi(getenv("VMODE")) & 3 : 0;
    const double PHI2[4] = {50e6 * 56.75 / 3 / 30 / 32, 50e6 * 47.125 / 3 / 24 / 32,
                            50e6 * 43.25 / 3 / 22 / 32, 50e6 * 35.375 / 2 / 28 / 32};
    const double RATIO[4] = {38400.0 / 49259.0, 307200.0 / 409045.0, 422400.0 / 563115.0, 307200.0 / 394785.0};
    const double F_PHI2 = PHI2[vm];
    const double T2X = 1.0 / (F_PHI2 * 64), TM = 81.375 / 1e9;   // seconds
    Vtrx_audio* d = new Vtrx_audio;
    std::mt19937 rnd(5);
    std::normal_distribution<double> nd(0, 0.25);
    d->gains = 0x80808080;
    d->vmode = vm;
    d->rst = 1;
    d->mrst = 1;
    double t2 = 0, tm = TM / 3;
    auto step = [&]() {
        // advance to the next clock edge (rising), whichever comes first
        if (t2 <= tm) {
            d->clk = 1; d->eval(); d->clk = 0; d->eval();
            t2 += T2X;
        } else {
            d->mclk = 1; d->eval(); d->mclk = 0; d->eval();
            tm += TM;
        }
    };
    for (int i = 0; i < 2000; i++) step();
    d->rst = 0;
    d->mrst = 0;
    long nout = 0, mcount = 0, c64 = 0, sub = 0, unprimed = 0;
    int qmin = 99, qmax = -1;
    while (c64 < ncyc) {
        bool is2x = t2 <= tm;
        if (is2x) {
            d->in_valid = sub == 0 && !(c64 >= p_from && c64 < p_from + p_len);
            if (sub == 0) {
                double x = noise ? nd(rnd) : impulse ? (c64 == 1000 ? 1.0 : 0.0)
                                                     : sin(2 * M_PI * f * c64 / F_PHI2);
                if (x > 1) x = 1;
                if (x < -1) x = -1;
                d->in0 = (int)lrint(x * amp * 524287) & 0xfffff;
                d->in1 = 0;
            }
        } else {
            d->sample_en = (mcount % 256) == 255;
        }
        step();
        if (is2x) {
            if (++sub == 64) { sub = 0; c64++; }
        } else {
            if (d->sample_en) {
                int ql = d->rootp->trx_audio__DOT__ql;
                if (nout > 200) { if (ql < qmin) qmin = ql; if (ql > qmax) qmax = ql; }
                if (!d->rootp->trx_audio__DOT__primed && nout > 200) unprimed++;
                fprintf(o, "%d\n", (int32_t)(d->out_l << 8) >> 8);
                nout++;
            }
            mcount++;
        }
    }
    fclose(o);
    double want = ncyc * RATIO[vm] / 16.0;
    printf("tb_audio %s: %ld C64 cycles, %ld output frames (exact ratio: %.1f), FIFO level %d..%d, %ld frames unprimed\n",
           argv[1], ncyc, nout, want, qmin, qmax, unprimed);
    delete d;
    return 0;
}
