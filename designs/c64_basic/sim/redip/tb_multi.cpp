// SPDX-License-Identifier: GPL-3.0-or-later
// T25.4 bench: sid_multi (NS SIDs in one reDIP pipeline, 64 clocks per C64
// cycle) against reference streams of the unmodified reDIP: SIDs 2p and 2p+1 of
// the op list are reDIP's SID 1 and 2 of reference run p (tb_ref ... p).
//   tb_multi <oplist> <model of UltiSID 1> <model of UltiSID 2> <refprefix>
// <refprefix>.<p> for p = 0 .. NS/2-1. Compared every cycle: each SID's sample
// (after the previous cycle's pass, like the reference) and every read.
// Needs -DNSIDS=<NS> and the equivalence build (LEGACY_FC = 1, VARIANT_B = 1).
#include "Vsid_multi.h"
#include "Vsid_multi___024root.h"
#include "verilated.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <vector>

struct Op { long cyc; int sid; bool w; unsigned reg, val; };

int main(int argc, char** argv)
{
    Verilated::commandArgs(argc, argv);
    if (argc < 5) return 2;
    const int m[2] = {atoi(argv[2]) == 8580, atoi(argv[3]) == 8580};
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
        if (s >= NSIDS) continue;
        ops.push_back({c, s, t == 'W', r, v});
    }
    fclose(f);
    // reference streams
    struct Ref { long smp[2]; int rd; };
    std::vector<std::vector<Ref>> ref(NSIDS / 2);
    for (int p = 0; p < NSIDS / 2; p++) {
        char name[512];
        snprintf(name, sizeof name, "%s.%d", argv[4], p);
        FILE* r = fopen(name, "r");
        if (!r) { fprintf(stderr, "cannot open %s\n", name); return 2; }
        long n; unsigned a, b; int rd;
        while (fscanf(r, "%ld %x %x %d", &n, &a, &b, &rd) == 4) ref[p].push_back({{(long)a, (long)b}, rd});
        fclose(r);
    }
    Vsid_multi* d = new Vsid_multi;
    auto tick = [&]() { d->clk = 0; d->eval(); d->clk = 1; d->eval(); };
    d->cfg_8580 = m[0] | (m[1] << 1);
    d->cfg_res_high = d->cfg_8580;                       // reDIP: 1/Q table follows the model
    d->cfg_digi = (m[0] ? 0 : 3) | ((m[1] ? 0 : 3) << 2);  // reDIP: voice DC for the 6581 only
    // LEGACY_FC = 2 build: the reset values of the app's registers (T25.5) = the original reDIP; the
    // explicit cfg_res_high / cfg_digi above are overruled by Auto. With LEGACY_FC = 1 these are not read.
    d->cfg_fc_sel = 0;
    d->cfg_bypass = getenv("BYPASS") ? 3 : 0;   // T25.5: filter bypass of both UltiSIDs (run_bypass.sh)
    d->cfg_auto = 0xF;
    int fb = getenv("FC_BASE") ? atoi(getenv("FC_BASE")) : 245, fo = getenv("FC_OFF") ? atoi(getenv("FC_OFF")) : -780;
    d->cfg_fc_base = fb | (fb << 9);
    d->cfg_fc_off = (fo & 0x7ff) | ((fo & 0x7ff) << 11);
    if (getenv("FC_SEL")) d->cfg_fc_sel = atoi(getenv("FC_SEL")) * 5;
    if (getenv("FC2_SCRAMBLE")) { d->cfg_res_high = d->cfg_8580 ^ 3; d->cfg_digi = 0x5; }   // Auto must not look at them
    d->start = 0; d->op_valid_i = 0; d->lv_addr = 0;
    d->rst = 1;
    for (int i = 0; i < 64; i++) tick();
    d->rst = 0;
    for (int i = 0; i < 8; i++) tick();
    long nerr = 0, nreads = 0, nsmp_cmp = 0;
    const int maxerr = getenv("MAXERR") ? atoi(getenv("MAXERR")) : 10;
    long last[8] = {0};
    auto rbfield = [&](int s, int off) {
        // rb_all: 24 bits per SID: osc3 [7:0], env3 [15:8], bus [23:16]
        int bit = 24 * s + off;
        unsigned x = 0;
#if NSIDS * 24 <= 64
        for (int i = 0; i < 8; i++) x |= (unsigned)((((uint64_t)d->rb_all) >> (bit + i)) & 1u) << i;
#else
        for (int i = 0; i < 8; i++) x |= ((d->rb_all[(bit + i) / 32] >> ((bit + i) % 32)) & 1u) << i;
#endif
        return x;
    };
    const int dj = getenv("DBGJ") ? atoi(getenv("DBGJ")) : -1;
    const long d0 = getenv("DBGFROM") ? atol(getenv("DBGFROM")) : 0, d1 = getenv("DBGTO") ? atol(getenv("DBGTO")) : 0;
    for (long cyc = 0; cyc < total; cyc++) {
        bool have = !ops.empty() && ops.front().cyc == cyc;
        Op op = {0, 0, false, 0, 0};
        if (have) { op = ops.front(); ops.pop_front(); }
        // compare: samples after the previous cycle's pass, the read of this cycle
        if (cyc >= 3) {
            for (int s = 0; s < NSIDS; s++) {
                const Ref& R = ref[s / 2][cyc];
                long a = last[s] & 0xfffff;
                if (a != R.smp[s & 1]) {
                    if (nerr++ < maxerr) printf("cycle %ld sid %d sample: rtl %05lx ref %05lx\n", cyc, s, a, R.smp[s & 1]);
                }
                nsmp_cmp++;
            }
        }
        unsigned rd = 0;
        if (have && !op.w) {
            switch (op.reg) {
            case 0x1b: rd = rbfield(op.sid, 0); break;
            case 0x1c: rd = rbfield(op.sid, 8); break;
            default: rd = rbfield(op.sid, 16); break;
            }
            int want = ref[op.sid / 2][cyc].rd;
            nreads++;
            // the bypass run feeds other write values than the reference: the open-bus read-back differs by design
            const bool skip = getenv("BYPASS") && op.reg != 0x1b && op.reg != 0x1c;
            if (cyc >= 3 && !skip && (int)rd != want && nerr++ < maxerr)
                printf("cycle %ld sid %d read %02x: rtl %02x ref %02x\n", cyc, op.sid, op.reg, rd, want);
        }
        d->op_valid_i = have;
        d->op_we_i = op.w;
        d->op_sid_i = op.sid;
        d->op_reg_i = op.reg;
        d->op_wdata_i = op.val;
        d->op_rdata_i = rd;
        d->start = 1;
        int nsmp = 0;
        for (int k = 0; k < 64; k++) {
            tick();
            d->start = 0;
            if (dj >= 0 && cyc >= d0 && cyc <= d1 && (d->rootp->sid_multi__DOT__core__DOT__voice_cycle == 6 + dj || d->rootp->sid_multi__DOT__core__DOT__voice_cycle == 6 + 3 * NSIDS + dj))
                printf("%ld vc %d wf5 %x m5 %d p3 %d st5 %03x wav %03x env %02x\n", cyc, (int)d->rootp->sid_multi__DOT__core__DOT__voice_cycle, d->rootp->sid_multi__DOT__core__DOT__waveform__DOT__waveform_5, d->rootp->sid_multi__DOT__core__DOT__waveform__DOT__model_5, (d->rootp->sid_multi__DOT__core__DOT__waveform__DOT__pulse>>3)&1, d->rootp->sid_multi__DOT__core__DOT__waveform__DOT__saw_tri_5, d->rootp->sid_multi__DOT__core__DOT__wav, d->rootp->sid_multi__DOT__core__DOT__env);
            if (d->smp_valid) { last[d->smp_sid] = (int32_t)(d->smp_data << 12) >> 12; nsmp++; }
        }
        if (cyc >= 3 && nsmp != NSIDS && nerr++ < maxerr) printf("cycle %ld: %d samples\n", cyc, nsmp);
        if (nerr >= maxerr) break;
    }
    printf("%s: %ld cycles, %ld reads, %ld sample compares, %ld errors\n", nerr ? "FAIL" : "PASS", total, nreads, nsmp_cmp, nerr);
    delete d;
    return nerr ? 1 : 0;
}
