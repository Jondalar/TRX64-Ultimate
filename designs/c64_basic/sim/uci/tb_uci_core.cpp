// SPDX-License-Identifier: GPL-3.0-or-later
// T28 §7.1 on the real machine (tb_uci_top.v: c64_core with uci_unit): the
// KERNAL boots to READY, a 6510 program at $C000 sends DOS identify through
// $DF1C-$DF1F (with the freeze request: the CPU waits until the app has
// answered), reads response and status while their bits say so, accepts; the
// app is a model of command_intf.cc on the IO bus, driven by the UCI's
// interrupt. Checks: reply and status bytes in C64 RAM, the state the program
// saw, freeze held the CPU and was let go, the number of UCI register
// accesses equals the program's (no read counted twice by a held CPU).
// The REU is on and there is no cartridge: $DF1C-$DF1F would be the REU's
// mirror without the UCI. Exit code 0 = ok.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include "Vtb_uci_top.h"
#include "verilated.h"

static Vtb_uci_top *top;
static uint64_t now_ps, next_clk, next_sys, sys_cycles, clk_edges;
static int fails;
static const uint64_t CLK_HALF = 15859;     // 31.5278 MHz
static const uint64_t SYS_HALF = 5000;      // 100 MHz
static bool freeze_seen;
static uint64_t freeze_clks;

#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static uint64_t next_c2 = ~0ull;
static void step()
{
    uint64_t t = next_clk < next_sys ? next_clk : next_sys;
    if (next_c2 < t) t = next_c2;
    now_ps = t;
    // clk2x (the T25.1 SID engine's clock): rises with every clk edge, falls halfway
    if (next_c2 == t) { top->clk2x = 0; next_c2 = ~0ull; }
    if (next_sys == t) {
        if (!top->sys_clk) sys_cycles++;
        top->sys_clk = !top->sys_clk; next_sys += SYS_HALF;
    }
    if (next_clk == t) {
        if (!top->clk) {
            clk_edges++;
            if (top->freeze) { freeze_seen = true; freeze_clks++; }
        }
        top->clk = !top->clk; next_c2 = next_clk + CLK_HALF / 2; next_clk += CLK_HALF; top->clk2x = 1;
    }
    top->eval();
}
static void sys_tick() { uint64_t c = sys_cycles; while (sys_cycles == c) step(); }
static void clk_tick() { uint64_t c = clk_edges; while (clk_edges == c) step(); while (top->clk) step(); }
static void run_us(double us) { uint64_t end = now_ps + uint64_t(us * 1e6); while (now_ps < end) step(); }

// ---- the app's IO bus to the UCI's range ----------------------------------------
static void io_write(uint16_t a, uint8_t v)
{
    top->io_addr = a; top->io_wdata = v; top->io_write = 1;
    sys_tick();
    top->io_write = 0;
    for (int n = 0; n < 1000 && !top->io_ack; n++) sys_tick();
    if (!top->io_ack) { printf("FAIL: io write %04X not acked\n", a); exit(4); }
    sys_tick();
}
static uint8_t io_read(uint16_t a)
{
    top->io_addr = a; top->io_read = 1;
    sys_tick();
    top->io_read = 0;
    for (int n = 0; n < 1000 && !top->io_ack; n++) sys_tick();
    if (!top->io_ack) { printf("FAIL: io read %04X not acked\n", a); exit(4); }
    uint8_t v = top->io_rdata;
    sys_tick();
    return v;
}

// ---- the C64 RAM through c64_mem's second port ------------------------------------
static uint8_t peek(uint16_t a) { top->dbg_addr = a; clk_tick(); clk_tick(); return top->dbg_rdata; }
static void poke(uint16_t a, uint8_t v)
{
    top->dbg_addr = a; top->dbg_wdata = v; top->dbg_we = 1;
    clk_tick();
    top->dbg_we = 0;
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
static int find_row(const char *prefix)
{
    for (int r = 0; r < 25; r++)
        if (row(r).rfind(prefix, 0) == 0) return r;
    return -1;
}

// ---- the app: command_intf.cc's ISR and task, DOS identify only -------------------
static const char IDENT[] = "ULTIMATE-II DOS V1.2";
static const char STAT[] = "00,OK";
static int app_cmds;
static void app_service()
{
    if (!top->io_irq) return;
    uint8_t flags = io_read(3) & ~io_read(0xB);
    io_write(4, flags);                                 // IRQMASK_SET
    if (flags & 0x01) {
        int len = io_read(0xE) | (io_read(0xF) & 7) << 8;
        uint8_t c0 = io_read(0x800), c1 = io_read(0x801);
        CHECK(len == 2 && c0 == 0x01 && c1 == 0x01, "app: command len %d %02X %02X", len, c0, c1);
        app_cmds++;
        io_write(2, 0x01);                              // ACCEPT_COMMAND
        for (unsigned i = 0; i < strlen(IDENT); i++) io_write(0x800 + 896 + i, IDENT[i]);
        for (unsigned i = 0; i < strlen(STAT); i++) io_write(0x800 + 1792 + i, STAT[i]);
        io_write(0xD, 0);
        io_write(0xC, strlen(IDENT));
        io_write(0xA, strlen(STAT));
        io_write(2, 0x10);                              // VALIDATE_LAST
        io_write(5, 0x01);
    }
    if (flags & 0x06) io_write(5, flags & 0x06);
}

static const uint8_t PROG[] = {
    0xA9, 0x01, 0x8D, 0x1D, 0xDF, 0x8D, 0x1D, 0xDF,   // DOS 1, IDENTIFY -> $DF1D
    0xA9, 0x81, 0x8D, 0x1C, 0xDF,                     // push, freeze
    0xAD, 0x1C, 0xDF, 0x29, 0x30, 0xC9, 0x10, 0xF0, 0xF7,   // wait while processing
    0x8D, 0xF0, 0xC1,                                 // state -> $C1F0
    0xA2, 0x00,
    0xAD, 0x1C, 0xDF, 0x10, 0x09,                     // response while bit 7
    0xAD, 0x1E, 0xDF, 0x9D, 0x00, 0xC2, 0xE8, 0xD0, 0xF2,
    0x8E, 0xF1, 0xC1,                                 // count -> $C1F1
    0xA2, 0x00,
    0x2C, 0x1C, 0xDF, 0x50, 0x09,                     // status while bit 6
    0xAD, 0x1F, 0xDF, 0x9D, 0x80, 0xC2, 0xE8, 0xD0, 0xF2,
    0x8E, 0xF2, 0xC1,                                 // count -> $C1F2
    0xA9, 0x02, 0x8D, 0x1C, 0xDF,                     // accept
    0xAD, 0x1C, 0xDF, 0x8D, 0xF4, 0xC1,               // state after -> $C1F4
    0xA9, 0xAA, 0x8D, 0xF3, 0xC1,                     // done
    0x4C, 0x4F, 0xC0,                                 // JMP *
};

int main(int argc, char **argv)
{
    Verilated::commandArgs(argc, argv);
    top = new Vtb_uci_top;
    top->rst = 1; top->sys_rst = 1;
    next_clk = CLK_HALF; next_sys = SYS_HALF;
    for (int i = 0; i < 100; i++) sys_tick();
    top->rst = 0; top->sys_rst = 0;
    for (int i = 0; i < 100; i++) sys_tick();

    // the app's UCI setup: enable, $DF1C, reset, interrupts on
    CHECK(io_read(6) == 0x70, "RESPONSE_START");
    io_write(1, 0x01);
    io_write(0, 0x47);
    io_write(2, 0x87);
    io_write(5, 0x07);

    for (int k = 0; k < 80; k++) {
        run_us(50000);
        if (find_row("READY.") >= 0) break;
    }
    CHECK(find_row("READY.") >= 0, "C64: no READY");
    printf("READY after %.1f ms\n", now_ps / 1e9);

    for (unsigned i = 0; i < sizeof PROG; i++) poke(0xC000 + i, PROG[i]);
    const char *sys = "SYS49152\r";
    for (int i = 0; sys[i]; i++) poke(0x0277 + i, sys[i]);
    poke(0x00C6, strlen(sys));

    uint64_t t_end = now_ps + 300000000000ull;          // 300 ms
    while (now_ps < t_end) {
        for (int i = 0; i < 2000; i++) step();
        app_service();
        if (peek(0xC1F3) == 0xAA) break;
    }
    CHECK(peek(0xC1F3) == 0xAA, "program did not finish");
    std::string resp, stat;
    for (int i = 0; i < peek(0xC1F1); i++) resp += char(peek(0xC200 + i));
    for (int i = 0; i < peek(0xC1F2); i++) stat += char(peek(0xC280 + i));
    printf("state %02X, response '%s', status '%s', state after accept %02X\n", peek(0xC1F0),
           resp.c_str(), stat.c_str(), peek(0xC1F4));
    CHECK((peek(0xC1F0) & 0x30) == 0x20, "state not DATA_LAST");
    CHECK(resp == IDENT, "response");
    CHECK(stat == STAT, "status");
    CHECK((peek(0xC1F4) & 0x30) == 0x00, "not idle after accept");
    CHECK(app_cmds == 1, "app saw %d commands", app_cmds);
    printf("freeze held the CPU for %llu clocks\n", (unsigned long long)freeze_clks);
    CHECK(freeze_seen && !top->freeze, "freeze");
    // the program's accesses: 2 command bytes, push, accept = 4 writes;
    // reads: 1 wait-loop poll (freeze holds the CPU at the opcode fetch after
    // the push until the answer is there), 21 bit-7 polls + 20 response, 6 BIT
    // polls + 5 status, 1 after the accept = 54, each seen once by the UCI
    printf("UCI register accesses: %u writes, %u reads\n", top->ev_writes, top->ev_reads);
    CHECK(top->ev_writes == 4, "writes %u", top->ev_writes);
    CHECK(top->ev_reads == 54, "reads %u", top->ev_reads);
    printf("%s, %d failures\n", fails ? "FAILED" : "PASS", fails);
    return fails ? 1 : 0;
}
