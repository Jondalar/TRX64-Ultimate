// SPDX-License-Identifier: GPL-3.0-or-later
// T38 on the real machine (tb_input_top.v: c64_core with app_input.v): the
// KERNAL boots to READY, then
//   1. the app's matrix (MATRIX_KEYB bytes 0-7 as Keyboard_USB writes them)
//      types A; the KERNAL's scan puts it on the screen;
//   2. with the menu owning the physical keyboard (ovl_own) a held physical X
//      stays out, the app's DEL and "PRINT1+1" RETURN go in: " 2" printed;
//   3. a 6510 loop at $C000 (SEI, PA all high) copies $DC00/$DC01 to
//      $C100/$C101: the app's joystick bits arrive (port 2 PA, port 1 PB),
//      the physical ones too, both together as a wired AND;
//   4. RESTORE from the app (byte 9) and the physical key each run the NMI
//      vector ($0318 -> INC $C103) once;
//   5. the app's reset byte holds the CPU in reset: the loop stops.
// Exit code 0 = ok.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include "Vtb_input_top.h"
#include "verilated.h"

static Vtb_input_top *top;
static uint64_t now_ps, next_clk, clk_edges;
static int fails;
static const uint64_t CLK_HALF = 15859;     // 31.5278 MHz

#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } \
                              else { printf("ok: " __VA_ARGS__); printf("\n"); } } while (0)

static uint64_t next_c2 = ~0ull;
static void step()
{
    uint64_t t = next_clk < next_c2 ? next_clk : next_c2;
    now_ps = t;
    if (next_c2 == t) { top->clk2x = 0; next_c2 = ~0ull; }
    if (next_clk == t) {
        if (!top->clk) clk_edges++;
        top->clk = !top->clk; next_c2 = next_clk + CLK_HALF / 2; next_clk += CLK_HALF; top->clk2x = 1;
    }
    top->eval();
}
static void clk_tick() { uint64_t c = clk_edges; while (clk_edges == c) step(); while (top->clk) step(); }
static void run_ms(double ms) { uint64_t end = now_ps + uint64_t(ms * 1e9); while (now_ps < end) step(); }

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
static int find_row(const char *prefix, int from = 0)
{
    for (int r = from; r < 25; r++)
        if (row(r).rfind(prefix, 0) == 0) return r;
    return -1;
}
static int last_row(const char *prefix)
{
    int found = -1;
    for (int r = 0; r < 25; r++)
        if (row(r).rfind(prefix, 0) == 0) found = r;
    return found;
}

// C64 matrix positions as col*8 + row (CIA 1 PA column, PB row)
static int key_of(char c)
{
    static const char *cols[8] = {
        "\x7f\r\0\0\0\0\0\0",   // DEL, RETURN, ...
        "3WA4ZSE\0", "5RD6CFTX", "7YG8BHUV", "9IJ0MKON", "+PL-.:@,", "\0*;\0\0=\0/", "1\0\0" "2 \0Q\0" };
    for (int col = 0; col < 8; col++)
        for (int r = 0; r < 8; r++)
            if (cols[col][r] == c && c) return col * 8 + r;
    return -1;
}
static uint64_t bit(int k) { return 1ull << k; }

// the app's matrix as applyMatrixState leaves it after each change: held
// 40 ms (two KERNAL scans and more), released 40 ms
static void app_tap(char c)
{
    int k = key_of(c);
    if (k < 0) { printf("FAIL: no key for '%c'\n", c); fails++; return; }
    top->app_keys = bit(k);
    run_ms(40);
    top->app_keys = 0;
    run_ms(40);
}

static const uint8_t PROG[] = {
    0xA9, 0x80, 0x8D, 0x18, 0x03,           // $0318/$0319 = $C080
    0xA9, 0xC0, 0x8D, 0x19, 0x03,
    0x78,                                   // SEI: no KERNAL scan
    0xA9, 0xFF, 0x8D, 0x00, 0xDC,           // PA all high: no column selected
    0xAD, 0x00, 0xDC, 0x8D, 0x00, 0xC1,     // loop: $DC00 -> $C100
    0xAD, 0x01, 0xDC, 0x8D, 0x01, 0xC1,     //       $DC01 -> $C101
    0xEE, 0x02, 0xC1,                       //       INC $C102
    0x4C, 0x10, 0xC0,                       //       JMP loop
};
static const uint8_t NMI_HANDLER[] = { 0xEE, 0x03, 0xC1, 0x40 };   // INC $C103, RTI

int main(int argc, char **argv)
{
    Verilated::commandArgs(argc, argv);
    top = new Vtb_input_top;
    top->rst = 1;
    next_clk = CLK_HALF;
    for (int i = 0; i < 200; i++) clk_tick();
    top->rst = 0;

    for (int k = 0; k < 80; k++) {
        run_ms(50);
        if (find_row("READY.") >= 0) break;
    }
    int ready = find_row("READY.");
    CHECK(ready >= 0, "C64: READY after %.1f ms", now_ps / 1e9);
    if (ready < 0) return 1;

    // 1. the app types A
    app_tap('A');
    CHECK(row(ready + 1) == "A", "app matrix A typed: '%s'", row(ready + 1).c_str());

    // 2. menu owns the physical keyboard: physical X held is ignored, the app's keys pass
    top->ovl_own = 1;
    top->phys_keys = bit(key_of('X'));
    app_tap('\x7f');
    for (const char *p = "PRINT1+1\r"; *p; p++) app_tap(*p);
    top->phys_keys = 0;
    top->ovl_own = 0;
    run_ms(60);
    int pr = find_row("PRINT1+1");
    CHECK(pr >= 0 && row(pr + 1) == " 2", "PRINT1+1 typed and run: '%s' / '%s'",
          pr >= 0 ? row(pr).c_str() : "-", pr >= 0 ? row(pr + 1).c_str() : "-");

    // 3. joysticks, read by a loop with PA released
    for (unsigned i = 0; i < sizeof PROG; i++) poke(0xC000 + i, PROG[i]);
    for (unsigned i = 0; i < sizeof NMI_HANDLER; i++) poke(0xC080 + i, NMI_HANDLER[i]);
    poke(0xC103, 0);
    const char *sys = "SYS49152\r";
    for (int i = 0; sys[i]; i++) poke(0x0277 + i, sys[i]);
    poke(0x00C6, strlen(sys));
    run_ms(40);
    uint8_t n0 = peek(0xC102); run_ms(1);
    CHECK(peek(0xC102) != n0, "loop runs");
    CHECK(peek(0xC100) == 0xFF && peek(0xC101) == 0xFF, "idle: $DC00 %02X $DC01 %02X", peek(0xC100), peek(0xC101));
    top->app_joy2 = 0x10; top->app_joy1 = 0x01;             // port 2 fire, port 1 up
    run_ms(1);
    CHECK(peek(0xC100) == 0xEF && peek(0xC101) == 0xFE, "app: $DC00 %02X $DC01 %02X", peek(0xC100), peek(0xC101));
    top->phys_joy2 = 0x04; top->phys_joy1 = 0x08;           // and physical left / right
    run_ms(1);
    CHECK(peek(0xC100) == 0xEB && peek(0xC101) == 0xF6, "app + physical: $DC00 %02X $DC01 %02X", peek(0xC100), peek(0xC101));
    top->app_joy2 = 0; top->app_joy1 = 0;
    run_ms(1);
    CHECK(peek(0xC100) == 0xFB && peek(0xC101) == 0xF7, "physical: $DC00 %02X $DC01 %02X", peek(0xC100), peek(0xC101));
    top->phys_joy2 = 0; top->phys_joy1 = 0;
    run_ms(1);
    CHECK(peek(0xC100) == 0xFF && peek(0xC101) == 0xFF, "released: $DC00 %02X $DC01 %02X", peek(0xC100), peek(0xC101));

    // 4. RESTORE: the app's byte 9, then the physical key
    top->app_restore = 1; run_ms(2); top->app_restore = 0; run_ms(2);
    CHECK(peek(0xC103) == 1, "app RESTORE: NMI count %d", peek(0xC103));
    top->phys_restore = 1; run_ms(2); top->phys_restore = 0; run_ms(2);
    CHECK(peek(0xC103) == 2, "physical RESTORE: NMI count %d", peek(0xC103));

    // 5. the app's reset byte holds the CPU
    top->app_reset = 1;
    run_ms(1);
    CHECK(top->c64_reset, "reset request out");
    n0 = peek(0xC102); run_ms(1);
    CHECK(peek(0xC102) == n0, "loop held in reset");
    top->app_reset = 0;
    run_ms(1);
    CHECK(!top->c64_reset, "reset request released");

    printf("%s (%d failures, %.1f ms simulated)\n", fails ? "FAIL" : "PASS", fails, now_ps / 1e9);
    delete top;
    return fails ? 1 : 0;
}
