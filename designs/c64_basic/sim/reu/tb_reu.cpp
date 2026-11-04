// SPDX-License-Identifier: GPL-3.0-or-later
// c64_core under Verilator: the REU as Gideon's core has it, up to 16 MB (T60, docs/specs/T60-reu-16mb.md).
// The REU registers are written and read through the app's DMA window (dma(): a stolen bus cycle at
// $DF00-$DF0A, exactly what the app's C64 access does), the transfers move bytes between RAM at $C400
// (poke/peek through the RAM port) and a sparse REU memory model behind reu_cmd_* (one cell per 16-bit word
// address, every command logged).
//
//   tb_reu run     cold start to READY (REU disabled: reu_en = 0 as after the app's reset), then:
//   A  enable: off = $DF02-$DF08 do not latch, on (live, no reset) = they do, off again
//   B  16 MB: bank $FF reaches word 0x7FFFFF, bank $80 is its own cell, the counter wraps at 24 bits
//   C  size masks: for each size the byte of bank $AB lands at the address Gideon's reu.vhd computes
//   D  wrap: 128 KB reads the same cell 128 KB further up, 16 MB does not; the 512 KB quirk of the standard
//      REU (bits 23:19 from the base register); $DF00 bit 4 follows the size; $DF06 reads back 11111+bits 18:16
// Exit code 0 = ok.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <unordered_map>
#include "Vc64_core.h"
#include "verilated.h"

static Vc64_core *top;
static uint64_t clocks;
static int fails;

// ---- the REU's memory (c64_top's p1 port: 16-bit words, rdata_valid later): sparse, keyed by the
// 23-bit word address c64_core hands out, so that every address of the 16 MB is its own cell. Every
// command is logged. A word nobody wrote reads back $EEEE.
static std::unordered_map<uint32_t, uint16_t> reu_mem;
static int reu_rd_delay = 0;
static uint16_t reu_rd_word;
static uint64_t reu_words;
struct ReuCmd { uint32_t addr; bool we; };
static std::vector<ReuCmd> reu_log;
static uint32_t reu_max_addr;

static void reu_model_pre()
{
    // the transaction of this clock edge: valid and ready seen before it
    if (top->reu_cmd_valid && top->reu_cmd_ready) {
        uint32_t a = top->reu_cmd_addr;
        reu_log.push_back({a, bool(top->reu_cmd_we)});
        if (a > reu_max_addr) reu_max_addr = a;
        if (top->reu_cmd_we) {
            uint16_t w = reu_mem.count(a) ? reu_mem[a] : 0xEEEE;
            if (top->reu_wdata_we & 1) w = (w & 0xFF00) | (top->reu_wdata & 0xFF);
            if (top->reu_wdata_we & 2) w = (w & 0x00FF) | (top->reu_wdata & 0xFF00);
            reu_mem[a] = w;
        } else {
            reu_rd_word = reu_mem.count(a) ? reu_mem[a] : 0xEEEE;
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
    top->reu_en = 0; top->reu_size = 7; top->reu_cmd_ready = 1; top->reu_rdata = 0; top->reu_rdata_valid = 0;
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


// ---- REU helpers: Gideon's address formula (reu.vhd, standard REU) -----------------------------------
static uint8_t size_mask(int s) { return s >= 7 ? 0xFF : uint8_t((2 << s) - 1); }
// the byte address the memory sees: bits 23:19 from the bank written to $DF06 (base), 18:16 from the
// running address, masked by the size
static uint32_t gideon_addr(int size, uint32_t base, uint32_t run)
{
    uint8_t m = size_mask(size);
    uint32_t hi = (((base >> 16) & 0xF8) & (m & 0xF8)) | (((run >> 16) & 7) & (m & 7));
    return (hi << 16) | (run & 0xFFFF);
}

static void reg(int r, uint8_t v) { wr(0xDF00 + r, v); }
static uint8_t regr(int r) { return rd(0xDF00 + r); }

// one transfer, type 0 = C64 -> REU, 1 = REU -> C64; waits for the end-of-block flag
static bool xfer(int type, uint16_t c64, uint32_t reu, uint16_t len)
{
    reg(0x0A, 0x00);
    reg(0x02, c64 & 0xFF); reg(0x03, c64 >> 8);
    reg(0x04, reu & 0xFF); reg(0x05, (reu >> 8) & 0xFF); reg(0x06, (reu >> 16) & 0xFF);
    reg(0x07, len & 0xFF); reg(0x08, len >> 8);
    reg(0x01, 0x90 | type);
    for (int i = 0; i < 400; i++) {
        run_cycles(30);
        if (regr(0x00) & 0x40) return true;
    }
    return false;
}

// the commands since log index from, against the formula: one per byte, write or read
static int check_log(size_t from, int type, int size, uint32_t reu, int len, const char *what)
{
    int bad = 0;
    if (reu_log.size() - from != size_t(len)) {
        printf("  %s: %zu commands, want %d\n", what, reu_log.size() - from, len);
        return 1;
    }
    for (int k = 0; k < len; k++) {
        uint32_t want = gideon_addr(size, reu, (reu + k) & 0xFFFFFF) >> 1;
        const ReuCmd &c = reu_log[from + k];
        if (c.addr != want || c.we != (type == 0)) {
            if (bad++ < 4) printf("  %s: byte %d: cmd %06X %s, want %06X %s\n", what, k, c.addr, c.we ? "W" : "R",
                                  want, type == 0 ? "W" : "R");
        }
    }
    return bad;
}

static uint8_t reu_byte(uint32_t byte_addr)       // the model's byte at a REU (word) address, $EE when unwritten
{
    uint32_t a = byte_addr >> 1;
    uint16_t w = reu_mem.count(a) ? reu_mem[a] : 0xEEEE;
    return (byte_addr & 1) ? w >> 8 : w & 0xFF;
}

static int test()
{
    if (!boot()) {
        printf("no READY\n");
        for (int r = 0; r < 25; r++) { std::string s = row(r); if (!s.empty()) printf("  | %s\n", s.c_str()); }
        return 3;
    }
    uint8_t jiffy0 = peek(0xA2);
    run_cycles(20000);
    CHECK(peek(0xA2) != jiffy0, "jiffy clock does not run at READY");

    // ---- A: enable ----
    printf("A: enable\n");
    int stuck = 0;
    for (int r = 2; r <= 8; r++) { reg(r, 0x5A); if (regr(r) == 0x5A) stuck++; }
    printf("  REU off: %d of 7 registers latched a write\n", stuck);
    CHECK(stuck == 0, "REU disabled but %d registers answer", stuck);
    top->reu_en = 1; run_cycles(50);
    for (int r = 2; r <= 5; r++) { reg(r, 0x5A); CHECK(regr(r) == 0x5A, "REU on: $DF0%X reads %02X", r, regr(r)); }
    reg(0x07, 0x33); CHECK(regr(0x07) == 0x33, "REU on: $DF07 reads %02X", regr(0x07));
    top->reu_en = 0; run_cycles(50);
    reg(0x02, 0x11); CHECK(regr(0x02) != 0x11, "REU off again (live, no reset): $DF02 still answers");
    top->reu_en = 1; run_cycles(50);
    printf("  on again\n");

    // ---- B: 16 MB ----
    printf("B: 16 MB\n");
    top->reu_size = 7; run_cycles(50);
    uint8_t pat[16];
    for (int i = 0; i < 16; i++) { pat[i] = uint8_t(0xA0 + i * 3); poke(0xC400 + i, pat[i]); poke(0xC500 + i, 0x00); }
    size_t l0 = reu_log.size();
    reu_max_addr = 0;
    CHECK(xfer(0, 0xC400, 0xFFFFF0, 16), "stash at $FFFFF0 did not end");
    CHECK(check_log(l0, 0, 7, 0xFFFFF0, 16, "stash $FFFFF0") == 0, "stash commands differ from the formula");
    printf("  highest word address %06X (want 7FFFFF)\n", reu_max_addr);
    CHECK(reu_max_addr == 0x7FFFFF, "highest word address %06X, want 7FFFFF", reu_max_addr);
    int mb = 0;
    for (int i = 0; i < 16; i++) if (reu_byte(0xFFFFF0 + i) != pat[i]) mb++;
    CHECK(mb == 0, "%d bytes at the top of the 16 MB differ in the model", mb);
    uint8_t a4 = regr(0x04), a5 = regr(0x05), a6 = regr(0x06);
    printf("  after the transfer $DF04-06 = %02X %02X %02X (24 bit counter wrapped)\n", a4, a5, a6);
    CHECK(a4 == 0x00 && a5 == 0x00 && a6 == 0xF8, "counter after the wrap %02X %02X %02X, want 00 00 F8", a4, a5, a6);
    l0 = reu_log.size();
    CHECK(xfer(1, 0xC500, 0xFFFFF0, 16), "fetch from $FFFFF0 did not end");
    int fb = 0;
    for (int i = 0; i < 16; i++) if (peek(0xC500 + i) != pat[i]) fb++;
    CHECK(fb == 0, "%d bytes fetched from the top of the 16 MB differ", fb);
    CHECK(check_log(l0, 1, 7, 0xFFFFF0, 16, "fetch $FFFFF0") == 0, "fetch commands differ from the formula");
    // bank $80 is its own cell
    poke(0xC400, 0x77);
    CHECK(xfer(0, 0xC400, 0x800010, 1), "stash at $800010");
    poke(0xC500, 0x00);
    CHECK(xfer(1, 0xC500, 0x000010, 1), "fetch from $000010");
    CHECK(peek(0xC500) == 0xEE, "bank $00 byte $10 = %02X: bank $80 aliases it (want $EE, never written)", peek(0xC500));
    CHECK(reu_byte(0x800010) == 0x77, "bank $80 byte not in the model");

    // ---- C: size masks ----
    printf("C: size masks, bank $AB at $1234\n");
    for (int s = 0; s < 8; s++) {
        top->reu_size = s; run_cycles(50);
        uint8_t v = uint8_t(0x40 + s);
        poke(0xC400, v);
        l0 = reu_log.size();
        CHECK(xfer(0, 0xC400, 0xAB1234, 1), "size %d: stash did not end", s);
        uint32_t want = gideon_addr(s, 0xAB1234, 0xAB1234);
        printf("  size %d (mask %02X): byte address %06X\n", s, size_mask(s), want);
        CHECK(check_log(l0, 0, s, 0xAB1234, 1, "mask") == 0, "size %d: address differs from the formula", s);
        CHECK(reu_byte(want) == v, "size %d: byte not at %06X", s, want);
    }

    // ---- D: wrap, status, read-back ----
    printf("D: wrap\n");
    top->reu_size = 0; run_cycles(50);
    for (int i = 0; i < 8; i++) { poke(0xC400 + i, uint8_t(0x30 + i)); poke(0xC500 + i, 0); }
    CHECK(xfer(0, 0xC400, 0x000100, 8), "128K: stash");
    CHECK(xfer(1, 0xC500, 0x020100, 8), "128K: fetch from +128K");
    int wb = 0;
    for (int i = 0; i < 8; i++) if (peek(0xC500 + i) != 0x30 + i) wb++;
    CHECK(wb == 0, "128 KB: %d bytes at +128K differ (a 128 KB REU wraps)", wb);
    top->reu_size = 7; run_cycles(50);
    for (int i = 0; i < 8; i++) poke(0xC500 + i, 0);
    CHECK(xfer(1, 0xC500, 0x020100, 8), "16M: fetch from +128K");
    int nb = 0;
    for (int i = 0; i < 8; i++) if (peek(0xC500 + i) == 0x30 + i) nb++;
    CHECK(nb == 0, "16 MB: +128K aliases the first 128K");
    // the standard REU keeps bits 23:19 of the base: a transfer over a 512K boundary wraps in its 512K
    for (int i = 0; i < 4; i++) poke(0xC400 + i, uint8_t(0x90 + i));
    l0 = reu_log.size();
    CHECK(xfer(0, 0xC400, 0x07FFFE, 4), "16M: stash across 512K");
    CHECK(check_log(l0, 0, 7, 0x07FFFE, 4, "512K") == 0, "512K crossing differs from the formula");
    CHECK(reu_log[l0 + 2].addr == 0x000000, "third byte at word %06X: Gideon's standard REU wraps to 0", reu_log[l0 + 2].addr);
    // status bit 4 = size >= 256K, $DF06 reads 11111 + bits 18:16
    for (int s = 0; s < 8; s++) {
        top->reu_size = s; run_cycles(50);
        uint8_t st = regr(0x00);
        CHECK(((st & 0x10) != 0) == (s != 0), "size %d: status %02X bit 4", s, st);
    }
    reg(0x06, 0xAB);
    CHECK(regr(0x06) == 0xFB, "$DF06 after writing $AB reads %02X, want $FB", regr(0x06));

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
