// SPDX-License-Identifier: GPL-3.0-or-later
// T25.6: the mixer with the sampler inputs (trx_audio): which input takes which gain byte.
//   tb_mix <lane mask: bit0 UltiSID 1, bit1 UltiSID 2, bit2 sampler L, bit3 sampler R> <gains hex> <gains2 hex> <C64 cycles> <out.txt>
// A 1 kHz sine of 0.3 full scale (the sampler lanes 23 bit with the same amplitude as the 20 bit ones) on each
// lane in the mask; writes L and R of every 48 kHz frame.
#include "Vtrx_audio.h"
#include "verilated.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>

int main(int argc, char **argv)
{
    Verilated::commandArgs(argc, argv);
    if (argc < 6) return 2;
    int mask = strtol(argv[1], 0, 16);
    unsigned g = strtoul(argv[2], 0, 16), g2 = strtoul(argv[3], 0, 16);
    long ncyc = atol(argv[4]);
    FILE *o = fopen(argv[5], "w");
    const double F_PHI2 = 50e6 * 56.75 / 3 / 30 / 32, T2X = 1.0 / (F_PHI2 * 64), TM = 81.375 / 1e9;
    Vtrx_audio *d = new Vtrx_audio;
    d->gains = g; d->gains2 = g2; d->vmode = 0; d->rst = 1; d->mrst = 1;
    d->in0 = d->in1 = d->in2 = d->in3 = 0;
    double t2 = 0, tm = TM / 3;
    auto step = [&]() {
        if (t2 <= tm) { d->clk = 1; d->eval(); d->clk = 0; d->eval(); t2 += T2X; }
        else { d->mclk = 1; d->eval(); d->mclk = 0; d->eval(); tm += TM; }
    };
    for (int i = 0; i < 2000; i++) step();
    d->rst = 0; d->mrst = 0;
    long c64 = 0, sub = 0, mcount = 0;
    while (c64 < ncyc) {
        bool is2x = t2 <= tm;
        if (is2x) {
            d->in_valid = sub == 0;
            if (sub == 0) {
                double x = 0.3 * sin(2 * M_PI * 1000.0 * c64 / F_PHI2);
                d->in0 = (mask & 1) ? ((int)lrint(x * 524287) & 0xfffff) : 0;
                d->in1 = (mask & 2) ? ((int)lrint(-x * 524287) & 0xfffff) : 0;
                d->in2 = (mask & 4) ? ((int)lrint(x * 524287) & 0x7fffff) : 0;
                d->in3 = (mask & 8) ? ((int)lrint(-x * 524287) & 0x7fffff) : 0;
            }
        } else d->sample_en = (mcount % 256) == 255;
        step();
        if (is2x) { if (++sub == 64) { sub = 0; c64++; } }
        else { if (d->sample_en) fprintf(o, "%d %d\n", (int32_t)(d->out_l << 8) >> 8, (int32_t)(d->out_r << 8) >> 8); mcount++; }
    }
    fclose(o);
    return 0;
}
