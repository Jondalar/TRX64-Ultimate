// SPDX-License-Identifier: GPL-3.0-or-later
// T25.4 bench: trx_sid (the app's address decode, split, op latch, read-back copy,
// pass start on clk2x) with sid_multi behind it (-DSID_MULTI -DSID_MULTI_EQUIV)
// against the unmodified reDIP. Random bases / masks / splits as the app sets them
// (fix_splits clears the split bits), random accesses in $D400-$D7FF.
//   tb_trx gen <seed> <cycles> <accfile> <opsfile>   decode in C++ (spec T25.1 §3.1), writes the
//                                                    access list and the same accesses as SID ops
//   tb_trx run <accfile> <model UltiSID 1> <model UltiSID 2> <refprefix>
//                                                    RTL against <refprefix>.<pair> (tb_ref streams)
#include "Vtrx_sid.h"
#include "verilated.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

struct Cfg { unsigned base[2], mask[2], split; };

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

static void new_cfg(std::mt19937& rnd, Cfg& cfg)
{
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
}

int main(int argc, char** argv)
{
    Verilated::commandArgs(argc, argv);
    if (argc < 2) return 2;
    if (!strcmp(argv[1], "gen")) {
        if (argc < 6) return 2;
        std::mt19937 rnd(atoi(argv[2]));
        const long ncyc = atol(argv[3]);
        FILE* fa = fopen(argv[4], "w");
        FILE* fo = fopen(argv[5], "w");
        Cfg cfg;
        for (long cyc = 0; cyc < ncyc; cyc++) {
            if (cyc % 20000 == 0) {
                new_cfg(rnd, cfg);
                fprintf(fa, "C %ld %02x %02x %02x %02x %d\n", cyc, cfg.base[0], cfg.base[1], cfg.mask[0], cfg.mask[1], cfg.split);
            }
            bool acc = rnd() % 4 != 0;
            unsigned addr = rnd() % 1024;
            if (rnd() % 2) addr = (addr & ~0x1f) | (rnd() % 0x19);
            bool we = acc && rnd() % 5 != 0;
            unsigned val = rnd() % 256;
            if (we && (addr & 0x1f) % 7 == 4 && (addr & 0x1f) < 21) val = (val & 0x0f) | ((1 << (rnd() % 4)) << 4);
            if (we && (addr & 0x1f) == 0x18) val |= 0x0f;
            if (!acc) continue;
            // no POT reads: the wrapper owns POT (its counter starts at FF, reDIP's at 0)
            if (!we && ((addr & 31) == 0x19 || (addr & 31) == 0x1a)) addr = (addr & ~31u) | 0x1b;
            int s = decode(cfg, addr);
            unsigned reg = addr & 31;
            bool pot = !we && (reg == 0x19 || reg == 0x1a);
            fprintf(fa, "A %ld %03x %c %02x %d %d\n", cyc, addr, we ? 'W' : 'R', val, s, pot);
            if (s >= 0 && !pot) fprintf(fo, "%ld %d %c %02x %02x\n", cyc, s, we ? 'W' : 'R', reg, we ? val : 0);
        }
        fprintf(fo, "%ld\n", ncyc + 4);
        fclose(fa);
        fclose(fo);
        return 0;
    }
    // run
    if (argc < 6) return 2;
    const int m[2] = {atoi(argv[3]) == 8580, atoi(argv[4]) == 8580};
    struct A { long cyc; unsigned addr; bool we; unsigned val; int sid; bool pot; };
    struct C { long cyc; unsigned b0, b1, m0, m1, split; };
    std::vector<A> acc;
    std::vector<C> cfgs;
    long total = 0;
    {
        FILE* f = fopen(argv[2], "r");
        if (!f) return 2;
        char line[256];
        while (fgets(line, sizeof line, f)) {
            if (line[0] == 'C') {
                C c;
                sscanf(line, "C %ld %x %x %x %x %u", &c.cyc, &c.b0, &c.b1, &c.m0, &c.m1, &c.split);
                cfgs.push_back(c);
            } else if (line[0] == 'A') {
                A a; char t; int pot;
                sscanf(line, "A %ld %x %c %x %d %d", &a.cyc, &a.addr, &t, &a.val, &a.sid, &pot);
                a.we = t == 'W';
                a.pot = pot;
                acc.push_back(a);
                total = a.cyc + 1;
            }
        }
        fclose(f);
    }
    struct Ref { long smp[2]; int rd; };
    std::vector<std::vector<Ref>> ref(4);
    for (int p = 0; p < 4; p++) {
        char name[512];
        snprintf(name, sizeof name, "%s.%d", argv[5], p);
        FILE* r = fopen(name, "r");
        if (!r) { fprintf(stderr, "cannot open %s\n", name); return 2; }
        long n; unsigned a, b; int rd;
        while (fscanf(r, "%ld %x %x %d", &n, &a, &b, &rd) == 4) ref[p].push_back({{(long)a, (long)b}, rd});
        fclose(r);
    }
    if (ref[0].size() < (size_t)total) total = ref[0].size() - 2;
    Vtrx_sid* d = new Vtrx_sid;
    long t2 = 0;
    auto tick2 = [&]() {
        bool ce = (t2 & 1) == 0;
        d->clk2x = 0; d->cv_clk = 0;
        if (ce) d->clk = 0;
        d->eval();
        d->clk2x = 1; d->cv_clk = 1;
        if (ce) d->clk = 1;
        d->eval();
        t2++;
    };
    d->cfg_8580 = m[0] | (m[1] << 1);
    d->cfg_res_high = d->cfg_8580;
    d->cfg_digi = (m[0] ? 0 : 3) | ((m[1] ? 0 : 3) << 2);
    d->cen = 1; d->phi2_n = 0; d->pot_x = 0; d->pot_y = 0; d->cv_we = 0; d->lv_addr = 0;
    d->cfg_base = 0x0101; d->cfg_mask = 0xfefe; d->cfg_split = 0;
    d->rst = 1;
    for (int i = 0; i < 8; i++) tick2();
    d->rst = 0;
    for (int i = 0; i < 100; i++) tick2();
    while (t2 % 64) tick2();
    size_t ia = 0, ic = 0;
    long nerr = 0, nhit = 0, nrd = 0, nsmpc = 0;
    const int maxerr = getenv("MAXERR") ? atoi(getenv("MAXERR")) : 10;
    long smp[8] = {0};
    int nsmp = 0;
    for (long cyc = 0; cyc < total; cyc++) {
        if (ic < cfgs.size() && cfgs[ic].cyc == cyc) {
            d->cfg_base = cfgs[ic].b1 << 8 | cfgs[ic].b0;
            d->cfg_mask = cfgs[ic].m1 << 8 | cfgs[ic].m0;
            d->cfg_split = cfgs[ic].split;
            ic++;
        }
        bool have = ia < acc.size() && acc[ia].cyc == cyc;
        A a = {0, 0, false, 0, -1, false};
        if (have) a = acc[ia++];
        d->cen = !have;
        d->we = have && a.we;
        d->addr = a.addr;
        d->din = a.val;
        for (int c = 0; c < 64; c++) {
            if (c == 62) d->phi2_n = 1;
            if (c == 62) {
                d->eval();
                if (have && bool(d->hit) != (a.sid >= 0)) {
                    if (nerr++ < maxerr) printf("cycle %ld addr %03x hit rtl %d model %d\n", cyc, a.addr, d->hit, a.sid);
                }
                if (have && a.sid >= 0) {
                    nhit++;
                    if (!a.we && !a.pot) {
                        nrd++;
                        int want = ref[a.sid / 2][cyc].rd;
                        if ((int)d->dout != want && nerr++ < maxerr)
                            printf("cycle %ld addr %03x sid %d read rtl %02x ref %02x\n", cyc, a.addr, a.sid, d->dout, want);
                    }
                }
            }
            tick2();
            if (c == 63) d->phi2_n = 0;
            if (d->smp_valid) { smp[d->smp_sid] = (int32_t)(d->smp_data << 12) >> 12; nsmp++; }
        }
        if (cyc >= 3) {
            if (nsmp != 8 && nerr++ < maxerr) printf("cycle %ld: %d samples\n", cyc, nsmp);
            for (int s = 0; s < 8; s++) {
                nsmpc++;
                if ((smp[s] & 0xfffff) != ref[s / 2][cyc].smp[s & 1] && nerr++ < maxerr)
                    printf("cycle %ld sid %d sample rtl %05lx ref %05lx\n", cyc, s, smp[s] & 0xfffff, ref[s / 2][cyc].smp[s & 1]);
            }
        }
        nsmp = 0;
        if (nerr > maxerr) break;
    }
    printf("%s tb_trx: %ld cycles, %ld hits, %ld reads, %ld sample compares, %ld mismatches\n", nerr ? "FAIL" : "PASS", total, nhit, nrd, nsmpc, nerr);
    return nerr ? 1 : 0;
}
