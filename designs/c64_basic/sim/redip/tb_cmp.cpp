// SPDX-License-Identifier: GPL-3.0-or-later
// U12: reDIP's output (sid_multi, hardware build) for one register log, one sample per C64 cycle, for
// cores/trx_sid/model/filt_cmp (OURS=<file>), which compares it with libresidfp's. The op-amp table is read from
// the run directory (sid_opamp_6581.mem), so a drive is tried by writing another table there, without a rebuild.
//   tb_cmp <log> <cycles> <out> [6581|8580]
// SID 0 plays the log (format of gen_logs.py: "<idle> W|R <reg> <val>", idle decimal, reg and val hex); UltiSID 1 runs as the app sets
// reSID8 by default: Filter Curve "Chip Model" (fc_sel 0), Resonance and Digis "Auto" (by the model), 6581 base
// 245 and offset -780. 64 clocks per C64 cycle; the sample of a cycle is SID 0's output of that cycle's pass.
#include "Vsid_multi.h"
#include "verilated.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

int main(int argc, char** argv)
{
    Verilated::commandArgs(argc, argv);
    if (argc < 4) { fprintf(stderr, "tb_cmp <log> <cycles> <out> [6581|8580]\n"); return 2; }
    const long ncyc = atol(argv[2]);
    const bool is8580 = argc > 4 && !strcmp(argv[4], "8580");
    FILE* lf = fopen(argv[1], "r");
    FILE* of = fopen(argv[3], "w");
    if (!lf || !of) { fprintf(stderr, "cannot open %s or %s\n", argv[1], argv[3]); return 2; }

    Vsid_multi* d = new Vsid_multi;
    auto tick = [&]() { d->clk = 0; d->eval(); d->clk = 1; d->eval(); };
    d->cfg_8580 = is8580 ? 3 : 0;
    d->cfg_res_high = 0;
    d->cfg_digi = 0;
    d->cfg_fc_sel = 0;                       // "Chip Model"
    d->cfg_auto = 0xf;                       // Resonance and Digis "Auto" for both UltiSIDs
    d->cfg_fc_base = 245 | (245 << 9);
    d->cfg_fc_off = (-780 & 0x7ff) | ((-780 & 0x7ff) << 11);
    d->cfg_bypass = 0;
    d->op_all_i = 0; d->op_valid_i = 0; d->start = 0; d->lv_addr = 0;
    d->rst = 1;
    for (int i = 0; i < 64; i++) tick();
    d->rst = 0;
    for (int i = 0; i < 4; i++) tick();

    long smp0 = 0, cyc = 0, ops = 0;
    auto pass = [&](bool have, bool w, unsigned r, unsigned v) {
        d->op_valid_i = have; d->op_we_i = w; d->op_sid_i = 0; d->op_reg_i = r & 0x1f;
        d->op_wdata_i = v; d->op_rdata_i = 0;
        d->start = 1;
        for (int k = 0; k < 64; k++) {
            tick();
            d->start = 0;
            if (d->smp_valid && d->smp_sid == 0) smp0 = (int32_t)(d->smp_data << 12) >> 12;
        }
        d->op_valid_i = 0;
        fprintf(of, "%ld\n", smp0);
        cyc++;
    };
    char line[256];
    while (cyc < ncyc && fgets(line, sizeof line, lf)) {
        long idle; char rw; unsigned reg, val = 0;
        if (sscanf(line, "%ld %c %x %x", &idle, &rw, &reg, &val) < 3) continue;
        for (long i = 0; i < idle && cyc < ncyc; i++) pass(false, false, 0, 0);
        if (cyc >= ncyc) break;
        pass(true, rw == 'W' || rw == 'w', reg, val);
        ops++;
    }
    while (cyc < ncyc) pass(false, false, 0, 0);
    fclose(lf); fclose(of);
    printf("tb_cmp %s %s: %ld cycles, %ld ops -> %s\n", argv[1], is8580 ? "8580" : "6581", cyc, ops, argv[3]);
    delete d;
    return 0;
}
