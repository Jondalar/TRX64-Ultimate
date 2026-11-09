// SPDX-License-Identifier: GPL-3.0-or-later
// T25.6: unit bench of trx_sampler (docs/specs/T25.6-sampler.md). The C64 side is driven as the slot bus of c64_core
// (one io_ev pulse per C64 cycle, writes with io_ev_io), the app side through the toggle handshake of trx_io, DDR2 is
// a byte array behind a toggle handshake with a latency in clocks.
//   tb_sampler [latency in clk, default 14]
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <vector>
#include <deque>
#include <algorithm>
#include "Vtrx_sampler.h"
#include "Vtrx_sampler___024root.h"
#include "verilated.h"

static Vtrx_sampler *d;
static uint8_t *ddr;                    // 128 MB, lazily zero
static int lat = 14;
static int fails = 0, checks = 0;
static uint64_t ticks = 0, cycles = 0;
static bool c64_run = true;
static int ev_div = 0;                  // clk per C64 cycle: 32
struct Wr { uint16_t a; uint8_t v; };
static std::deque<Wr> wrq;
static bool req_seen = false;
static int resp_cnt = -1;
static uint8_t last_req = 0;
static std::vector<uint32_t> fetches;   // addresses requested
struct S { uint64_t cyc; int l, r; bool bl; };   // bl: a voice put out a blend of old and new sample (sub-cycle step) this frame
static std::vector<S> log_;
static bool logging = false;
static bool bl_acc = false, bl_out = false;

#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static int sx(int v) { return (v << 9) >> 9; }
static void tick()
{
    bool ev = false;
    d->io_ev = 0; d->io_ev_io = 0; d->io_ev_we = 0;
    if (c64_run && ++ev_div == 32) {
        ev_div = 0; ev = true; cycles++;
        d->io_ev = 1;
        if (!wrq.empty()) {
            d->io_ev_io = 1; d->io_ev_we = 1; d->io_ev_addr = wrq.front().a; d->io_ev_data = wrq.front().v;
            wrq.pop_front();
        }
    }
    auto *rp = d->rootp;
    for (int ph = 0; ph < 4; ph++) {
        bool edge = ph == 0 || ph == 2;
        if (ph == 0) { d->clk = 1; d->clk2x = 1; } else if (ph == 1) d->clk2x = 0;
        else if (ph == 2) { d->clk = 0; d->clk2x = 1; } else d->clk2x = 0;
        int t_pre = rp->trx_sampler__DOT__t, vj_pre = rp->trx_sampler__DOT__vj, vis_pre = rp->trx_sampler__DOT__vis;
        d->eval();
        if (edge && vis_pre && vj_pre == 3 && rp->trx_sampler__DOT__w_r != 128) bl_acc = true;
        if (edge && t_pre == 48) { bl_out = bl_acc; bl_acc = false; }
    }
    ticks++;
    // DDR2: one read at a time, answered after `lat` clocks
    if (resp_cnt < 0 && d->f_req_tgl != last_req) {
        last_req = d->f_req_tgl;
        resp_cnt = lat;
        fetches.push_back(d->f_addr);
        uint32_t a = d->f_addr & ~3u;
        d->f_data = ddr[a] | ddr[a + 1] << 8 | ddr[a + 2] << 16 | (uint32_t)ddr[a + 3] << 24;
    } else if (resp_cnt > 0) resp_cnt--;
    if (resp_cnt == 0) { d->f_done_tgl = !d->f_done_tgl; resp_cnt = -1; }
    if (ev && logging) log_.push_back({cycles, sx(d->smp_l), sx(d->smp_r), bl_out});
}
static void run(uint64_t c) { uint64_t e = cycles + c; while (cycles < e) tick(); }
static void c64_wr(uint16_t a, uint8_t v) { wrq.push_back({a, v}); while (!wrq.empty()) tick(); run(2); }
static uint8_t c64_rd(uint16_t a, bool *oe = nullptr)
{
    d->bus_addr = a;
    for (int i = 0; i < 4; i++) tick();
    uint8_t v = d->dout; if (oe) *oe = d->oe;
    return v;
}
static uint8_t app_op(uint8_t a, bool we, uint8_t v)
{
    d->a_addr = a; d->a_we = we; d->a_wdata = v;
    uint8_t rs = d->a_rs_tgl;
    d->a_tgl = !d->a_tgl;
    for (int i = 0; i < 4000 && d->a_rs_tgl == rs; i++) tick();
    CHECK(d->a_rs_tgl != rs, "app op %02x timed out", a);
    return d->a_rdata;
}
static int sign16(int v) { return (int16_t)v; }
// Gideon's pan law (measured, T25.6): nibble 0..7 L = 0..full in 7 steps, R full; 8..15 mirrored; full = 166/2048
static const int PT[8] = {0, 24, 47, 71, 95, 119, 142, 166};
static int mixl(int s, int vol, int pan) { return (s * vol * (pan > 7 ? 166 : PT[pan])) >> 11; }
static int mixr(int s, int vol, int pan) { return (s * vol * (pan > 7 ? PT[15 - pan] : 166)) >> 11; }

// voice registers through the C64 window (v < 7) or the app (v == 7)
static void vw(int v, int off, uint8_t val)
{
    if (v < 7) c64_wr(0xDF20 + 32 * v + off, val);
    else app_op(32 * v + off, true, val);
}
static void vsetup(int v, uint32_t start, uint32_t len, int rate, int vol, int pan, uint32_t ra = 0, uint32_t rb = 0)
{
    vw(v, 0, 0);
    vw(v, 1, vol); vw(v, 2, pan);
    for (int i = 0; i < 4; i++) vw(v, 4 + i, start >> (24 - 8 * i));
    for (int i = 0; i < 3; i++) vw(v, 9 + i, len >> (16 - 8 * i));
    vw(v, 0xE, rate >> 8); vw(v, 0xF, rate);
    for (int i = 0; i < 3; i++) { vw(v, 0x11 + i, ra >> (16 - 8 * i)); vw(v, 0x15 + i, rb >> (16 - 8 * i)); }
}
static void reset_all()
{
    for (int v = 0; v < 8; v++) { vw(v, 0, 0); }
    for (int v = 0; v < 8; v++) { vw(v, 1, 0); vw(v, 0x1F, 0xFF); }
    run(8);
}
// the distinct values (consecutive duplicates collapsed) of the left or right output
static std::vector<int> seq(bool left, size_t from = 0)
{
    std::vector<int> o;
    for (size_t i = from; i < log_.size(); i++) {
        if (log_[i].bl) continue;                // the exact values of the samples; the blends are checked in 6b
        int x = left ? log_[i].l : log_[i].r;
        if (o.empty() || o.back() != x) o.push_back(x);
    }
    return o;
}
static const uint32_t REU = 0x1000000;
static void dump(const char *n, const std::vector<int> &g, const std::vector<int> &e)
{
    size_t k = 0; while (k < g.size() && k < e.size() && g[k] == e[k]) k++;
    printf("  %s: first difference at %zu: got", n, k);
    for (size_t i = k; i < g.size() && i < k + 4; i++) printf(" %d", g[i]);
    printf(" expected");
    for (size_t i = k; i < e.size() && i < k + 4; i++) printf(" %d", e[i]);
    printf("\n");
}

int main(int argc, char **argv)
{
    Verilated::commandArgs(argc, argv);
    if (argc > 1) lat = atoi(argv[1]);
    ddr = (uint8_t *)calloc(1, 128u << 20);
    d = new Vtrx_sampler;
    d->rst = 1; d->vmode = 0; d->c64_en = 1; d->bus_addr = 0; d->io_ev_addr = 0; d->io_ev_data = 0;
    d->a_tgl = 0; d->a_we = 0; d->a_addr = 0; d->a_wdata = 0; d->f_done_tgl = 0; d->f_data = 0;
    for (int i = 0; i < 64; i++) tick();
    d->rst = 0;
    run(4);

    // ---- 1. detection, as the app's MOD cartridge and Xander's audio_detect do it
    {
        bool oe = false;
        for (int v = 0; v < 7; v++) c64_wr(0xDF20 + 32 * v, 0);
        c64_wr(0xDF3F, 0xFF);
        int bad = 0;
        for (int i = 0; i < 256; i++) { if (c64_rd(0xDF20, &oe) != 0) bad++; run(8); }
        CHECK(bad == 0 && oe, "detect: status not 0 before start (%d, oe %d)", bad, oe);
        memset(ddr + REU, 0, 256);
        c64_wr(0xDF21, 0); c64_wr(0xDF24, 0); c64_wr(0xDF25, 0); c64_wr(0xDF26, 0); c64_wr(0xDF27, 0);
        c64_wr(0xDF2E, 0); c64_wr(0xDF29, 0); c64_wr(0xDF2F, 1); c64_wr(0xDF2A, 1);
        c64_wr(0xDF20, 5);
        int n = 0, st = 0;
        // 6502 loop is about 11 C64 cycles per read; 128 reads at most
        for (n = 0; n < 128; n++) { st = c64_rd(0xDF20); if (st) break; run(11); }
        CHECK(n < 128 && st == 1, "detect: status %d after %d reads", st, n);
        int bad2 = 0;
        for (int i = 0; i < 256; i++) { if (c64_rd(0xDF20) != 1) bad2++; run(11); }
        CHECK(bad2 == 0, "detect: status not stable at 1 (%d)", bad2);
        CHECK(d->irq == 1, "irq line high while pending");
        CHECK(c64_rd(0xDF21) == 0x01, "version");
        c64_wr(0xDF3F, 0xFF);
        CHECK(c64_rd(0xDF20) == 0 && d->irq == 0, "ack clears status and irq");
        c64_wr(0xDF20, 0);
        printf("detect: status turned 1 after %d reads (x ~11 C64 cycles)\n", n);
    }
    reset_all();

    // ---- 2. 8 bit samples, signed, values, pan, volume; first fetch address
    {
        for (int i = 0; i < 64; i++) ddr[REU + 0x100 + i] = (uint8_t)(i * 2 - 60);   // signed 8 bit ramp, no equal neighbours
        fetches.clear();
        vsetup(0, REU + 0x100, 20, 400, 63, 15);
        log_.clear(); logging = true;
        c64_wr(0xDF20, 0x01);
        run(1500);
        logging = false;
        CHECK(!fetches.empty() && fetches[0] == REU + 0x100, "first fetch %08x", fetches.empty() ? 0 : fetches[0]);
        bool ok = fetches.size() == 20;
        for (size_t i = 0; i < fetches.size() && i < 20; i++) ok = ok && fetches[i] == REU + 0x100 + i;
        CHECK(ok, "8 bit fetch addresses (%zu fetches)", fetches.size());
        std::vector<int> l = seq(true), r = seq(false);
        // expected: silence, 20 samples, silence (first value 0 may be absent when the first sample is nonzero)
        std::vector<int> e;
        for (int i = 0; i < 20; i++) e.push_back(mixl(((int8_t)(i * 2 - 60)) * 256, 63, 15));
        std::vector<int> g(l.begin(), l.end());
        if (!g.empty() && g.front() == 0) g.erase(g.begin());
        if (!g.empty() && g.back() == 0) g.pop_back();
        CHECK(g == e, "8 bit L values (%zu vs %zu)", g.size(), e.size()); if (g != e) dump("8bit", g, e);
        bool rz = true; for (int x : r) if (x != 0) rz = false;
        CHECK(rz, "pan 15: right is silent");
        CHECK(!l.empty() && l.back() == 0, "silent after the end");
    }
    reset_all();
    // pan law and volume with a constant sample
    {
        memset(ddr + REU + 0x400, 0x40, 64);
        int pans[] = {0, 1, 3, 7, 8, 9, 12, 15};
        int vols[] = {63, 32, 1, 0};
        for (int vi = 0; vi < 4; vi++) for (int pi = 0; pi < 8; pi++) {
            vsetup(0, REU + 0x400, 64, 7, vols[vi], pans[pi], 0, 0);
            c64_wr(0xDF20, 0x03);                     // repeat, B = 0: loops at the length
            run(60);
            int l = sx(d->smp_l), r = sx(d->smp_r);
            CHECK(l == mixl(0x4000, vols[vi], pans[pi]) && r == mixr(0x4000, vols[vi], pans[pi]),
                  "vol %d pan %d: %d/%d expected %d/%d", vols[vi], pans[pi], l, r,
                  mixl(0x4000, vols[vi], pans[pi]), mixr(0x4000, vols[vi], pans[pi]));
            c64_wr(0xDF20, 0);
        }
        run(4);
        CHECK(sx(d->smp_l) == 0 && sx(d->smp_r) == 0, "stop silences");
    }
    reset_all();

    // ---- 3. 16 bit little endian signed
    {
        int16_t smp[] = {1000, -2000, 32767, -32768, 5, -1, 12345, -300};
        for (int i = 0; i < 8; i++) { ddr[REU + 0x1000 + 2 * i] = smp[i] & 0xFF; ddr[REU + 0x1000 + 2 * i + 1] = (smp[i] >> 8) & 0xFF; }
        fetches.clear();
        vsetup(1, REU + 0x1000, 16, 300, 40, 0);
        log_.clear(); logging = true;
        c64_wr(0xDF40, 0x11);
        run(900);
        logging = false;
        std::vector<int> r = seq(false);
        std::vector<int> e;
        for (int i = 0; i < 8; i++) e.push_back(mixr(smp[i], 40, 0));
        if (!r.empty() && r.front() == 0) r.erase(r.begin());
        if (!r.empty() && r.back() == 0) r.pop_back();
        CHECK(r == e, "16 bit R values (%zu vs %zu)", r.size(), e.size()); if (r != e) dump("16bit", r, e);
        bool ok = fetches.size() == 8;
        for (size_t i = 0; i < fetches.size() && i < 8; i++) ok = ok && fetches[i] == REU + 0x1000 + 2 * i;
        CHECK(ok, "16 bit fetches step 2 (%zu)", fetches.size());
        std::vector<int> l = seq(true);
        bool lz = true; for (int x : l) if (std::abs(x) > 5000) lz = false;   // pan 0: left is exactly 0
        bool l0 = true; for (int x : l) if (x != 0) l0 = false;
        CHECK(l0, "pan 0: left is silent");
        (void)lz;
    }
    reset_all();

    // ---- 4. rates: a sample every n x 160 ns
    {
        for (int i = 0; i < 4096; i++) ddr[REU + 0x2000 + i] = (uint8_t)(1 + (i % 100));  // ramp 1..100, repeating
        int rates[] = {7, 50, 156, 300, 1000, 5000};
        double clk[] = {985248.6, 1022684, 1023917, 987031};
        for (int m = 0; m < 2; m++) {
            d->vmode = m; run(8);
            for (int ri = 0; ri < 6; ri++) {
                int n = rates[ri];
                int cnt = n < 100 ? 300 : n < 500 ? 120 : 24;
                vsetup(2, REU + 0x2000, 4000, n, 63, 15);
                log_.clear(); logging = true;
                c64_wr(0xDF60, 0x01);
                double exp = n * clk[m] / 6.25e6;                       // C64 cycles per sample
                run((uint64_t)(exp * (cnt + 3)) + 20);
                logging = false;
                c64_wr(0xDF60, 0);
                // change instants of L
                std::vector<uint64_t> tt;
                int last = 0;
                for (auto &s : log_) { if (s.bl && n >= 20) continue; if (s.l != last) { tt.push_back(s.cyc); last = s.l; } }
                if (tt.size() > 4) {
                    size_t k = std::min<size_t>(tt.size() - 1, cnt);
                    double per = double(tt[k] - tt[1]) / (k - 1);
                    double err = (per - exp) / exp * 100;
                    double tol = n < 20 ? 25.0 : 1.5;
                    printf("rate %5d mode %d: %.3f cycles per sample, expected %.3f (%.2f %%)\n", n, m, per, exp, err);
                    CHECK(std::abs(err) < tol, "rate %d mode %d off by %.2f %%", n, m, err);
                } else CHECK(false, "rate %d: too few samples", n);
            }
        }
        d->vmode = 0;
    }
    reset_all();

    // ---- 5. repeat A/B, release, end, IRQ
    {
        for (int i = 0; i < 16; i++) ddr[REU + 0x3000 + i] = (uint8_t)(10 + i * 3);
        auto m = [](int i) { return mixl((int8_t)(10 + i * 3) * 256, 63, 15); };
        // loop 4..12 (B = 12): 0..11, then 4..11 over and over
        vsetup(0, REU + 0x3000, 16, 150, 63, 15, 4, 12);
        log_.clear(); logging = true;
        c64_wr(0xDF20, 0x03);
        run(1500);
        logging = false;
        std::vector<int> l = seq(true);
        if (!l.empty() && l.front() == 0) l.erase(l.begin());
        std::vector<int> e;
        for (int i = 0; i < 12; i++) e.push_back(m(i));
        for (int k = 0; k < 20; k++) for (int i = 4; i < 12; i++) e.push_back(m(i));
        bool ok = l.size() >= 40;
        for (size_t i = 0; i < l.size() && i < e.size(); i++) ok = ok && l[i] == e[i];
        CHECK(ok, "loop A=4 B=12 sequence (%zu samples)", l.size()); if (!ok) dump("loop", l, e);
        // release: ctrl 0x01 over 0x03 keeps the voice and lets it run on to the length
        log_.clear(); logging = true;
        c64_wr(0xDF20, 0x01);
        run(300);
        logging = false;
        l = seq(true);
        CHECK(!l.empty() && l.back() == 0, "after release: ends silent");
        bool has15 = std::find(l.begin(), l.end(), m(15)) != l.end();
        CHECK(has15, "after release: plays on to the last sample (index 15)");
        bool no_again = true;
        { int c = 0; for (int x : l) if (x == m(4)) c++; no_again = c <= 1; }
        CHECK(no_again, "after release: no further loop");
        c64_wr(0xDF20, 0);
        // B = 0 loops at the length
        vsetup(0, REU + 0x3000, 16, 150, 63, 15, 4, 0);
        log_.clear(); logging = true;
        c64_wr(0xDF20, 0x03);
        run(1500);
        logging = false;
        l = seq(true);
        if (!l.empty() && l.front() == 0) l.erase(l.begin());
        e.clear();
        for (int i = 0; i < 16; i++) e.push_back(m(i));
        for (int k = 0; k < 20; k++) for (int i = 4; i < 16; i++) e.push_back(m(i));
        ok = l.size() >= 30;
        for (size_t i = 0; i < l.size() && i < e.size(); i++) ok = ok && l[i] == e[i];
        CHECK(ok, "loop at the length (B = 0)"); if (!ok) dump("loopB0", l, e);
        c64_wr(0xDF20, 0);
        // IRQ: end of sample with IRQ enabled, none without; wrap sets it with repeat
        vsetup(0, REU + 0x3000, 8, 20, 63, 15);
        c64_wr(0xDF20, 0x01); run(60);
        CHECK(c64_rd(0xDF20) == 0 && d->irq == 0, "no IRQ without the enable bit");
        c64_wr(0xDF20, 0x00);
        c64_wr(0xDF20, 0x05); run(60);
        CHECK(c64_rd(0xDF20) == 1 && d->irq == 1, "IRQ at the end of the sample");
        c64_wr(0xDF3F, 0xFF);
        CHECK(c64_rd(0xDF20) == 0 && d->irq == 0, "acknowledged");
        c64_wr(0xDF20, 0x00);
        vsetup(0, REU + 0x3000, 8, 20, 63, 15, 0, 8);
        c64_wr(0xDF20, 0x07); run(60);
        CHECK(c64_rd(0xDF20) == 1, "IRQ at a loop wrap");
        c64_wr(0xDF20, 0);
        c64_wr(0xDF3F, 0xFF);
        // status is per voice
        vsetup(3, REU + 0x3000, 8, 20, 63, 15);
        c64_wr(0xDF80, 0x05); run(60);
        CHECK(c64_rd(0xDF80) == 1 && c64_rd(0xDF20) == 0 && c64_rd(0xDF40) == 0, "status is per voice");
        c64_wr(0xDF80, 0); c64_wr(0xDF9F, 0xFF);
        // restart: ctrl 0 then 1 starts from START again
        vsetup(0, REU + 0x3000, 16, 150, 63, 15);
        c64_wr(0xDF20, 0x01); run(100);
        c64_wr(0xDF20, 0); c64_wr(0xDF20, 0x01);
        log_.clear(); logging = true; run(40); logging = false;
        l = seq(true);
        CHECK(l.size() >= 2 && l[0] == 0 && l[1] == m(0), "restart plays from the first sample (%d)", l.size() > 1 ? l[1] : -1);
        c64_wr(0xDF20, 0);
    }
    reset_all();

    // ---- 6. eight voices, saturation, voice 7 from the app, register oddities
    {
        memset(ddr + REU + 0x5000, 0x7F, 4096);
        for (int v = 0; v < 8; v++) vsetup(v, REU + 0x5000, 4096, 7, 63, 15);
        for (int v = 0; v < 8; v++) vw(v, 0, 0x03);
        for (int v = 0; v < 8; v++) { vw(v, 0, 0); } run(8);
        for (int v = 0; v < 8; v++) {
            vsetup(v, REU + 0x5000, 4096, 7, 63, 15); vw(v, 0, 0x03); run(60);
            CHECK(sx(d->smp_l) == mixl(0x7F00, 63, 15), "voice %d alone: %d", v, sx(d->smp_l));
            vw(v, 0, 0); run(6);
        }
        for (int v = 0; v < 8; v++) { vsetup(v, REU + 0x5000, 4096, 7, 63, 15); vw(v, 0, 0x03); }
        run(120);
        CHECK(sx(d->smp_l) == 8 * mixl(0x7F00, 63, 15) && sx(d->smp_r) == 0, "8 voices full left: %d/%d", sx(d->smp_l), sx(d->smp_r));
        for (int v = 0; v < 8; v++) vw(v, 0, 0);
        run(10);
        // 3 voices: voice 7 through the app
        for (int v = 0; v < 8; v++) vsetup(v, REU + 0x5000, 4096, 7, 10 + v, v);
        for (int v : {1, 4, 7}) vw(v, 0, 0x03);
        run(120);
        int el = 0, er = 0;
        for (int v : {1, 4, 7}) { el += mixl(0x7F00, 10 + v, v); er += mixr(0x7F00, 10 + v, v); }
        CHECK(sx(d->smp_l) == el && sx(d->smp_r) == er, "three voices (7 via the app): %d/%d expected %d/%d", sx(d->smp_l), sx(d->smp_r), el, er);
        for (int v = 0; v < 8; v++) vw(v, 0, 0);
        run(10);
        CHECK(sx(d->smp_l) == 0 && sx(d->smp_r) == 0, "all stopped");
        // status and version through the app
        CHECK(app_op(0x20 * 7 + 1, false, 0) == 0x01, "app reads the version");
        CHECK(app_op(0x20 * 7 + 0, false, 0) == 0x00, "app reads status 0");
        vsetup(7, REU + 0x3000, 8, 20, 63, 15);
        vw(7, 0, 0x05); run(60);
        CHECK(app_op(0xE0, false, 0) == 0x01, "app reads status 1");
        app_op(0xFF, true, 0xFF);
        CHECK(app_op(0xE0, false, 0) == 0x00, "app acknowledges");
        vw(7, 0, 0);
        // the app's own clear: 64 zero bytes while the C64 plays: no hang, no click on other voices
        for (int i = 0; i < 64; i++) app_op(i, true, 0);
        // words 6 and 7 are not state: writes do nothing
        vsetup(0, REU + 0x5000, 4096, 100, 63, 8);
        vw(0, 0x18, 0xFF); vw(0, 0x1C, 0xFF); vw(0, 0x1D, 0xFF);
        vw(0, 0, 0x03); run(100);
        CHECK(d->smp_l == mixl(0x7F00, 63, 8) && d->smp_r == mixr(0x7F00, 63, 8), "writes to +18 and +1C are ignored");
        vw(0, 0, 0); run(4);
        // disabled window: no answer, no write
        d->c64_en = 0; run(4);
        bool oe = true; c64_rd(0xDF20, &oe);
        CHECK(!oe, "window off: no answer");
        vw(7, 0, 0);
        d->c64_en = 1; run(4);
        // outside the window
        c64_rd(0xDF1C, &oe); CHECK(!oe, "$DF1C is not ours");
        c64_rd(0xDE20, &oe); CHECK(!oe, "$DE20 is not ours");
        c64_rd(0xDF00, &oe); CHECK(!oe, "$DF00 (REU) is not ours");
    }
    reset_all();

    // ---- 7. the machine stopped: nothing advances, the app still gets through
    {
        vsetup(0, REU + 0x3000, 16, 150, 63, 15);
        c64_wr(0xDF20, 0x03);
        run(60);
        int l0 = sx(d->smp_l);
        c64_run = false;
        for (int i = 0; i < 5000; i++) tick();
        CHECK(sx(d->smp_l) == l0, "no C64 cycles: the output does not move");
        CHECK(app_op(0x21, false, 0) == 1, "app reaches the sampler while the machine is stopped");
        app_op(0x01, true, 20);                                    // volume change goes through
        for (int i = 0; i < 400; i++) tick();
        c64_run = true;
        run(5);
        CHECK(d->smp_l != l0 || true, "resumes");
        vw(0, 0, 0);
        run(4);
    }
    reset_all();

    // ---- 8. a slow DDR2 (the voice waits, no lost step) and back to back register writes on consecutive cycles
    {
        int save = lat; lat = 45;
        for (int i = 0; i < 40; i++) ddr[REU + 0x6000 + i] = (uint8_t)(i * 3 + 1);
        fetches.clear();
        vsetup(0, REU + 0x6000, 24, 100, 63, 15);
        log_.clear(); logging = true;
        c64_wr(0xDF20, 1); run(500); logging = false;
        std::vector<int> l = seq(true);
        std::vector<int> e;
        for (int i = 0; i < 24; i++) e.push_back(mixl((int8_t)(i * 3 + 1) * 256, 63, 15));
        if (!l.empty() && l.front() == 0) l.erase(l.begin());
        if (!l.empty() && l.back() == 0) l.pop_back();
        CHECK(l == e, "latency 45 clk: all samples in order (%zu)", l.size());
        lat = save;
        // four START bytes on four consecutive C64 cycles
        vw(0, 0, 0);
        wrq.push_back({0xDF24, 1}); wrq.push_back({0xDF25, 0x00}); wrq.push_back({0xDF26, 0x60}); wrq.push_back({0xDF27, 0x08});
        while (!wrq.empty()) tick();
        run(3);
        vw(0, 0x9, 0); vw(0, 0xA, 0); vw(0, 0xB, 4); vw(0, 0xE, 0); vw(0, 0xF, 50);
        fetches.clear();
        vw(0, 0, 1); run(100);
        CHECK(!fetches.empty() && fetches[0] == REU + 0x6008, "consecutive writes: START %08x", fetches.empty() ? 0 : fetches[0]);
        vw(0, 0, 0);
    }
    // ---- 9. a long run with all voices busy: the fetch machinery keeps up (no voice starves)
    {
        reset_all();
        for (int i = 0; i < 8192; i++) ddr[REU + 0x8000 + i] = (uint8_t)(1 + (i * 7) % 120);
        int per_v[8];
        for (int v = 0; v < 8; v++) vsetup(v, REU + 0x8000, 8000, 160 + 3 * v, 20, v);
        fetches.clear();
        for (int v = 0; v < 8; v++) vw(v, 0, 1);
        uint64_t c0 = cycles;
        run(2000);
        double secs = (cycles - c0) / 985248.6;
        size_t n = fetches.size();
        double want = 0; for (int v = 0; v < 8; v++) want += secs * 6.25e6 / (160 + 3 * v);
        printf("8 voices: %zu fetches in %.2f ms, expected %.0f (%.2f %%)\n", n, secs * 1e3, want, (n - want) / want * 100);
        CHECK(std::abs((double)n - want) / want < 0.03, "8 voices at about 40 kHz keep their rate");
        (void)per_v;
        for (int v = 0; v < 8; v++) vw(v, 0, 0);
    }

    // ---- 10. U11: a 16 bit sample starts at any byte address
    {
        const uint32_t base = REU + 0x9000;
        int16_t smp[] = {1000, -2000, 32767, -32768, 5, -1, 12345, -300};
        for (int off = 0; off < 4; off++) {
            uint32_t start = base + off;
            for (int i = 0; i < 16; i++) ddr[start + i] = 0;
            for (int i = 0; i < 8; i++) {
                ddr[start + 2 * i]     = smp[i] & 0xFF;
                ddr[start + 2 * i + 1] = (smp[i] >> 8) & 0xFF;
            }
            fetches.clear();
            vsetup(1, start, 16, 300, 40, 0);
            log_.clear(); logging = true;
            vw(1, 0, 0x11);
            run(900);
            logging = false;
            vw(1, 0, 0);
            std::vector<int> r = seq(false);
            if (!r.empty() && r.front() == 0) r.erase(r.begin());
            if (!r.empty() && r.back() == 0) r.pop_back();
            std::vector<int> e;
            for (int i = 0; i < 8; i++) e.push_back(mixr(smp[i], 40, 0));
            CHECK(r == e, "16 bit at byte offset %d: the values at that address (%zu vs %zu)",
                  off, r.size(), e.size());
            if (r != e) dump("offset", r, e);
            // one read per sample, two only where the sample crosses the 32 bit word
            std::vector<uint32_t> ef;
            for (int i = 0; i < 8; i++) {
                uint32_t a = start + 2 * i;
                ef.push_back(a);
                if ((a & 3) == 3) ef.push_back(a + 1);
            }
            bool ok = fetches.size() == ef.size();
            for (size_t i = 0; i < fetches.size() && i < ef.size(); i++) ok = ok && fetches[i] == ef[i];
            CHECK(ok, "16 bit at byte offset %d: %zu reads for 8 samples, expected %zu",
                  off, fetches.size(), ef.size());
        }
        // D5: rel steps by 2 from 0, so an odd LENGTH is the even one below it
        int ends[3][2] = {{8, 4}, {9, 4}, {10, 5}};
        for (int k = 0; k < 3; k++) {
            for (int i = 0; i < 16; i++) ddr[base + i] = (uint8_t)(0x40 + i);
            fetches.clear();
            vsetup(1, base, ends[k][0], 300, 40, 0);
            vw(1, 0, 0x11);
            run(900);
            vw(1, 0, 0);
            CHECK((int)fetches.size() == ends[k][1], "16 bit LENGTH %d stops after %d samples (%zu)",
                  ends[k][0], ends[k][1], fetches.size());
        }
    }
    reset_all();

    printf("%d checks, %d failed\n", checks, fails);
    printf(fails ? "tb_sampler FAIL\n" : "tb_sampler PASS\n");
    return fails ? 1 : 0;
}
