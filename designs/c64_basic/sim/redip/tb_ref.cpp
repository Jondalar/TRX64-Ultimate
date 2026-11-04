// SPDX-License-Identifier: GPL-3.0-or-later
// T25.4 reference bench: the unmodified reDIP SID (sid_api, two SIDs) fed from
// a merged op list (mklog.py). Only SIDs 0 and 1 of the list are applied, or,
// with `pair p`, SIDs 2p and 2p+1 as reDIP's SID 1 and 2 (the others idle).
//   tb_ref <oplist> <model 6581|8580> <variants 0|1> <pair> <out> [mono]
// mono = 1: both SIDs at $D400 (every op reaches both; only the ops of SID 0 of the
// pair are applied).
// out: one line per C64 cycle: "<n> <smp1> <smp2> <read>" hex, the samples are
// those after the pass the cycle's op started (sampled in the next cycle just
// before its phi2 falls), <read> the byte a read in this cycle returned (-1
// for no read). 32 clocks per C64 cycle (dot4x), phi2 high for the first 16.
#include "Vsid_api.h"
#ifdef REF_DEBUG
#include "Vsid_api___024root.h"
#endif
#include "verilated.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>

struct Op { long cyc; int sid; bool w; unsigned reg, val; };

int main(int argc, char** argv)
{
    Verilated::commandArgs(argc, argv);
    if (argc < 6) return 2;
    const int model8580 = atoi(argv[2]) == 8580;
    const int variants = atoi(argv[3]);
    const int pair = atoi(argv[4]);
    const int mono = argc > 6 ? atoi(argv[6]) : 0;
    std::deque<Op> ops;
    long total = 0;
    FILE* f = fopen(argv[1], "r");
    if (!f) return 2;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        long c; int s; char t; unsigned r, v;
        int n = sscanf(line, "%ld %d %c %x %x", &c, &s, &t, &r, &v);
        if (n == 1) { total = c; break; }
        if (n < 5) continue;
        if (s / 2 != pair) continue;
        if (mono && (s & 1)) continue;
        ops.push_back({c, s & 1, t == 'W', r, v});
    }
    fclose(f);
    FILE* o = fopen(argv[5], "w");
    Vsid_api* d = new Vsid_api;
    auto tick = [&]() { d->clk = 0; d->eval(); d->clk = 1; d->eval(); };
    d->sid_model = model8580;
    d->sid_mono = mono;
    d->sid_variants = variants;
    d->pot_charged = 0;
    d->audio_i_left = 0; d->audio_i_right = 0;
    d->cs_io1_n = 1; d->cs_n = 1; d->bus_r_w_n = 1; d->a5 = 0; d->a8 = 0; d->bus_addr = 0; d->bus_data = 0;
    d->bus_phi2 = 0; d->bus_phi2_n = 0;
    d->bus_res = 1;
    for (int i = 0; i < 64; i++) tick();
    d->bus_res = 0;
    long smp1 = 0, smp2 = 0;
    auto s20 = [](unsigned x) { return (long)((int32_t)(x << 12) >> 12); };
#ifdef REF_DEBUG
    // DBGJ=<voice slot> DBGFROM/DBGTO=<cycles>: print the voice's waveform of both rotations
    const int dj = getenv("DBGJ") ? atoi(getenv("DBGJ")) : -1;
    const long d0 = getenv("DBGFROM") ? atol(getenv("DBGFROM")) : 0, d1 = getenv("DBGTO") ? atol(getenv("DBGTO")) : 0;
#endif
    for (long cyc = 0; cyc < total; cyc++) {
        int rd = -1;
        bool have = !ops.empty() && ops.front().cyc == cyc;
        Op op = {0, 0, false, 0, 0};
        if (have) { op = ops.front(); ops.pop_front(); }
        for (int k = 0; k < 32; k++) {
            if (k == 0) {
                d->bus_phi2 = 1;
                if (have) {
                    d->cs_n = 0; d->a5 = op.sid; d->a8 = 0;
                    d->bus_addr = op.reg; d->bus_data = op.val; d->bus_r_w_n = !op.w;
                } else {
                    d->cs_n = 1; d->bus_r_w_n = 1; d->a5 = 0;
                }
            }
            if (k == 15) { smp1 = s20(d->audio_o_left); smp2 = s20(d->audio_o_right); }
            if (k == 14 && have && !op.w) { d->eval(); rd = d->data_o; }
            d->bus_phi2_n = (k == 16);
            if (k == 16) d->bus_phi2 = 0;
            tick();
#ifdef REF_DEBUG
            if (dj >= 0 && cyc >= d0 && cyc <= d1 && (d->rootp->sid_api__DOT__voice_cycle == 6 + dj || d->rootp->sid_api__DOT__voice_cycle == 12 + dj))
                printf("%ld vc %d wf5 %x m5 %d p3 %d st5 %03x wav %03x env %02x\n", cyc, (int)d->rootp->sid_api__DOT__voice_cycle, d->rootp->sid_api__DOT__waveform__DOT__waveform_5, d->rootp->sid_api__DOT__waveform__DOT__model_5, (d->rootp->sid_api__DOT__waveform__DOT__pulse>>3)&1, d->rootp->sid_api__DOT__waveform__DOT__saw_tri_5, d->rootp->sid_api__DOT__wav, d->rootp->sid_api__DOT__env);
#endif
        }
        // the sample after the pass of the previous cycle's op: written one cycle late
        fprintf(o, "%ld %05lx %05lx %d\n", cyc, smp1 & 0xfffff, smp2 & 0xfffff, rd);
    }
    fclose(o);
    delete d;
    return 0;
}
