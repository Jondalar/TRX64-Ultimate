// SPDX-License-Identifier: GPL-3.0-or-later
// T25.1 M2/M3 bench: trx_sid_engine (Verilator) against the reference model
// (cores/trx_sid/model/sidref.cpp), cycle by cycle.
//
//   tb_sid <memdir> <curvedir> <clocks/cycle> <log0> [<log1> .. <log7>]
// Log n drives SID n (one log: SID 0 only). One CPU op per C64 cycle in all:
// ops due in the same cycle wait, in the same way for both sides.
// UltiSID 1 (SIDs 0-3): 6581 waves, low resonance, digi Medium, curve 2 (6581).
// UltiSID 2 (SIDs 4-7): 8580 waves, high resonance, digi High, curve 0 (8580 Lo).
// Compared after every pass: each SID's sample, OSC3/ENV3/bus read-back,
// the 24 envelope levels, every read result, the UltiSID sums.
#include "Vtrx_sid_engine.h"
#include "Vtrx_sid_engine___024root.h"
#include "sidref.h"
#include "verilated.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

struct Op {
    long due;
    bool we;
    unsigned reg, val;
};

static std::deque<Op> load_log(const char* p)
{
    std::deque<Op> q;
    FILE* f = fopen(p, "r");
    if (!f) {
        fprintf(stderr, "cannot open %s\n", p);
        exit(2);
    }
    char line[256];
    long c = 0;
    while (fgets(line, sizeof line, f)) {
        char* h = strchr(line, '#');
        if (h) *h = 0;
        long idle;
        char op;
        unsigned r, v = 0;
        if (sscanf(line, "%ld %c %x %x", &idle, &op, &r, &v) < 2) continue;
        c += idle;
        q.push_back({c, op == 'W', r & 31, v & 0xff});
        c++;
    }
    fclose(f);
    return q;
}

static bool load_curve(const std::string& p, uint16_t* c)
{
    FILE* f = fopen(p.c_str(), "r");
    if (!f) return false;
    char line[512];
    int i = 0;
    while (i < 1024 && fgets(line, sizeof line, f))
        if (line[0] != '/' && line[0] != '\n') c[i++] = uint16_t(strtoul(line, nullptr, 16));
    fclose(f);
    return i == 1024;
}

int main(int argc, char** argv)
{
    Verilated::commandArgs(argc, argv);
    if (argc < 5) {
        fprintf(stderr, "usage: tb_sid memdir curvedir clocks log0 [log1..log7]\n");
        return 2;
    }
    const int cpc = atoi(argv[3]);
    const int nsid = argc - 4;
    static trxsid::Tables tab;
    if (!tab.load(argv[1])) return 2;
    static uint16_t curve[2][1024];
    if (!load_curve(std::string(argv[2]) + "/curve2.mem", curve[0]) ||
        !load_curve(std::string(argv[2]) + "/curve0.mem", curve[1]))
        return 2;
    std::vector<trxsid::Sid*> m;
    std::vector<std::deque<Op>> logs;
    for (int s = 0; s < 8; s++) {
        trxsid::Sid* x = new trxsid::Sid(&tab);
        int u = s >> 2;
        x->cfg.is8580 = u == 1;
        x->cfg.res_high = u == 1;
        x->cfg.digi = u ? 3 : 2;
        x->cfg.curve = curve[u];
        x->reset();
        m.push_back(x);
        logs.push_back(s < nsid ? load_log(argv[4 + s]) : std::deque<Op>());
    }

    Vtrx_sid_engine* d = new Vtrx_sid_engine;
    auto tick = [&]() {
        d->clk = 0;
        d->cv_clk = 0;
        d->eval();
        d->clk = 1;
        d->cv_clk = 1;
        d->eval();
    };
    d->cfg_8580 = 2;
    d->cfg_res_high = 2;
    d->cfg_digi = (3 << 2) | 2;
    d->start = 0;
    d->op_valid_i = 0;
    d->st_we = 0;
    d->st_addr = 0;
    d->rst = 1;
    tick();
    d->rst = 0;
    for (int i = 0; i < 40; i++) tick();
    // the app writes both curves, byte by byte
    for (int u = 0; u < 2; u++)
        for (int i = 0; i < 1024; i++)
            for (int b = 0; b < 2; b++) {
                d->cv_we = 1;
                d->cv_addr = (u << 11) | (i << 1) | b;
                d->cv_wdata = (curve[u][i] >> (8 * b)) & 0xff;
                tick();
            }
    d->cv_we = 0;

    long cyc = 0, nerr = 0, nops = 0;
    const int maxerr = getenv("MAXERR") ? atoi(getenv("MAXERR")) : 10;
    // voice state bit b of entry vi: vsw holds bits 0-92, vsa 93-217
    auto field = [&](int vi, int off, int w) {
        auto& xw = d->rootp->trx_sid_engine__DOT__vsw[vi];
        auto& xa = d->rootp->trx_sid_engine__DOT__vsa[vi];
        unsigned long r = 0;
        for (int i = 0; i < w; i++) {
            int b = off + i;
            unsigned bit = b < 93 ? (xw[b / 32] >> (b % 32)) & 1 : (xa[(b - 93) / 32] >> ((b - 93) % 32)) & 1;
            r |= (unsigned long)bit << i;
        }
        return r;
    };
    auto err = [&](const char* what, int s, long a, long b) {
        if (nerr++ < maxerr) {
            printf("cycle %ld sid %d %s: rtl %lx model %lx\n", cyc, s, what, a, b);
            if (s >= 0 && s < 8 && nerr <= 2)
                for (int v = 0; v < 3; v++)
                    printf("   v%d acc %06lx/%06x wav %03lx/%03x env %02lx/%02x sr %06lx/%06x nout %03lx/%03x\n", v,
                           field(s * 4 + v, 0, 24), m[s]->w[v].acc, field(s * 4 + v, 60, 12), m[s]->w[v].wav_out,
                           field(s * 4 + v, 202, 8), m[s]->e[v].counter, field(s * 4 + v, 24, 23), m[s]->w[v].sr,
                           field(s * 4 + v, 47, 12), m[s]->w[v].noise_out);
        }
    };
    long hw_sum[2] = {0, 0};
    for (;;) {
        bool more = false;
        for (auto& q : logs) more |= !q.empty();
        if (!more) break;
        // this C64 cycle's op: the first SID whose next op is due
        int os = -1;
        for (int s = 0; s < 8; s++)
            if (!logs[s].empty() && logs[s].front().due <= cyc) {
                os = s;
                break;
            }
        // ops that wait slip by one cycle, together with the rest of their log
        for (int s = 0; s < 8; s++)
            if (s != os && !logs[s].empty() && logs[s].front().due <= cyc)
                for (auto& o : logs[s]) o.due++;
        Op op = {0, false, 0, 0};
        unsigned hw_rd = 0;
        if (os >= 0) {
            op = logs[os].front();
            logs[os].pop_front();
            nops++;
            if (!op.we) {
                unsigned mr = m[os]->read(op.reg);
                d->rb_sid = os;
                d->eval();
                switch (op.reg) {
                case 0x19: case 0x1a: hw_rd = 0xff; break;
                case 0x1b: hw_rd = d->rb_osc3; break;
                case 0x1c: hw_rd = d->rb_env3; break;
                default: hw_rd = d->rb_bus; break;
                }
                if (hw_rd != mr) err("read", os, hw_rd, mr);
            } else {
                m[os]->write(op.reg, uint8_t(op.val));
            }
        }
        // pass: the op is latched at the end of the cycle
        d->op_valid_i = os >= 0;
        d->op_we_i = op.we;
        d->op_sid_i = os >= 0 ? os : 0;
        d->op_reg_i = op.reg;
        d->op_wdata_i = op.val;
        d->op_rdata_i = hw_rd;
        d->start = 1;
        long smp[8];
        int nsmp = 0;
        bool sumv = false;
        for (int c = 0; c < cpc; c++) {
            tick();
            d->start = 0;
            if (d->smp_valid) {
                smp[d->smp_sid] = (long)((int32_t)(d->smp_data << 12) >> 12);
                nsmp++;
            }
            if (d->sum_valid) {
                sumv = true;
                hw_sum[0] = (long)((int32_t)(d->sum_u0 << 12) >> 12);
                hw_sum[1] = (long)((int32_t)(d->sum_u1 << 12) >> 12);
            }
        }
        cyc++;
        for (int s = 0; s < 8; s++) m[s]->clock();
        if (nsmp != 8 || !sumv) err("pass incomplete", -1, nsmp, 8);
        long ms[2] = {0, 0};
        for (int s = 0; s < 8; s++) {
            if (smp[s] != m[s]->out) err("sample", s, smp[s], m[s]->out);
            ms[s >> 2] += m[s]->out;
            d->rb_sid = s;
            d->eval();
            if (d->rb_osc3 != (m[s]->w[2].osc3 >> 4)) err("osc3", s, d->rb_osc3, m[s]->w[2].osc3 >> 4);
            if (d->rb_env3 != m[s]->e[2].env3) err("env3", s, d->rb_env3, m[s]->e[2].env3);
            if (d->rb_bus != m[s]->bus_value) err("bus", s, d->rb_bus, m[s]->bus_value);
            for (int v = 0; v < 3; v++) {
                if (field(s * 4 + v, 0, 24) != m[s]->w[v].acc) err("acc", s, field(s * 4 + v, 0, 24), m[s]->w[v].acc);
                if (field(s * 4 + v, 24, 23) != m[s]->w[v].sr) err("sr", s, field(s * 4 + v, 24, 23), m[s]->w[v].sr);
                if (field(s * 4 + v, 60, 12) != m[s]->w[v].wav_out) err("wav", s, field(s * 4 + v, 60, 12), m[s]->w[v].wav_out);
                d->lv_addr = s * 4 + v;
                d->eval();
                if (d->lv_data != m[s]->e[v].counter) err("level", s * 4 + v, d->lv_data, m[s]->e[v].counter);
            }
        }
        for (int u = 0; u < 2; u++) {
            long x = ms[u] > 0x7ffff ? 0x7ffff : ms[u] < -0x80000 ? -0x80000 : ms[u];
            if (x != hw_sum[u]) err("sum", u, hw_sum[u], x);
        }
        if (nerr > maxerr) break;
    }
    // state port: every word reads what the model holds; a write lands where it is read
    if (nerr == 0) {
        for (int e = 0; e < 32; e++) {
            if ((e & 3) == 3 || e / 4 >= 8) continue;
            for (int w = 0; w < 7; w++) {
                d->st_addr = e * 8 + w;
                d->eval();
                unsigned long x = 0;
                for (int i = 0; i < 32 && w * 32 + i < 218; i++) x |= (unsigned long)((field(e, w * 32 + i, 1))) << i;
                if (d->st_rdata != x) err("st read", e, d->st_rdata, x);
            }
        }
        for (int s = 0; s < 8; s++) {
            d->st_addr = 0x181 + 4 * s; d->eval();
            if ((int32_t)d->st_rdata != m[s]->vbp) err("st vbp", s, d->st_rdata, m[s]->vbp);
            d->st_addr = 0x1a0 + s; d->eval();
            if ((d->st_rdata & 0xff) != m[s]->bus_value) err("st bus", s, d->st_rdata, m[s]->bus_value);
            d->st_addr = 0x100 + 2 * (s * 4 + 2); d->eval();
            if ((d->st_rdata >> 16 & 0xff) != m[s]->reg[0x12]) err("st ctrl", s, d->st_rdata >> 16 & 0xff, m[s]->reg[0x12]);
        }
        d->st_addr = 5 * 8 + 0; d->st_wdata = 0x00123456; d->st_we = 1; tick(); d->st_we = 0;
        d->st_addr = 0x180 + 4 * 6; d->st_wdata = 0x1f2a7c05; d->st_we = 1; tick(); d->st_we = 0;
        d->st_addr = 5 * 8 + 0; d->eval();
        if (d->st_rdata != 0x00123456 || field(5, 0, 24) != 0x123456) err("st write", 5, d->st_rdata, 0x123456);
        d->st_addr = 0x180 + 4 * 6; d->eval();
        if (d->st_rdata != 0x1f2a7c05) err("st write filt", 6, d->st_rdata, 0x1f2a7c05);
    }
    printf("tb_sid %d SID(s), %d clocks/cycle: %ld cycles, %ld ops, %ld mismatches\n", nsid, cpc, cyc, nops, nerr);
    delete d;
    return nerr ? 1 : 0;
}
