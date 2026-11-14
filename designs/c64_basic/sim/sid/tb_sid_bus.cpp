// SPDX-License-Identifier: GPL-3.0-or-later
// T25.1 M3 bench: trx_sid (bus decode, split, read path, op latch, pass start
// on clk2x) with 8 reference models behind a C++ copy of the spec's decode.
// Random UltiSID bases/masks/splits as the app sets them (fix_splits clears
// the split bits from base and mask), random accesses in $D400-$D7FF; every
// read compared at phi2_n, every SID's sample compared every pass.
//   tb_sid_bus <memdir> <curvedir> <cycles> [seed]
#include "Vtrx_sid.h"
#include "sidref.h"
#include "verilated.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>

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

struct Cfg {
    unsigned base[2], mask[2], split;
};

// spec §3.1
static int decode(const Cfg& c, unsigned addr)
{
    unsigned a = 0x40 | ((addr >> 4) & 0x3f);
    bool h0 = (a & c.mask[0]) == c.base[0], h1 = (a & c.mask[1]) == c.base[1];
    if (!h0 && !h1) return -1;
    unsigned p = 0;
    auto b = [&](int n) { return (addr >> n) & 1; };
    switch (c.split) {
    case 1: p = b(5); break;
    case 2: p = b(6); break;
    case 3: p = b(7); break;
    case 4: p = b(8); break;
    case 5: p = b(6) << 1 | b(5); break;
    case 6: p = b(8) << 1 | b(5); break;
    case 7: p = b(8) << 1 | b(7); break;
    default: break;
    }
    return (h0 ? 0 : 4) + int(p);
}

int main(int argc, char** argv)
{
    Verilated::commandArgs(argc, argv);
    if (argc < 4) return 2;
    const long ncyc = atol(argv[3]);
    std::mt19937 rnd(argc > 4 ? atoi(argv[4]) : 1);
    static trxsid::Tables tab;
    if (!tab.load(argv[1])) return 2;
    static int32_t opamp[1024];
    if (!trxsid::load_opamp((std::string(argv[1]) + "/sid_opamp6581.mem").c_str(), opamp)) return 2;
    static uint16_t curve[2][1024];
    if (!load_curve(std::string(argv[2]) + "/curve3.mem", curve[0]) ||
        !load_curve(std::string(argv[2]) + "/curve1.mem", curve[1]))
        return 2;
    trxsid::Sid* m[8];
    for (int s = 0; s < 8; s++) {
        m[s] = new trxsid::Sid(&tab);
        m[s]->cfg.is8580 = s >= 4;
        m[s]->cfg.res_high = s < 4;
        m[s]->cfg.digi = s < 4 ? 1 : 0;
        m[s]->cfg.curve = curve[s >> 2];
        m[s]->cfg.opamp = opamp;          // U12 D1 (a): used by the 6581s (SIDs 0-3)
        m[s]->reset();
    }
    Vtrx_sid* d = new Vtrx_sid;
    // clk2x ticks; clk on every other one
    long t2 = 0;
    // clk rises with every even clk2x edge (t2 counts clk2x edges, cycles are 64)
    auto tick2 = [&]() {
        bool ce = (t2 & 1) == 0;
        d->clk2x = 0;
        d->cv_clk = 0;
        if (ce) d->clk = 0;
        d->eval();
        d->clk2x = 1;
        d->cv_clk = 1;
        if (ce) d->clk = 1;
        d->eval();
        t2++;
    };
    d->cfg_8580 = 2;
    d->cfg_res_high = 1;
    d->cfg_digi = (0 << 2) | 1;
    d->cen = 1;
    d->phi2_n = 0;
    d->pot_x = 0;
    d->pot_y = 0;
    d->rst = 1;
    for (int i = 0; i < 8; i++) tick2();
    d->rst = 0;
    for (int i = 0; i < 100; i++) tick2();
    for (int u = 0; u < 2; u++)
        for (int i = 0; i < 2048; i++) {
            d->cv_we = 1;
            d->cv_addr = (u << 11) | i;
            d->cv_wdata = (curve[u][i >> 1] >> (8 * (i & 1))) & 0xff;
            tick2();
        }
    d->cv_we = 0;

    Cfg cfg;
    auto new_cfg = [&]() {
        cfg.split = rnd() % 8;
        static const unsigned sb[8] = {0, 0x02, 0x04, 0x08, 0x10, 0x06, 0x12, 0x18};
        for (int u = 0; u < 2; u++) {
            unsigned r = rnd() % 10;
            cfg.base[u] = r == 0 ? 0x01 : (0x40 | ((rnd() % 32) << 1));   // $D400..$D7E0 or off
            cfg.mask[u] = 0xfe;
            if (rnd() % 3 == 0) cfg.mask[u] &= ~(((rnd() % 32) << 1) & 0x3e);  // mirroring
            cfg.mask[u] &= ~sb[cfg.split];
            cfg.base[u] &= cfg.mask[u] | 0x01;
        }
        d->cfg_base = cfg.base[1] << 8 | cfg.base[0];
        d->cfg_mask = cfg.mask[1] << 8 | cfg.mask[0];
        d->cfg_split = cfg.split;
    };
    new_cfg();
    while (t2 % 64) tick2();

    long nerr = 0, nhit = 0, nrd = 0;
    const int maxerr = getenv("MAXERR") ? atoi(getenv("MAXERR")) : 10;
    long smp[8];
    int nsmp = 0;
    for (long cyc = 0; cyc < ncyc; cyc++) {
        if (cyc % 20000 == 19999) {
            new_cfg();
            // settles within the first clocks of the cycle
        }
        // this cycle's access (or none)
        bool acc = rnd() % 4 != 0;
        unsigned addr = rnd() % 1024;
        if (rnd() % 2) addr = (addr & ~0x1f) | (rnd() % 0x19);   // favour the real registers
        bool we = acc && rnd() % 5 != 0;
        unsigned val = rnd() % 256;
        if (we && (addr & 0x1f) % 7 == 4 && (addr & 0x1f) < 21) val = (val & 0x0f) | ((1 << (rnd() % 4)) << 4);
        if (we && (addr & 0x1f) == 0x18) val |= 0x0f;
        d->cen = !acc;
        d->we = we;
        d->addr = addr;
        d->din = val;
        // 32 clk ticks; phi2_n high in the last one (the edge that ends the cycle)
        for (int c = 0; c < 64; c++) {
            if (c == 62) d->phi2_n = 1;
            if (c == 62) {
                d->eval();
                int s = acc ? decode(cfg, addr) : -1;
                if (bool(d->hit) != (s >= 0) && acc) {
                    if (nerr++ < maxerr) printf("cycle %ld addr %03x hit rtl %d model %d\n", cyc, addr, d->hit, s);
                }
                if (s >= 0 && getenv("TRACE") && cyc > atol(getenv("TRACE")) - 2000)
                    printf("  c%ld sid %d %c %02x %02x\n", cyc, s, we ? 'W' : 'R', addr & 31, we ? val : 0);
                if (s >= 0) {
                    nhit++;
                    if (we) m[s]->write(addr & 31, uint8_t(val));
                    else {
                        unsigned mr = m[s]->read(addr & 31);
                        // POT: the RTL answers $D419/$D41A from its POT counters (0xFF without paddles)
                        nrd++;
                        if (d->dout != mr && nerr++ < maxerr)
                            printf("cycle %ld addr %03x sid %d read rtl %02x model %02x\n", cyc, addr, s, d->dout, mr);
                    }
                }
            }
            tick2();
            if (c == 63) d->phi2_n = 0;
            if (d->smp_valid) {
                smp[d->smp_sid] = (int32_t)(d->smp_data << 12) >> 12;
                nsmp++;
            }
        }
        // samples seen in this cycle come from the pass of the previous cycle's op
        if (cyc > 0) {
            if (nsmp != 8 && nerr++ < maxerr) printf("cycle %ld: %d samples\n", cyc, nsmp);
            for (int s = 0; s < 8; s++)
                if (smp[s] != m[s]->out && nerr++ < maxerr)
                    printf("cycle %ld sid %d sample rtl %lx model %x\n", cyc, s, smp[s], m[s]->out);
        }
        nsmp = 0;
        for (int s = 0; s < 8; s++) m[s]->clock();
        if (nerr > maxerr) break;
    }
    printf("tb_sid_bus: %ld cycles, %ld hits, %ld reads, %ld mismatches\n", ncyc, nhit, nrd, nerr);
    return nerr ? 1 : 0;
}
