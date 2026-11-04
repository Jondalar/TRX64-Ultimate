// SPDX-License-Identifier: GPL-3.0-or-later
// T25.6: step timing of the sampler. One voice loops a sine (8 bit, period 50 samples, 1000 samples long) at a given
// rate; the output of every C64 cycle (left) goes to a text file for spur.py, which compares it with the ideal
// time average of a zero order hold at the exact 6.25 MHz / rate step grid.
//   tb_spur rate ncycles outfile [vmode] [dc: 0 = sine, 1 = two more voices running (collisions)]
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cmath>
#include <deque>
#include "Vtrx_sampler.h"
#include "verilated.h"

static Vtrx_sampler *d;
static uint8_t *ddr;
static int ev_div = 0, resp_cnt = -1;
static uint8_t last_req = 0;
static uint64_t cycles = 0;
struct Wr { uint16_t a; uint8_t v; };
static std::deque<Wr> wrq;
static FILE *fo = nullptr;
static bool logging = false;
static int sx(int v) { return (v << 9) >> 9; }
static void tick()
{
    bool ev = false;
    d->io_ev = 0; d->io_ev_io = 0; d->io_ev_we = 0;
    if (++ev_div == 32) {
        ev_div = 0; ev = true; cycles++;
        d->io_ev = 1;
        if (!wrq.empty()) { d->io_ev_io = 1; d->io_ev_we = 1; d->io_ev_addr = wrq.front().a; d->io_ev_data = wrq.front().v; wrq.pop_front(); }
    }
    d->clk = 1; d->clk2x = 1; d->eval();
    d->clk2x = 0; d->eval();
    d->clk = 0; d->clk2x = 1; d->eval();
    d->clk2x = 0; d->eval();
    if (resp_cnt < 0 && d->f_req_tgl != last_req) {
        last_req = d->f_req_tgl; resp_cnt = 14;
        uint32_t a = d->f_addr & ~3u;
        d->f_data = ddr[a] | ddr[a + 1] << 8 | ddr[a + 2] << 16 | (uint32_t)ddr[a + 3] << 24;
    } else if (resp_cnt > 0) resp_cnt--;
    if (resp_cnt == 0) { d->f_done_tgl = !d->f_done_tgl; resp_cnt = -1; }
    if (ev && logging) fprintf(fo, "%d\n", sx(d->smp_l));
}
static void run(uint64_t c) { uint64_t e = cycles + c; while (cycles < e) tick(); }
static void wr(int v, int off, uint8_t val) { wrq.push_back({(uint16_t)(0xDF20 + 32 * v + off), val}); while (!wrq.empty()) tick(); run(2); }
static const uint32_t REU = 0x1000000;
static void setup(int v, uint32_t start, uint32_t len, int rate, int vol)
{
    wr(v, 0, 0); wr(v, 1, vol); wr(v, 2, 15);
    for (int i = 0; i < 4; i++) wr(v, 4 + i, start >> (24 - 8 * i));
    for (int i = 0; i < 3; i++) wr(v, 9 + i, len >> (16 - 8 * i));
    wr(v, 0xE, rate >> 8); wr(v, 0xF, rate);
}
int main(int argc, char **argv)
{
    Verilated::commandArgs(argc, argv);
    if (argc < 4) { fprintf(stderr, "tb_spur rate ncycles outfile [vmode] [extra voices] [their rate]\n"); return 2; }
    int rate = atoi(argv[1]); long n = atol(argv[2]); int vmode = argc > 4 ? atoi(argv[4]) : 0; int extra = argc > 5 ? atoi(argv[5]) : 0; int erate = argc > 6 ? atoi(argv[6]) : 0;
    fo = fopen(argv[3], "w");
    ddr = (uint8_t *)calloc(1, 128u << 20);
    for (int i = 0; i < 1000; i++) ddr[REU + 0x100 + i] = (uint8_t)(int8_t)lrint(120 * sin(2 * M_PI * i / 50.0));
    for (int i = 0; i < 1000; i++) ddr[REU + 0x2000 + i] = (uint8_t)(int8_t)lrint(100 * sin(2 * M_PI * i / 33.0 + 1));
    d = new Vtrx_sampler;
    d->rst = 1; d->vmode = vmode; d->c64_en = 1; d->bus_addr = 0; d->a_tgl = 0; d->a_we = 0; d->a_addr = 0; d->a_wdata = 0; d->f_done_tgl = 0; d->f_data = 0;
    for (int i = 0; i < 64; i++) tick();
    d->rst = 0; run(4);
    // the voices that only load the engine are silent (volume 0) and play from another place at other rates
    for (int e = 1; e <= extra; e++) { setup(e, REU + 0x2000, 1000, erate ? erate + 7 * e : 11 + 3 * e, 0); wr(e, 0, 0x03); }
    setup(0, REU + 0x100, 1000, rate, 63);
    wr(0, 0, 0x03);
    logging = true;
    run(n);
    fclose(fo);
    return 0;
}
