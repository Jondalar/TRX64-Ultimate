// SPDX-License-Identifier: GPL-3.0-or-later
// T25.6: the app's path to the sampler through the real trx_io page 8 (three unrelated clocks: cpu 100 MHz, clk
// 31.5 MHz, clk2x 63 MHz): version / status reads, the app's own clear (64 zero bytes), a voice played from the app
// alone (SAMPLER_BASE bytes) and the C64 reading its status.
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <vector>
#include "Vsm_io_top.h"
#include "verilated.h"

static Vsm_io_top *d;
static uint8_t *ddr;
static double t_cpu = 0, t_clk = 3, t_2x = 1.5;            // ns of the next rising edges
static const double P_CPU = 10.0, P_CLK = 31.7, P_2X = 15.85;
static int fails = 0, checks = 0;
static int resp = -1; static uint8_t last_req = 0;
static std::vector<int> outl;
static uint64_t n2x = 0, ncpu = 0;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void step()
{
    double t = std::min(t_cpu, std::min(t_clk, t_2x));
    bool c0 = t_cpu == t, c1 = t_clk == t, c2 = t_2x == t;
    if (c0) { d->cpu_clk = 1; t_cpu += P_CPU; ncpu++; }
    if (c1) { d->clk = 1; t_clk += P_CLK; }
    if (c2) { d->clk2x = 1; t_2x += P_2X; n2x++; }
    d->eval();
    d->cpu_clk = 0; d->clk = 0; d->clk2x = 0; d->eval();
    if (c1) {
        // DDR model, 14 clk
        if (resp < 0 && d->f_req_tgl != last_req) {
            last_req = d->f_req_tgl; resp = 14;
            uint32_t a = d->f_addr & ~3u;
            d->f_data = ddr[a] | ddr[a + 1] << 8 | ddr[a + 2] << 16 | (uint32_t)ddr[a + 3] << 24;
        } else if (resp > 0) resp--;
        if (resp == 0) { d->f_done_tgl = !d->f_done_tgl; resp = -1; }
        static int div = 0;
        d->io_ev = 0;
        if (++div == 32) { div = 0; d->io_ev = 1; }
    }
    if (c2 && (n2x % 64) == 0) outl.push_back((int32_t)(d->smp_l << 9) >> 9);
}
static uint8_t app(uint32_t off, bool w, uint8_t v = 0)
{
    d->addr = 0x8000 + off; d->wdata = v;
    d->we = w; d->re = !w;
    uint64_t n0 = ncpu;
    while (ncpu == n0) step();                      // one cpu clock
    d->we = 0; d->re = 0;
    for (int i = 0; i < 20000 && !d->ack; i++) step();
    CHECK(d->ack, "no ack for %04x", off);
    uint8_t r = d->rdata;
    step(); step();
    return r;
}
static void run_us(double us) { double e = t_cpu + us * 1000; while (t_cpu < e) step(); }

int main(int argc, char **argv)
{
    Verilated::commandArgs(argc, argv);
    ddr = (uint8_t *)calloc(1, 128u << 20);
    d = new Vsm_io_top;
    d->rst = 1; d->c64_en = 1; d->we = d->re = 0; d->addr = 0; d->bus_addr = 0; d->io_ev_io = 0; d->f_done_tgl = 0;
    for (int i = 0; i < 100; i++) step();
    d->rst = 0;
    run_us(2);
    CHECK(app(0xE1, false) == 0x01, "version through trx_io");
    CHECK(app(0xE0, false) == 0x00, "status 0");
    for (int i = 0; i < 64; i++) app(i, true, 0);                      // clear_sampler_registers
    for (int v = 0; v < 8; v++) app(32 * v, true, 0);                  // Sampler::reset
    for (int i = 0; i < 16; i++) ddr[0x1002000 + i] = (uint8_t)(20 + 7 * i);
    // voice 7 from the app only: START 0x01002000, length 16, rate 150, volume 63, pan 15, ctrl 5 (enable + IRQ)
    const uint32_t b = 0xE0;
    app(b + 1, true, 63); app(b + 2, true, 15);
    app(b + 4, true, 0x01); app(b + 5, true, 0x00); app(b + 6, true, 0x20); app(b + 7, true, 0x00);
    app(b + 9, true, 0); app(b + 10, true, 0); app(b + 11, true, 16);
    app(b + 14, true, 0); app(b + 15, true, 150);
    outl.clear();
    app(b + 0, true, 0x05);
    run_us(600);
    // 16 distinct nonzero samples, left only
    std::vector<int> g;
    // the sub-cycle blend puts values between two samples out for one cycle at a step: only the samples themselves count
    auto exp_v = [](size_t i) { return (((int8_t)(20 + 7 * i)) * 256 * 63 * 166) >> 11; };
    auto is_smp = [&](int x) { for (size_t i = 0; i < 16; i++) if (x == exp_v(i)) return true; return false; };
    for (int x : outl) if (x != 0 && is_smp(x) && (g.empty() || g.back() != x)) g.push_back(x);
    bool ok = g.size() == 16;
    for (size_t i = 0; i < g.size() && i < 16; i++) ok = ok && g[i] == (((int8_t)(20 + 7 * i)) * 256 * 63 * 166) >> 11;
    CHECK(ok, "voice 7 played from the app alone (%zu samples)", g.size());
    CHECK(app(0xE0, false) == 0x01 && d->irq == 1, "status 1 and the IRQ line");
    // the C64 reads the status of voice 7? no: the window has 7 voices; voice 6 is $DFE0. Voice 7 is not visible
    d->bus_addr = 0xDF20; for (int i = 0; i < 8; i++) step();
    CHECK(d->oe && d->dout == 0, "C64 reads voice 0's status");
    app(0xFF, true, 0xFF);
    CHECK(app(0xE0, false) == 0x00, "acknowledged");
    printf("%d checks, %d failed\n", checks, fails);
    printf(fails ? "tb_sm_io FAIL\n" : "tb_sm_io PASS\n");
    return fails ? 1 : 0;
}
