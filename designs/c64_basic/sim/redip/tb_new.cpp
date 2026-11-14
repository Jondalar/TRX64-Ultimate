// SPDX-License-Identifier: GPL-3.0-or-later
// T25.4/T25.5 bench for what reDIP does not have (sid_multi, hardware build: LEGACY_FC = 2,
// VARIANT_B = 0, NS = 8): no reference stream, the expected values come from the spec.
//   tb_new <seed> <cycles>
// Filter source, Auto bits and the 6581 curve parameters are random per UltiSID and per seed.
// 1. cutoff: the filter's w0*T is 5 * FC for the 8580 forms (source 2, reserved 3, source 0 with
//    the 8580 model) and a function of FC, base and offset only for the own 6581 curve (source 1,
//    source 0 with the 6581 model), for every SID and every pass (random FC writes)
// 2. 1/Q: RES Low/High table per UltiSID (res_hi), independent of WAVES
// 3. DIGI: the voice DC of every voice slot is 0x800*0xff >> {inf, 2, 1, 0} by its UltiSID's DIGI
// 4. the two UltiSID sums are the saturated sums of the four SIDs' samples
// 5. envelope levels (+0x80..): after gating voice v of SID s alone, level {s, v} rises to the
//    sustain level and no other level moves
// 6. ENV3 of every SID equals level {s, 2} at the end of a pass
// 7. U12 (OPAMP_6581): every SID's sample is its master volume times the mixer input (taken at stage 8): for a
//    6581 the direct voices plus the filter output (rounded to whole LSB) through the op-amp table
//    (sid_opamp_6581.mem, interpolated as gen_opamp.py --redip describes it), for an 8580 reDIP's own sum, each
//    through the 16 bit clamp
#include "Vsid_multi.h"
#include "Vsid_multi___024root.h"
#include "verilated.h"
#include <cstdio>
#include <cstdlib>
#include <random>
#include <map>
#include <tuple>

#define R(x) d->rootp->sid_multi__DOT__##x

static long clamp16(long x) { return x > 32767 ? 32767 : x < -32768 ? -32768 : x; }

static const int Q6[16] = {1448, 1324, 1219, 1129, 1052, 984, 925, 872, 826, 783, 745, 711, 679, 651, 624, 600};
static const int Q8[16] = {1448, 1328, 1218, 1117, 1024, 939, 861, 790, 724, 664, 609, 558, 512, 470, 431, 395};

int main(int argc, char** argv)
{
    Verilated::commandArgs(argc, argv);
    const int seed = argc > 1 ? atoi(argv[1]) : 1;
    const long ncyc = argc > 2 ? atol(argv[2]) : 20000;
    std::mt19937 rnd(seed);
    Vsid_multi* d = new Vsid_multi;
    long nerr = 0, nchk[8] = {0};
    // 7: the op-amp table, as the build reads it (the .mem files are copied into the run directory)
    static long opamp[1024];
    {
        FILE* f = fopen("sid_opamp_6581.mem", "r");
        if (!f) { printf("FAIL: no sid_opamp_6581.mem\n"); return 1; }
        char line[512]; int n = 0;            // the header comment is longer than a value line
        while (n < 1024 && fgets(line, sizeof line, f)) {
            if (line[0] == '/' || line[0] == '\n') continue;
            long v = strtol(line, nullptr, 16) & 0xffff;
            opamp[n++] = v & 0x8000 ? v - 0x10000 : v;
        }
        fclose(f);
        if (n != 1024) { printf("FAIL: sid_opamp_6581.mem has %d values\n", n); return 1; }
    }
    auto opamp_of = [&](long x) {          // x: the 17 bit mixer sum
        const long u = x + 65536, i = u >> 7, i1 = i == 1023 ? 1023 : i + 1, fr = u & 0x7f;
        return clamp16(opamp[i] + (((opamp[i1] - opamp[i]) * fr) >> 7));
    };
    long ms[8] = {0}, mv[8] = {0}, m6[8] = {0}, mvd[8] = {0}, mfr[8] = {0};
    const int maxerr = 12;
    auto bad = [&](const char* what, long cyc, int k, long a, long b) {
        if (nerr++ < maxerr) printf("cycle %ld %s %d: rtl %lx expected %lx\n", cyc, what, k, a, b);
    };
    auto tick = [&]() { d->clk = 0; d->eval(); d->clk = 1; d->eval(); };
    // configuration: random per run
    const int waves = rnd() & 3, reshi = rnd() & 3;
    int digi[2] = {int(rnd() & 3), int(rnd() & 3)};
    d->cfg_8580 = waves;
    d->cfg_res_high = reshi;
    d->cfg_digi = digi[0] | (digi[1] << 2);
    // filter source, Auto bits and the 6581 curve parameters random per UltiSID; the checks below follow them
    const bool fc2 = true;
    int sel[2], aut[2], bas[2], ofs[2];
    for (int u = 0; u < 2; u++) {
        sel[u] = rnd() & 3; aut[u] = rnd() & 3; bas[u] = rnd() & 511; ofs[u] = int(rnd() & 2047) - 1024;
    }
    d->cfg_fc_sel = sel[0] | (sel[1] << 2);
    d->cfg_auto = aut[0] | (aut[1] << 2);
    d->cfg_fc_base = bas[0] | (bas[1] << 9);
    d->cfg_fc_off = (ofs[0] & 0x7ff) | ((ofs[1] & 0x7ff) << 11);
    std::map<std::tuple<unsigned, int, int>, long> own6581;
    d->start = 0; d->op_valid_i = 0; d->lv_addr = 0;
    d->rst = 1;
    for (int i = 0; i < 64; i++) tick();
    d->rst = 0;
    for (int i = 0; i < 4; i++) tick();

    unsigned reg[8][32] = {{0}};
    // 4: the sums come first, then the pass's eight samples, one per clock. With the 6581's mixing node (U12) the
    // last of them is out after the 64 clocks of its pass, so they are counted to their sums, not to the window.
    long psmp[8] = {0}, psum[2] = {0, 0};
    int pn = -1;
    auto pass = [&](bool have, int s, bool w, unsigned r, unsigned v, long cyc, bool check) {
        d->op_valid_i = have;
        d->op_we_i = w;
        d->op_sid_i = s;
        d->op_reg_i = r;
        d->op_wdata_i = v;
        d->op_rdata_i = 0;
        d->start = 1;
        if (have && w) reg[s][r] = v;
        for (int k = 0; k < 64; k++) {
            tick();
            d->start = 0;
            if (d->sum_valid) {
                if (pn >= 0 && check) bad("sum", cyc, -1, pn, 8);   // a pass whose samples did not all come
                psum[0] = (int32_t)(d->sum_u0 << 12) >> 12; psum[1] = (int32_t)(d->sum_u1 << 12) >> 12;
                pn = 0;
            } else if (d->smp_valid && pn >= 0) {
                psmp[d->smp_sid] = (int32_t)(d->smp_data << 12) >> 12;
                if (++pn == 8) {
                    if (check) {
                        for (int q = 0; q < 8; q++) {
                            const long want = mv[q] * (m6[q] ? clamp16(mvd[q] + opamp_of(mfr[q])) : clamp16(ms[q]));
                            nchk[7]++;
                            if (psmp[q] != want) bad("mixer", cyc, q, psmp[q], want);
                        }
                        long sums[2] = {0, 0};
                        for (int s2 = 0; s2 < 8; s2++) sums[s2 >> 2] += psmp[s2];
                        for (int u = 0; u < 2; u++) {
                            long x = sums[u] > 0x7ffff ? 0x7ffff : sums[u] < -0x80000 ? -0x80000 : sums[u];
                            nchk[3]++;
                            if (x != psum[u]) bad("sum", cyc, u, psum[u], x);
                        }
                    }
                    pn = -1;
                }
            }
            {
                // 7: the mixer sum, volume and model of SID q at its stage 8
                const int fc8 = R(core__DOT__filter_cycle);
                for (int q = 0; q < 8; q++)
                    if (fc8 == 8 + 5 * q) {
                        ms[q] = (long)((int32_t)(R(core__DOT__filter__DOT__msum) << 15) >> 15);
                        mv[q] = R(core__DOT__filter__DOT__vol);
                        m6[q] = R(core__DOT__filter__DOT__model_r) == 0;
                        // the hardware build: FILTER_FRAC = 6, vf 23 bit with 6 fractional bits, vd1 24 bit
                        const long vf = (long)((int32_t)(R(core__DOT__filter__DOT__vf) << 9) >> 9);
                        const long vd1 = (long)((int32_t)(R(core__DOT__filter__DOT__vd1) << 8) >> 8);
                        const long x17 = ((vf + 32) >> 6) & 0x1ffff, d17 = (vd1 >> 7) & 0x1ffff;
                        mfr[q] = x17 & 0x10000 ? x17 - 0x20000 : x17;
                        mvd[q] = d17 & 0x10000 ? d17 - 0x20000 : d17;
                    }
            }
            if (!check) continue;
            int fc = R(core__DOT__filter_cycle), vc = R(core__DOT__voice_cycle);
            for (int q = 0; q < 8; q++) {
                // w0 of SID q during filter cycle 5 + 5q
                if (fc == 5 + 5 * q) {
                    unsigned fcv = (reg[q][0x16] << 3) | (reg[q][0x15] & 7);
                    long want = 5 * fcv;
                    long got = R(core__DOT__filter__DOT__a) & 0xffff;
                    const int u = q >> 2, model8580 = (waves >> u) & 1;
                    nchk[0]++;
                    if (sel[u] == 1 || (sel[u] == 0 && !model8580)) {
                        // reDIP's own 6581 curve: the value is a function of FC, base and offset only
                        int b = bas[u] - (q & 1) * 5; if (b < 0) b = 0;
                        int o = ofs[u] - (q & 1) * 5; if (o < -1024) o = -1024;
                        auto key = std::make_tuple(fcv, b, o);
                        auto it = own6581.find(key);
                        if (it == own6581.end()) own6581[key] = got;
                        else want = it->second;
                        if (it == own6581.end()) want = got;
                    }                                       // else 5 * FC: source 2, 3 (reserved), 8580 chip's own
                    if (got != want) bad("w0", cyc, q, got, want);
                }
                if (fc == 4 + 5 * q) {
                    int res = reg[q][0x17] >> 4;
                    const int u = q >> 2;
                    int rh = (fc2 && (aut[u] & 1)) ? ((waves >> u) & 1) : ((reshi >> u) & 1);
                    int want = rh ? Q8[res] : Q6[res];
                    nchk[1]++;
                    if (int(R(core__DOT__filter__DOT___1_Q_lsl10)) != want) bad("1/Q", cyc, q, R(core__DOT__filter__DOT___1_Q_lsl10), want);
                }
            }
            for (int j = 0; j < 24; j++)
                if (vc == 8 + j) {
                    static const long DC = 0x800 * 0xff;
                    const int u = (j / 3) >> 2;
                    int dg = (fc2 && (aut[u] & 2)) ? (((waves >> u) & 1) ? 0 : 3) : digi[u];
                    long want = dg == 0 ? 0 : dg == 1 ? DC >> 2 : dg == 2 ? DC >> 1 : DC;
                    nchk[2]++;
                    if ((long)(int32_t)R(core__DOT__voice__DOT__dca_DC) != want) bad("dc", cyc, j, R(core__DOT__voice__DOT__dca_DC), want);
                }
        }
        if (check) {
            for (int s2 = 0; s2 < 8; s2++) {
                d->lv_addr = s2 * 4 + 2;
                d->eval();
                unsigned env3 = (d->rb_all[(24 * s2 + 8) / 32] >> ((24 * s2 + 8) % 32)) & 0xff;
                nchk[4]++;
                if (d->lv_data != env3) bad("env3/level", cyc, s2, d->lv_data, env3);
            }
        }
    };
    // 1-4, 6: random traffic
    for (long cyc = 0; cyc < ncyc && nerr < maxerr; cyc++) {
        bool have = (rnd() % 3) != 0;
        int s = rnd() & 7;
        unsigned r;
        switch (rnd() % 4) {
        case 0: r = 0x15 + (rnd() & 3); break;       // filter registers
        case 1: r = rnd() % 21; break;               // voice registers
        case 2: r = 0x17; break;
        default: r = rnd() % 29; break;
        }
        bool w = (rnd() % 4) != 0 && r < 0x19;
        unsigned v = rnd() & 0xff;
        if (r == 0x18 && w) v = (v & 0xf0) | (rnd() % 16);
        pass(have, s, w, r, v, cyc, cyc > 3);
    }
    // 5: one voice after the other
    {
        // quiet start: every voice ungated, test bit off, envelope parameters fast
        for (int s = 0; s < 8; s++)
            for (int v = 0; v < 3; v++) {
                pass(true, s, true, 7 * v + 5, 0x00, 0, false);
                pass(true, s, true, 7 * v + 6, 0xf0, 0, false);
                pass(true, s, true, 7 * v + 4, 0x00, 0, false);
            }
        for (int i = 0; i < 40000; i++) pass(false, 0, false, 0, 0, 0, false);
        unsigned base[32], cur[32];
        auto lv = [&](unsigned* a) {
            for (int e = 0; e < 32; e++) { d->lv_addr = e; d->eval(); a[e] = d->lv_data; }
        };
        lv(base);
        for (int s = 0; s < 8 && nerr < maxerr; s++)
            for (int v = 0; v < 3; v++) {
                pass(true, s, true, 7 * v + 4, 0x01, 0, false);   // gate (AD = 0, SR = F0: attack, decay to 255)
                for (int i = 0; i < 3000; i++) pass(false, 0, false, 0, 0, 0, false);
                lv(cur);
                for (int e = 0; e < 32; e++) {
                    bool me = e == s * 4 + v;
                    nchk[5]++;
                    if (me ? cur[e] != 255 : cur[e] != base[e]) bad("level", s * 4 + v, e, cur[e], me ? 255 : base[e]);
                }
                lv(base);
            }
    }
    // 7: /RES clears the registers of every SID: filter registers, voice registers (the whole
    // ring) and the bus values, in passes that run while it is held
    {
        for (int s = 0; s < 8; s++)
            for (int r = 0; r < 25; r++) pass(true, s, true, r, 1 + (rnd() % 255), 0, false);
        d->rst = 1;
        for (int i = 0; i < 3; i++) pass(false, 0, false, 0, 0, 0, false);
        d->rst = 0;
        pass(false, 0, false, 0, 0, 0, false);
        long nz = 0;
        for (int s = 0; s < 8; s++) {
            if (R(core__DOT__control__DOT__f)[s] != 0) nz++;
            if (((d->rb_all[(24 * s + 16) / 32] >> ((24 * s + 16) % 32)) & 0xff) != 0) nz++;
        }
        for (int i = 0; i < 24; i++) if (R(core__DOT__control__DOT__v)[i] != 0) nz++;
        nchk[6] = 8 + 8 + 24;
        if (nz) bad("reset left nonzero", ncyc, 0, nz, 0);
    }
    printf("%s: seed %d waves %d reshi %d digi %d/%d, %ld cycles; checks w0 %ld 1/Q %ld dc %ld sum %ld env3 %ld level %ld reset %ld mixer %ld, %ld errors\n",
           nerr ? "FAIL" : "PASS", seed, waves, reshi, digi[0], digi[1], ncyc, nchk[0], nchk[1], nchk[2], nchk[3], nchk[4], nchk[5], nchk[6], nchk[7], nerr);
    delete d;
    return nerr ? 1 : 0;
}
