// SPDX-License-Identifier: GPL-3.0-or-later
// c64_core under Verilator: the app's DMA as a stolen bus cycle while the C64
// RUNS (G06, docs/specs/G06-dma-cycle-steal.md). The 6510 keeps executing a
// stress program from RAM (KERNAL IRQ on, screen on, badlines) while the
// bench issues DMA bytes into $D000-$DFFF the way freeze_resp's cyc_req does
// (dma_req pulse, addr/we/wdata held, dma_done pulse with dma_rdata).
//
//   tb_steal run     cold start to READY, then all checks in this process
//
// Phases:
//   1  single reads/writes: $D020 (read $FE at READY, write $02 reads back
//      $F2, as on Gideon's core), $D021, CIA1 $DC01 (no key = $FF), a CIA1
//      DDRB write and read back
//   2  hammer: back-to-back and randomly spaced reads and writes while the
//      CPU runs loop A; every read checked, loop A's invariants checked, the
//      CPU must keep counting (no lost write, no corrupted read, no crash);
//      the longest wait must show badlines were waited out
//   3  with REU transfers: loop B stashes and fetches 256 bytes through a REU
//      memory model all the time while the bench steals bytes
//   4  during a CPU reset (informational)
// Exit code 0 = ok.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "Vc64_core.h"
#include "verilated.h"

static Vc64_core *top;
static uint64_t clocks;
static int fails;

// ---- the REU's memory (c64_top's p1 port: 16-bit words, rdata_valid later) ----
static std::vector<uint16_t> reu_mem(1 << 20);
static int reu_rd_delay = 0;
static uint16_t reu_rd_word;
static uint64_t reu_words;

static void reu_model_pre()
{
    // the transaction of this clock edge: valid and ready seen before it
    if (top->reu_cmd_valid && top->reu_cmd_ready) {
        uint32_t a = top->reu_cmd_addr & 0xFFFFF;
        if (top->reu_cmd_we) {
            uint16_t w = reu_mem[a];
            if (top->reu_wdata_we & 1) w = (w & 0xFF00) | (top->reu_wdata & 0xFF);
            if (top->reu_wdata_we & 2) w = (w & 0x00FF) | (top->reu_wdata & 0xFF00);
            reu_mem[a] = w;
        } else {
            reu_rd_word = reu_mem[a];
            reu_rd_delay = 4;
        }
        reu_words++;
    }
}
static void reu_model_post()
{
    top->reu_rdata_valid = 0;
    if (reu_rd_delay && --reu_rd_delay == 0) {
        top->reu_rdata = reu_rd_word;
        top->reu_rdata_valid = 1;
    }
}

static void tick()
{
    reu_model_pre();
    // the BUFGCE: gclk's edge passes if the enable was high before it; clk2x (T29: the CPU and c64_mem's
    // second port) rises with every clk edge and falls halfway
    bool pass = top->gate_ce;
    top->clk = 1; top->gclk = pass; top->snd_clk = 1; top->cart_wclk = 1; top->clk2x = 1;
    top->eval();
    top->clk2x = 0;
    top->eval();
    top->clk = 0; top->gclk = 0; top->snd_clk = 0; top->cart_wclk = 0; top->clk2x = 1;
    top->eval();
    top->clk2x = 0;
    top->eval();
    reu_model_post();
    clocks++;
}

static void run_cycles(uint64_t n) { for (uint64_t i = 0; i < n * 32; i++) tick(); }

#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static uint8_t peek(uint16_t a)
{
    top->dbg_addr = a; top->dbg_req = 1;
    tick(); tick();
    top->dbg_req = 0;
    uint8_t v = top->dbg_rdata;
    tick();
    return v;
}
static void poke(uint16_t a, uint8_t v)
{
    top->dbg_addr = a; top->dbg_wdata = v; top->dbg_we = 1; top->dbg_req = 1;
    tick(); tick();
    top->dbg_we = 0; top->dbg_req = 0;
    tick();
}

// one stolen byte, 0 = done, -1 = no answer within limit clocks
static uint64_t dma_n, dma_clk_sum, dma_clk_max, dma_slow;
static int dma(uint16_t a, bool we, uint8_t v, int *rdata, uint64_t limit = 32ull * 3000)
{
    top->dma_addr = a; top->dma_we = we; top->dma_wdata = v; top->dma_req = 1;
    tick();
    top->dma_req = 0;
    for (uint64_t n = 0; n < limit; n++) {
        tick();
        if (top->dma_done) {
            *rdata = top->dma_rdata;
            dma_n++; dma_clk_sum += n + 2;
            if (n + 2 > dma_clk_max) dma_clk_max = n + 2;
            if (n + 2 > 32 * 20) dma_slow++;
            return 0;
        }
    }
    return -1;
}
static uint8_t rd(uint16_t a)
{
    int d;
    if (dma(a, false, 0, &d)) { printf("FAIL: DMA read %04X timeout\n", a); exit(4); }
    return uint8_t(d);
}
static void wr(uint16_t a, uint8_t v)
{
    int d;
    if (dma(a, true, v, &d)) { printf("FAIL: DMA write %04X timeout\n", a); exit(4); }
}

static std::string row(int r)
{
    std::string s;
    for (int i = 0; i < 40; i++) {
        uint8_t c = peek(0x0400 + r * 40 + i) & 0x7F;
        if (c == 0) s += '@';
        else if (c < 27) s += char('A' + c - 1);
        else if (c < 64) s += char(c);
        else s += '.';
    }
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

static void init()
{
    top->rst = 1; top->snd_rst = 1; top->audio_sample_en = 0;
    top->keys = 0; top->joy1 = 0; top->joy2 = 0; top->restore = 0;
    top->dbg_addr = 0; top->dbg_we = 0; top->dbg_wdata = 0; top->dbg_req = 0;
    top->reu_en = 1; top->reu_size = 5; top->reu_cmd_ready = 1; top->reu_rdata = 0; top->reu_rdata_valid = 0;
    top->cpu_reset_req = 0; top->cpu_pause_req = 0;
    top->cart_type = 0; top->cart_kill_tgl = 0; top->cart_force_tgl = 0;
    top->cart_ack_tgl = 0; top->cart_fill_be = 0;
    top->dma_req = 0; top->dma_addr = 0; top->dma_we = 0; top->dma_wdata = 0; top->ultimax = 0;
    top->dbg_rom = 0; top->sb_addr = 0; top->cap_every = 0;
    top->cram_addr = 0; top->cram_we = 0; top->cram_wdata = 0;
    for (int i = 0; i < 64; i++) tick();
    top->rst = 0; top->snd_rst = 0;
}

static bool boot()
{
    init();
    for (int n = 0; n < 400; n++) {
        run_cycles(10000);
        for (int r = 0; r < 25; r++)
            if (row(r).rfind("READY.", 0) == 0) {
                run_cycles(20000);
                printf("READY after %llu cycles\n", (unsigned long long)(clocks / 32));
                return true;
            }
    }
    return false;
}

static void load(uint16_t at, std::initializer_list<uint8_t> b)
{
    for (uint8_t v : b) poke(at++, v);
}

// loop A ($C200): two counters, a subroutine with stack traffic, $D020 sampled
//   $C000 += 1; $C001 += 1; JSR sub ($C002 += 3); $C003 = $D020
static void load_loop_a()
{
    load(0xC200, {0xEE, 0x00, 0xC0,             // INC $C000
                  0xEE, 0x01, 0xC0,             // INC $C001
                  0x20, 0x20, 0xC2,             // JSR $C220
                  0xAD, 0x20, 0xD0,             // LDA $D020
                  0x8D, 0x03, 0xC0,             // STA $C003
                  0x4C, 0x00, 0xC2});           // JMP $C200
    load(0xC220, {0x48,                         // PHA
                  0xAD, 0x02, 0xC0,             // LDA $C002
                  0x18,                         // CLC
                  0x69, 0x03,                   // ADC #3
                  0x8D, 0x02, 0xC0,             // STA $C002
                  0x68,                         // PLA
                  0x60});                       // RTS
}

// loop B ($C300): REU stash $C400 -> REU 0:0000, fetch REU 0:0000 -> $C500,
// 256 bytes each, counter at $C000
static void load_loop_b()
{
    load(0xC300, {0xEE, 0x00, 0xC0,             // INC $C000
                  0xA9, 0xC4, 0xA0, 0x90, 0x20, 0x20, 0xC3,     // A=$C4 Y=stash JSR xfer
                  0xA9, 0xC5, 0xA0, 0x91, 0x20, 0x20, 0xC3,     // A=$C5 Y=fetch JSR xfer
                  0x4C, 0x00, 0xC3});           // JMP $C300
    load(0xC320, {0x8D, 0x03, 0xDF,             // STA $DF03 (C64 hi)
                  0xA9, 0x00,
                  0x8D, 0x02, 0xDF,             // C64 lo
                  0x8D, 0x04, 0xDF, 0x8D, 0x05, 0xDF, 0x8D, 0x06, 0xDF,   // REU address
                  0x8D, 0x07, 0xDF,             // length lo
                  0xA9, 0x01, 0x8D, 0x08, 0xDF, // length hi = 1
                  0x8C, 0x01, 0xDF,             // STY $DF01: go
                  0x60});
}

// loop A's invariants, read through the RAM port while the CPU runs: A-B in
// {0,1} (the three peeks are a few clocks apart: one iteration of slack), C = 3B
// or 3B-3 (before the subroutine ran)
static bool loop_a_ok(const char *when)
{
    uint8_t a = peek(0xC000), b = peek(0xC001), c = peek(0xC002);
    uint8_t d = uint8_t(a - b);
    uint8_t e = uint8_t(c - 3 * b);
    bool ok = (d <= 1) && (e == 0 || e == 0xFD);
    if (!ok) printf("  %s: A=%02X B=%02X C=%02X\n", when, a, b, c);
    return ok;
}

static uint32_t rng = 12345;
static uint32_t rnd() { rng = rng * 1664525u + 1013904223u; return rng >> 8; }

static int test()
{
    if (!boot()) {
        printf("no READY\n");
        for (int r = 0; r < 25; r++) { std::string s = row(r); if (!s.empty()) printf("  | %s\n", s.c_str()); }
        return 3;
    }

    // ---- the CPU runs; nothing pauses it from outside ----
    uint8_t jiffy0 = peek(0xA2);
    run_cycles(20000);
    CHECK(peek(0xA2) != jiffy0, "jiffy clock does not run at READY");

    // ---- phase 1: single accesses at the READY prompt ----
    printf("phase 1: single accesses\n");
    uint8_t d20 = rd(0xD020), d21 = rd(0xD021);
    printf("  $D020 = %02X, $D021 = %02X (Gideon's core at READY: $FE, $F6)\n", d20, d21);
    CHECK(d20 == 0xFE, "$D020 = %02X, want $FE (light blue, unused bits 1)", d20);
    CHECK(d21 == 0xF6, "$D021 = %02X, want $F6", d21);
    wr(0xD020, 0x02);
    uint8_t d20w = rd(0xD020);
    printf("  after write $02: $D020 = %02X (Gideon: $F2)\n", d20w);
    CHECK(d20w == 0xF2, "$D020 after write = %02X, want $F2", d20w);
    wr(0xD020, 0x0E);
    CHECK(rd(0xD020) == 0xFE, "$D020 not restored");
    uint8_t dc01 = rd(0xDC01);
    printf("  CIA1 $DC01 = %02X, $DC00 = %02X\n", dc01, rd(0xDC00));
    CHECK(dc01 == 0xFF, "$DC01 = %02X, no key pressed", dc01);
    uint8_t ddrb = rd(0xDC03);
    wr(0xDC03, 0xA5);
    uint8_t ddrb_w = rd(0xDC03);
    wr(0xDC03, ddrb);
    CHECK(ddrb_w == 0xA5, "CIA1 DDRB write/readback %02X", ddrb_w);
    CHECK(rd(0xDC03) == ddrb, "CIA1 DDRB not restored");
    CHECK(peek(0xA2) != jiffy0, "jiffy clock stands after phase 1");

    // ---- phase 2: hammer, loop A running ----
    printf("phase 2: hammer with the CPU in loop A\n");
    poke(0xC000, 0); poke(0xC001, 0); poke(0xC002, 0); poke(0xC003, 0);
    load_loop_a();
    const char *cmd = "SYS49664\r";                 // $C200
    for (int i = 0; cmd[i]; i++) poke(0x0277 + i, uint8_t(cmd[i]));
    poke(0x00C6, uint8_t(strlen(cmd)));
    run_cycles(60000);
    uint8_t a0 = peek(0xC000);
    run_cycles(500);
    CHECK(peek(0xC000) != a0, "loop A is not running");
    CHECK(loop_a_ok("start"), "loop A invariants before the first steal");
    dma_n = dma_clk_sum = dma_clk_max = dma_slow = 0;
    int bad_d20 = 0, bad_dc = 0, bad_inv = 0, sample_bad = 0;
    uint8_t d20_expect = 0xFE;
    for (int i = 0; i < 4000; i++) {
        // the first 1000 back to back, then random gaps (also odd dot clocks)
        if (i >= 1000) for (uint32_t g = rnd() % (i & 1 ? 40 : 700); g; g--) tick();
        switch (rnd() % 5) {
        case 0: {
            uint8_t v = uint8_t(rnd() & 0x0F);
            wr(0xD020, v); d20_expect = 0xF0 | v;
            break;
        }
        case 1: if (rd(0xD020) != d20_expect) bad_d20++; break;
        case 2: if (rd(0xDC01) != 0xFF) bad_dc++; break;
        case 3: {
            uint8_t v = uint8_t(rnd());
            wr(0xDC03, v);
            if (rd(0xDC03) != v) bad_dc++;
            wr(0xDC03, 0x00);                       // the keyboard scan reads port B
            break;
        }
        default: rd(0xD012); break;
        }
        if (i % 250 == 249) {
            uint8_t before = peek(0xC000);
            if (!loop_a_ok("hammer")) bad_inv++;
            run_cycles(300);
            if (peek(0xC000) == before) sample_bad++;
        }
    }
    printf("  %llu stolen bytes, mean wait %.1f C64 cycles, max %.1f, %llu longer than 20 cycles\n",
           (unsigned long long)dma_n, double(dma_clk_sum) / dma_n / 32, double(dma_clk_max) / 32,
           (unsigned long long)dma_slow);
    CHECK(bad_d20 == 0, "%d wrong $D020 reads", bad_d20);
    CHECK(bad_dc == 0, "%d wrong CIA1 reads", bad_dc);
    CHECK(bad_inv == 0, "%d loop A invariant breaks (a CPU cycle was lost or corrupted)", bad_inv);
    CHECK(sample_bad == 0, "%d times the CPU did not run on after the steals", sample_bad);
    CHECK(dma_slow > 0, "no steal waited for a badline: the BA path is not exercised");
    CHECK(dma_clk_max < 32 * 200, "a steal waited %.1f cycles", double(dma_clk_max) / 32);
    // the CPU saw $D020 as written: loop A copies it to $C003
    wr(0xD020, 0x02);
    run_cycles(500);
    uint8_t c3 = peek(0xC003);
    CHECK(c3 == 0xF2, "the CPU read $D020 = %02X after the write of $02", c3);
    wr(0xD020, 0x0E);
    uint8_t j1 = peek(0xA2);
    run_cycles(20000);
    CHECK(peek(0xA2) != j1, "jiffy clock stands after phase 2");
    CHECK(loop_a_ok("end"), "loop A invariants at the end of phase 2");

    // ---- phase 3: REU transfers all the time ----
    printf("phase 3: with REU DMA\n");
    uint8_t pat[256];
    for (int i = 0; i < 256; i++) { pat[i] = uint8_t(i * 7 + 3); poke(0xC400 + i, pat[i]); poke(0xC500 + i, 0); }
    load_loop_b();
    poke(0xC211, 0xC3);                             // loop A's JMP $C200 -> JMP $C300 (one byte, atomic)
    run_cycles(3000);
    uint8_t b0 = peek(0xC000);
    run_cycles(3000);
    CHECK(peek(0xC000) != b0, "loop B (REU) is not running");
    uint64_t words0 = reu_words;
    dma_n = dma_clk_sum = dma_clk_max = dma_slow = 0;
    int bad3 = 0;
    for (int i = 0; i < 600; i++) {
        for (uint32_t g = rnd() % 300; g; g--) tick();
        if (i & 1) { uint8_t v = rd(0xD020); if (v != d20_expect) bad3++; }
        else { uint8_t v = uint8_t(rnd() & 15); wr(0xD020, v); d20_expect = 0xF0 | v; }
    }
    printf("  %llu stolen bytes, mean wait %.1f C64 cycles, max %.1f; REU moved %llu words meanwhile\n",
           (unsigned long long)dma_n, double(dma_clk_sum) / dma_n / 32, double(dma_clk_max) / 32,
           (unsigned long long)(reu_words - words0));
    CHECK(bad3 == 0, "%d wrong $D020 reads during REU transfers", bad3);
    CHECK(reu_words - words0 > 1000, "the REU moved only %llu words", (unsigned long long)(reu_words - words0));
    run_cycles(2000);
    int rbad = 0, fbad = 0;
    for (int i = 0; i < 256; i++) {
        uint16_t w = reu_mem[i / 2];
        uint8_t m = (i & 1) ? w >> 8 : w & 0xFF;
        if (m != pat[i]) rbad++;
        if (peek(0xC500 + i) != pat[i]) fbad++;
    }
    CHECK(rbad == 0, "%d bytes in the REU model differ from the stashed page", rbad);
    CHECK(fbad == 0, "%d bytes of the fetched page differ", fbad);
    uint8_t b1 = peek(0xC000);
    run_cycles(5000);
    CHECK(peek(0xC000) != b1, "loop B stands after phase 3");

    // ---- phase 4: during a CPU reset (informational) ----
    printf("phase 4: CPU held in reset (informational)\n");
    top->cpu_reset_req = 1;
    run_cycles(100);
    int d;
    int rc = dma(0xD020, false, 0, &d, 32ull * 4000);
    printf("  DMA read $D020 in reset: %s", rc ? "no answer" : "answered");
    if (!rc) printf(" %02X", d);
    printf("\n");
    top->cpu_reset_req = 0;
    run_cycles(2000);

    printf(fails ? "%d FAILED\n" : "PASS\n", fails);
    return fails ? 1 : 0;
}

int main(int argc, char **argv)
{
    Verilated::commandArgs(argc, argv);
    top = new Vc64_core;
    int rc = 3;
    if (argc >= 2 && !strcmp(argv[1], "run")) rc = test();
    else fprintf(stderr, "usage: %s run\n", argv[0]);
    top->final();
    delete top;
    return rc;
}
