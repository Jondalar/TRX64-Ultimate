// SPDX-License-Identifier: GPL-3.0-or-later
// T25.1 M1: compare the reference model's digital part with libresidfp
// (GPL-2.0-or-later, pinned checkout, tools/fetch_libresidfp.sh) cycle by
// cycle on a register-op log.
//
//   compare_resid <memdir> <6581|8580> <log> [maxerr]
//
// Log lines: "<idle> W <reg> <val>" or "<idle> R <reg>" (hex reg/val): run
// <idle> cycles, then the op in the next cycle. "#" starts a comment.
// Compared after every cycle: accumulator, shift register, waveform output
// (bits 11..4 when a combined table is in use), envelope counter, OSC3, ENV3,
// bus value, and every read result.
#define private public
#define protected public
#include "SID.h"
#undef private
#undef protected
#include "sidref.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

int main(int argc, char** argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: compare_resid memdir 6581|8580 log [maxerr]\n");
        return 2;
    }
    static trxsid::Tables tab;
    if (!tab.load(argv[1])) {
        fprintf(stderr, "cannot load tables from %s\n", argv[1]);
        return 2;
    }
    const bool is8580 = strcmp(argv[2], "8580") == 0;
    const int maxerr = argc > 4 ? atoi(argv[4]) : 10;
    FILE* f = fopen(argv[3], "r");
    if (!f) return 2;

    reSIDfp::SID rs;
    rs.setChipModel(is8580 ? reSIDfp::CSG8580 : reSIDfp::MOS6581);
    rs.reset();
    trxsid::Sid m(&tab);
    m.cfg.is8580 = is8580;
    m.reset();

    std::map<std::string, long> errs;
    long cyc = 0, nerr = 0, ops = 0;
    auto chk = [&](const char* what, int v, unsigned a, unsigned b) {
        if (a == b) return;
        std::string k = std::string(what);
        if (errs[k]++ < maxerr)
            printf("cycle %ld %s voice %d: model %x resid %x\n", cyc, what, v, a, b);
        nerr++;
    };
    // a floating DAC keeps the last output: compare it the way it was made
    unsigned last_mask[3] = {0xfff, 0xfff, 0xfff};
    auto clock1 = [&]() {
        rs.clockDigital(1);
        m.clock();
        cyc++;
        for (int v = 0; v < 3; v++) {
            reSIDfp::WaveformGenerator* w = rs.voice[v].wave();
            reSIDfp::EnvelopeGenerator* e = rs.voice[v].envelope();
            if (m.w[v].waveform != 0) last_mask[v] = m.table_used(v) ? 0xff0 : 0xfff;
            unsigned mask = last_mask[v];
            if (getenv("DBG") && m.w[v].acc != w->accumulator && errs["acc"] < 2)
                for (int u = 0; u < 3; u++) {
                    reSIDfp::WaveformGenerator* q = rs.voice[u].wave();
                    printf("  dbg acc v%d: test %d/%d sync %d/%d msbr %d/%d freq %x acc %06x/%06x\n", u, m.w[u].test,
                           q->test, m.w[u].sync, q->sync, m.w[u].msb_rising, q->msb_rising, q->freq, m.w[u].acc,
                           q->accumulator);
                }
            chk("acc", v, m.w[v].acc, w->accumulator);
            chk("sr", v, m.w[v].sr, w->shift_register);
            if (getenv("DBG") && (m.w[v].wav_out & mask) != (w->waveform_output & mask) && errs["wav"] < 3)
                printf("  dbg v%d wf %x acc %06x/%06x pulse %d/%x noise %03x/%03x\n", v, m.w[v].waveform, m.w[v].acc,
                       w->accumulator, m.w[v].pulse_out, w->pulse_output, m.w[v].noise_out, w->noise_output);
            chk("wav", v, m.w[v].wav_out & mask, w->waveform_output & mask);
            chk("env", v, m.e[v].counter, e->envelope_counter);
            if (getenv("DBG") && (m.w[v].osc3 >> 4) != (w->osc3 >> 4))
                printf("  dbg osc3 v%d wf %x tsp %03x/%03x acc %06x pulse %d/%x noise %03x/%03x wav %03x/%03x\n", v,
                       m.w[v].waveform, m.w[v].tsp, w->tri_saw_pipeline, m.w[v].acc, m.w[v].pulse_out,
                       w->pulse_output, m.w[v].noise_out, w->noise_output, m.w[v].wav_out, w->waveform_output);
            chk("osc3", v, m.w[v].osc3 >> 4, w->osc3 >> 4);
            chk("env3", v, m.e[v].env3, e->env3);
        }
        chk("bus", -1, m.bus_value, rs.busValue);
    };
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char* h = strchr(line, '#');
        if (h) *h = 0;
        long idle;
        char op;
        unsigned r, val = 0;
        int n = sscanf(line, "%ld %c %x %x", &idle, &op, &r, &val);
        if (n < 2) continue;
        for (long i = 0; i < idle; i++) clock1();
        if (op == 'W') {
            rs.write(r & 31, uint8_t(val));
            m.write(r & 31, uint8_t(val));
        } else if (op == 'R') {
            unsigned a = m.read(r & 31), b = rs.read(r & 31);
            chk("read", int(r), a, b);
        }
        ops++;
        clock1();
    }
    fclose(f);
    printf("%s %s: %ld cycles, %ld ops, %ld mismatches", argv[3], argv[2], cyc, ops, nerr);
    for (auto& k : errs) printf(" [%s %ld]", k.first.c_str(), k.second);
    printf("\n");
    return nerr ? 1 : 0;
}
