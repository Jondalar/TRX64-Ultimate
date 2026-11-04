// SPDX-License-Identifier: GPL-3.0-or-later
// T25.6 stress bench: seven voices retriggered the way Heartbeat does it (a per-tick flush of the control bytes of all
// voices, direct 0x10 + registers + a deferred 0x11 on a new note), with a DDR2 that answers after a random number of
// clocks, so the done event lands on every phase of the engine's 64 slot frame. Invariants, checked inside the engine:
//   F  the fetch address of a voice is its previous one + 2 (+0 at a start: START)
//   S  the sample a voice puts out (s16 at the visit) is the 16 bit word that was fetched for that very voice
//   R  after a (re)start the voice puts out 0 until the first fetch of the new note has come back (no stale sample of
//      a fetch that was in flight at the restart)
//   tb_stress [seed] [latency lo] [latency hi] [cycles]
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <vector>
#include <deque>
#include "Vtrx_sampler.h"
#include "Vtrx_sampler___024root.h"
#include "verilated.h"

static Vtrx_sampler *d;
static uint8_t *ddr;
static int latlo = 8, lathi = 80;
static uint64_t cycles = 0;
static int ev_div = 0;
struct Wr { uint16_t a; uint8_t v; };
static std::deque<Wr> wrq;
static uint8_t last_req = 0;
static int req_epoch = 0;
static long stale_landed = 0;     // fetches that came back after the engine had restarted their voice
static int resp_cnt = -1;
static uint32_t rng = 1;
static uint32_t rnd() { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

struct V {
    bool run = false;           // the model thinks the voice plays
    bool dirty = true;          // a (re)start happened and no sample has arrived yet
    uint32_t start = 0, len = 0, next = 0;
    int rate = 7;
    int fetched = 0;
    int32_t exp16 = 0;          // the word that was fetched last and has come back
    bool exp_valid = false;
    int32_t inflight = 0;
    bool in_flight = false;
    bool in_flight_done = false;
    bool checked = true;
    uint8_t ctl = 0;
    uint32_t last_fa = 0; int last_ep = -1;   // the engine's own previous fetch of the voice (rule F)
    int exp_ep = 0;             // epoch at which exp16 came back
    int epoch = 0;              // counts the engine's restarts of the voice (the start handler)
    bool fresh = false;         // restarted by the engine, first sample of the new note not yet back
} vm[8];
static long fails_r = 0, fails_f = 0, fails_s = 0, fetch_n = 0, checks_s = 0, stale_lo = 0, stale_hi = 0, stray = 0;

static uint16_t ddr16(uint32_t a) { a &= ~1u; return ddr[a] | ddr[a + 1] << 8; }

// the high byte of the 16 bit word and the low byte are written to the voice's RAM in two slots; look at the engine's
// RAM directly at the visit
static void tick()
{
    auto *rp = d->rootp;
    bool ev = false;
    d->io_ev = 0; d->io_ev_io = 0; d->io_ev_we = 0;
    if (++ev_div == 32) {
        ev_div = 0; ev = true; cycles++;
        d->io_ev = 1;
        if (!wrq.empty()) {
            d->io_ev_io = 1; d->io_ev_we = 1; d->io_ev_addr = wrq.front().a; d->io_ev_data = wrq.front().v;
            wrq.pop_front();
        }
    }
    (void)ev;
    for (int ph = 0; ph < 4; ph++) {
        bool clk2x_edge = (ph == 0 || ph == 2);
        if (ph == 0) { d->clk = 1; d->clk2x = 1; }
        else if (ph == 1) { d->clk2x = 0; }
        else if (ph == 2) { d->clk = 0; d->clk2x = 1; }
        else { d->clk2x = 0; }
        bool pre_vis = rp->trx_sampler__DOT__vis;
        int pre_vj = rp->trx_sampler__DOT__vj, pre_vv = rp->trx_sampler__DOT__vv;
        if (clk2x_edge && rp->trx_sampler__DOT__t == 43 && rp->trx_sampler__DOT__fq) {
            V &c = vm[rp->trx_sampler__DOT__fv];
            // a fetch the engine drops (fkill) is not a sample; one that was requested before a restart must not
            // clear `fresh` (and must not show up as a sample either: rule R)
            // (a stopped voice puts out 0 whatever came back: smp_in)
            if (!rp->trx_sampler__DOT__fkill && ((rp->trx_sampler__DOT__run >> rp->trx_sampler__DOT__fv) & 1)) {
                if (c.in_flight_done) { c.exp16 = c.inflight; c.exp_valid = true; c.in_flight_done = false; c.dirty = false; c.checked = false; c.exp_ep = c.epoch; } else c.exp_valid = false;   // a sample the model did not request (old note, before its restart)
                if (c.epoch == req_epoch) c.fresh = false;
            } else c.in_flight_done = false;
        }
        // the start handler finishes its last slot (t = 46, hs = 2): the voice is new, its sample words are zero
        if (clk2x_edge && rp->trx_sampler__DOT__hm == 1 && rp->trx_sampler__DOT__t == 46)
            { V &c = vm[rp->trx_sampler__DOT__hv]; c.fresh = true; c.epoch++; }
        d->eval();
        if (clk2x_edge && pre_vis && pre_vj == 3) {
            // the sample of the voice pre_vv was just taken apart: the new sample is old + delta (the blend of the output comes later)
            V &v = vm[pre_vv];
            int s16 = (int)((rp->trx_sampler__DOT__sold_r + rp->trx_sampler__DOT__dlt) & 0xffff);
            if (v.fresh && s16 != 0) {
                fails_r++;
                if (fails_r <= 8) printf("R voice %d s16 %04x after a restart, before its first sample (cyc %llu)\n", pre_vv, (uint16_t)s16, (unsigned long long)cycles);
            }
            // a voice that ran out (stop step right after its last sample) or was restarted since shows 0: not checked
            if (!v.dirty && v.run && v.exp_valid && !v.checked && v.fetched > 3 && v.exp_ep == v.epoch && ((rp->trx_sampler__DOT__run >> pre_vv) & 1)) {
                v.checked = true;
                checks_s++;
                if ((int)(uint16_t)s16 != (int)(uint16_t)v.exp16) {
                    fails_s++;
                    if (fails_s <= 8) printf("S voice %d s16 %04x expected %04x (fetched %d, cyc %llu)\n", pre_vv, (uint16_t)s16, (uint16_t)v.exp16, v.fetched, (unsigned long long)cycles);
                    // classify: only the low byte differs?
                    if (((uint16_t)s16 >> 8) == ((uint16_t)v.exp16 >> 8)) stale_lo++;
                    else if (((uint16_t)s16 & 255) == ((uint16_t)v.exp16 & 255)) stale_hi++;
                }
            }
        }
    }
    // DDR2: one read at a time, answered after a random latency
    if (resp_cnt < 0 && d->f_req_tgl != last_req) {
        last_req = d->f_req_tgl;
        resp_cnt = latlo + rnd() % (lathi - latlo + 1);
        uint32_t a = d->f_addr & ~3u;
        d->f_data = ddr[a] | ddr[a + 1] << 8 | ddr[a + 2] << 16 | (uint32_t)ddr[a + 3] << 24;
        int fv = d->rootp->trx_sampler__DOT__fv;
        V &v = vm[fv];
        fetch_n++;
        req_epoch = v.epoch;
        uint32_t fa = d->f_addr & ~1u;
        uint32_t prev_fa = v.last_fa; int prev_ep = v.last_ep;
        v.last_ep = v.epoch; v.last_fa = fa;
        if (!v.run) { stray++; }
        else {
            // within one run of the voice (no restart in between) the address goes on by 2; the first one of a run is START
            // (not checked here: the model sets the new START ahead of the engine's restart)
            if (prev_ep == v.epoch && fa != prev_fa + 2) {
                fails_f++;
                if (fails_f <= 8) printf("F voice %d fetch %08x expected %08x (cyc %llu)\n", fv, fa, prev_fa + 2, (unsigned long long)cycles);
            }
            v.next = fa + 2;
            v.inflight = ddr16(fa); v.in_flight = true;
            v.fetched++;
        }
    } else if (resp_cnt > 0) resp_cnt--;
    if (resp_cnt == 0) {
        d->f_done_tgl = !d->f_done_tgl; resp_cnt = -1;
        int fv = d->rootp->trx_sampler__DOT__fv;
        V &v = vm[fv];
        if (v.epoch != req_epoch) stale_landed++;
        if (v.in_flight) { v.in_flight = false; v.in_flight_done = true; }
    }
}
static void run(uint64_t c) { uint64_t e = cycles + c; while (cycles < e) tick(); }
static void wr(int v, int off, uint8_t val) { wrq.push_back({(uint16_t)(0xDF20 + 32 * v + off), val}); }

int main(int argc, char **argv)
{
    Verilated::commandArgs(argc, argv);
    int seed = argc > 1 ? atoi(argv[1]) : 1;
    if (argc > 2) latlo = atoi(argv[2]);
    if (argc > 3) lathi = atoi(argv[3]);
    uint64_t total = argc > 4 ? strtoull(argv[4], 0, 10) : 4000000;
    rng = 2463534242u + seed;
    ddr = (uint8_t *)calloc(1, 128u << 20);
    const uint32_t REU = 0x1000000;
    for (uint32_t i = 0; i < 0x200000; i += 2) { uint32_t h = (i + seed) * 2654435761u; h ^= h >> 15; ddr[REU + i] = h; ddr[REU + i + 1] = h >> 8; }
    d = new Vtrx_sampler;
    d->rst = 1; d->vmode = 0; d->c64_en = 1; d->bus_addr = 0; d->io_ev_addr = 0; d->io_ev_data = 0;
    d->a_tgl = 0; d->a_we = 0; d->a_addr = 0; d->a_wdata = 0; d->f_done_tgl = 0; d->f_data = 0;
    for (int i = 0; i < 64; i++) tick();
    d->rst = 0;
    run(8);
    uint8_t sg[7] = {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10};
    // the voice model starts with run = false; a voice starts when the control byte goes 0x10 -> 0x11 (enable 0 -> 1)
    long frames = 0;
    // Directed phase (rule R): retrigger a voice (0x10, 0x11: stop, start again) while its fetch is in flight, with
    // a DDR2 slow enough that the answer lands after the engine has restarted the voice.
    {
        int slo = latlo, shi = lathi;
        latlo = 150; lathi = 400;
        uint32_t start = REU + 0x1000, len = 0x4000;
        wr(0, 0, 0x10); wr(0, 1, 40); wr(0, 2, 8);
        for (int i = 0; i < 4; i++) wr(0, 4 + i, start >> (24 - 8 * i));
        for (int i = 0; i < 3; i++) wr(0, 9 + i, len >> (16 - 8 * i));
        wr(0, 0xE, 0); wr(0, 0xF, 120);
        for (int i = 0; i < 3; i++) { wr(0, 0x11 + i, 0); wr(0, 0x15 + i, 0); }
        wr(0, 0, 0x11);
        vm[0].start = start; vm[0].len = len; vm[0].next = start; vm[0].run = true; vm[0].dirty = true;
        while (!wrq.empty()) tick();
        for (int it = 0; it < 400; it++) {
            run(20 + rnd() % 200);
            int guard = 0;
            while (!(d->rootp->trx_sampler__DOT__fbusy && !d->rootp->trx_sampler__DOT__fret && resp_cnt > 40) && guard++ < 100000) tick();
            for (int i = rnd() % 3; i > 0; i--) tick();
            wr(0, 0, 0x10); wr(0, 0, 0x11);
            vm[0].run = true; vm[0].dirty = true; vm[0].exp_valid = false; vm[0].in_flight = false; vm[0].in_flight_done = false;
            vm[0].next = start; vm[0].fetched = 0;
            while (!wrq.empty()) tick();
        }
        run(2000);
        printf("directed retrigger with a fetch in flight: 400 retriggers, %ld fetches came back after their voice was restarted, R fails %ld\n", stale_landed, fails_r);
        latlo = slo; lathi = shi;
        if (stale_landed == 0) { printf("directed phase did not reach the case\n"); return 2; }
        long fr = fails_r; (void)fr;
    }
    while (cycles < total) {
        // one player tick (20 ms = 19656 cycles): notes, then the flush
        int nev = rnd() % 3;
        bool used[7] = {};
        for (int e = 0; e < nev; e++) {
            int v = rnd() % 7;
            if (getenv("DISTINCT") && used[v]) continue;
            used[v] = true;
            uint32_t start = REU + ((rnd() % 0x80000) & ~3u);
            uint32_t len = (500 + rnd() % 16000) & ~1u;
            int rate = 40 + rnd() % 200;
            wr(v, 0, 0x10);
            wr(v, 1, 20 + rnd() % 44); wr(v, 2, rnd() & 15);
            for (int i = 0; i < 4; i++) wr(v, 4 + i, start >> (24 - 8 * i));
            for (int i = 0; i < 3; i++) wr(v, 9 + i, len >> (16 - 8 * i));
            wr(v, 0xE, rate >> 8); wr(v, 0xF, rate);
            for (int i = 0; i < 3; i++) { wr(v, 0x11 + i, 0); wr(v, 0x15 + i, 0); }
            // model: stop now, start at the flush
            vm[v].run = false; vm[v].dirty = true; vm[v].exp_valid = false; vm[v].in_flight = false; vm[v].in_flight_done = false;
            vm[v].start = start; vm[v].len = len; vm[v].next = start; vm[v].fetched = 0;
            sg[v] = 0x11;
            // model of the end: the voice stops itself when rel == len; tracked below by the fetch count
        }
        // the flush
        for (int v = 0; v < 7; v++) {
            uint8_t b = sg[v];
            wr(v, 0, b);
            if (b == 0x11 && !vm[v].run && vm[v].len) { vm[v].run = true; }
        }
        while (!wrq.empty()) tick();
        // let the tick pass; voices that played to their length: the model stops them when the fetch count passes
        uint64_t until = cycles + 19656 - 100;
        while (cycles < until) {
            tick();
            for (int v = 0; v < 7; v++) if (vm[v].run && vm[v].next >= vm[v].start + vm[v].len) vm[v].run = false;
        }
        frames++;
    }
    printf("seed %d lat %d..%d: %ld frames, %ld fetches, %ld stray, F fails %ld, S checks %ld fails %ld (low byte stale %ld, high byte stale %ld), R fails %ld\n",
           seed, latlo, lathi, frames, fetch_n, stray, fails_f, checks_s, fails_s, stale_lo, stale_hi, fails_r);
    return (fails_f || fails_s || fails_r) ? 1 : 0;
}
