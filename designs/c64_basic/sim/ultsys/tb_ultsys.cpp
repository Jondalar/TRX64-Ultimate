// SPDX-License-Identifier: GPL-3.0-or-later
// The debugger's stop through ultimate_sys (T12 §14.8, T21 monitor; SLIM-state-path.md): tb_us_top.v is
// tb_dbgs_top (c64_core, trx_dbg, dma_win, trx_sys) with the app's DMA window made by the real
// ultimate_sys.vhd, whose RISC-V is a bench stand-in (rvlite_bfm.v) that makes the byte accesses the app
// makes. sim/dbgsess drives dma_* into dma_win directly and so cannot see what ultimate_sys lets through.
//
//   tb_ultsys boot <state>   cold start to READY, save
//   tb_ultsys run <state>    restore, then (C64_STOP is never set in 1-3: the machine is stopped by the debugger):
//     1. a debugger stop opens the D page ($D000-$DFFF) of the window as bus cycles: it equals the same read
//        in an app freeze (C64_STOP + Ultimax) on the same clock edge, colour RAM included, and each byte
//        costs a machine cycle; there is no session (SH_STATUS 0)
//     2. $DC0D, $D019 looked at over the state bus leave the words as they were; the window's read of
//        $DC0D is a bus cycle that clears the flag
//     3. a write to $D020/$D021/$D800 in the stop reaches the chip at once, and the machine goes on at RUN
//     4. MEMONLY: the D page is the RAM under the I/O, no bus cycle
//     6. the app's freeze through ultimate_sys: C64_STOP + C64_DMA_MEMONLY, RAM and the colour RAM aperture
//        (0x18F000): the menu is written and read back with no machine cycle at all
//     7. T62: U64_RESTORE_REG follows the RESTORE level, C64_SCANLINES reaches scanlines, the debug register
//        0x10181800 takes the app's writes and the C64's writes to $D7FF (a program of the C64 writes two bytes; the last wins)
//     5. the menu's own keyboard scan: KEYB_COL / KEYB_ROW give the matrix, KEYB_JOY the selected
//        joystick port (the C64-screen menu scans these instead of the CIA)
//   Exit code 0 = ok.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include <cstdarg>
#include <ctime>
#include "Vtb_us_top.h"
#include "Vtb_us_top___024root.h"
#include "verilated.h"
#include "verilated_save.h"

static Vtb_us_top *top;
static int fails, oks;

static void ck(bool c, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
#include <cstdarg>
static void ck(bool c, const char *fmt, ...)
{
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    if (c) { oks++; printf("   ok: %s\n", b); }
    else { fails++; printf("FAIL: %s\n", b); }
}
#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } else oks++; } while (0)
#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } else oks++; } while (0)

// ---- clocks: C64 31.5278 MHz and the RISC-V's 100 MHz; ui_clk (the cartridge's DDR2 side) stays low ----
// The 100 MHz domain (ultimate_sys) runs only while an app access is made (cpu_on): it has nothing to
// do in between, and a Verilator eval per clock edge of it would make the C64's start 20 times slower.
static uint64_t now_ps, clk_next, cpu_next, ticks, c2_next = ~0ull;
static bool cpu_on;
static const uint64_t CLK_HALF = 15859, CPU_HALF = 5000, NEVER = ~0ull;
static void step()
{
    uint64_t t = std::min(std::min(clk_next, cpu_on ? cpu_next : NEVER), c2_next);
    now_ps = t;
    // clk2x rises with every clk edge and falls halfway (T29)
    if (c2_next == t) { top->clk2x = 0; c2_next = NEVER; top->eval(); }
    if (cpu_on && cpu_next == t) { top->cpu_clk = !top->cpu_clk; top->eval(); cpu_next += CPU_HALF; }
    if (clk_next == t) { top->clk = !top->clk; top->clk2x = 1; c2_next = t + CLK_HALF / 2; top->eval(); clk_next += CLK_HALF; }
}
// one clk period: a rising edge, then the falling one (the gate's CE is taken there)
static void tick()
{
    while (!top->clk) step();
    while (top->clk) step();
    ticks++;
}
static void run_ticks(uint64_t n) { for (uint64_t i = 0; i < n; i++) tick(); }
static void run_ms(double ms) { run_ticks(uint64_t(ms * 31527.778)); }
static const uint32_t FRAME = 628992;

// ---- the C64 RAM's second port (JTAG's: it goes first) ----------------------------------
static uint8_t peek(uint16_t a)
{
    top->dbg_addr = a; top->dbg_re = 1;
    tick();
    top->dbg_re = 0;
    uint8_t v = top->dbg_rdata;
    tick();
    return v;
}
static void poke(uint16_t a, uint8_t v)
{
    top->dbg_addr = a; top->dbg_wdata = v; top->dbg_we = 1;
    tick();
    top->dbg_we = 0;
}
static std::vector<uint8_t> ram()
{
    std::vector<uint8_t> m(65536);
    for (int a = 0; a < 65536; a++) m[a] = peek(uint16_t(a));
    return m;
}

// ---- the app's side: byte accesses on ultimate_sys's I/O bus, made by the BFM -------------
// (io address = CPU address - 0x10000000)
#define BFM(x) (top->rootp->tb_us_top__DOT__ult__DOT__i_cpu__DOT__x_##x)
static uint8_t io(uint32_t addr, bool we, uint8_t d)
{
    cpu_on = true;
    cpu_next = now_ps + CPU_HALF;
    run_ticks(4);                               // the synchronizers of ultimate_sys see the C64 side's state
    BFM(addr) = addr; BFM(wdata) = d; BFM(we) = we; BFM(done) = 0; BFM(go) = 1;
    for (int n = 0; n < 32 * 400000; n++) {
        tick();
        if (BFM(done)) {
            uint8_t v = BFM(rdata);
            run_ticks(1);
            cpu_on = false;
            return v;
        }
    }
    printf("FAIL: io access %06X got no ack\n", addr);
    exit(4);
}
enum { IO_C64_MODE = 0x040000, IO_C64_STOP = 0x040001, IO_MEMONLY = 0x180003, IO_WIN = 0x050000,
       IO_KEYB_JOY = 0x100406, IO_KEYB_COL = 0x10040A, IO_KEYB_ROW = 0x10040B };
static void c64_stop(bool on) { io(IO_C64_STOP, true, on); }
static void c64_ultimax(bool on) { io(IO_C64_MODE, true, on ? 2 : 0); }
static void memonly(bool on) { io(IO_MEMONLY, true, on); }
// the window: what the app's C64 memory accesses are
static uint8_t rd(uint16_t a) { return io(IO_WIN + a, false, 0); }
static void wr(uint16_t a, uint8_t v) { io(IO_WIN + a, true, v); }

// ---- trx_sys: JTAG words, the app's bytes -----------------------------------------------
static uint32_t jrd(uint16_t w)
{
    top->j_addr = w; top->j_re = 1;
    tick();
    top->j_re = 0;
    for (int n = 0; n < 1000; n++) {
        tick();
        if (top->j_rvalid) return top->j_rdata;
    }
    printf("FAIL: JTAG read %03X\n", w);
    exit(4);
}
static void jwr(uint16_t w, uint32_t v)
{
    top->j_addr = w; top->j_wdata = v; top->j_we = 1;
    tick();
    top->j_we = 0;
    run_ticks(8);
}
static uint8_t page_now = 0xFF;
static void jpage(uint8_t p) { if (p != page_now) { jwr(0x400, p); page_now = p; } }
static uint32_t treg(uint8_t p, uint16_t off) { jpage(p); return jrd(off >> 2); }
static void twr(uint8_t p, uint16_t off, uint32_t v) { jpage(p); jwr(off >> 2, v); }
static const int NWORDS[5] = {1, 3, 12, 5, 5};
static std::vector<uint32_t> sb_dump()
{
    std::vector<uint32_t> w;
    for (int c = 0; c < 5; c++) {
        twr(1, 0x000, c << 7);
        for (int i = 0; i < NWORDS[c]; i++) w.push_back(treg(1, 0x004));
    }
    return w;
}
static uint32_t sb_word(int chip, int word) { twr(1, 0x000, chip << 7 | word); return treg(1, 0x004); }
static uint8_t sb_vic(int reg) { return sb_word(2, reg >> 2) >> (8 * (reg & 3)); }
static uint8_t sb_cia(int cia, int reg) { return sb_word(cia == 1 ? 3 : 4, reg >> 2) >> (8 * (reg & 3)); }
static const char *WNAME[] = {"glue", "6510", "VIC", "CIA1", "CIA2"};
static int sb_diff(const std::vector<uint32_t> &a, const std::vector<uint32_t> &b, const char *what, bool skip_glue = true)
{
    int d = 0, i = 0;
    for (int c = 0; c < 5; c++)
        for (int k = 0; k < NWORDS[c]; k++, i++)
            if (a[i] != b[i] && !(c == 0 && skip_glue)) {
                if (d++ < 8) printf("   %s: %s w%d %08X vs %08X\n", what, WNAME[c], k, a[i], b[i]);
            }
    return d;
}
// ---- the debug registers (trx_dbg.v) and page 5 (dma_win.v, trx_sys.v) ---------------------
enum { CMD_PAUSE_CB = 1, CMD_PAUSE_IB = 2, CMD_RUN = 3, CMD_STEP_I = 5, CMD_STEP_C = 6 };
static void dcmd(uint32_t c) { twr(3, 0x000, c); }
static uint32_t dstat() { return treg(3, 0x008); }
static unsigned dstops() { return dstat() >> 24; }
static unsigned dreason() { return (dstat() >> 8) & 15; }
static uint32_t cycles() { return treg(0, 0x120); }
static uint32_t instrs() { return treg(0, 0x128); }
static void own(bool on) { twr(0, 0x118, on); }
static void bmp_set(uint16_t a)
{
    twr(3, 0x040, a >> 5);
    twr(3, 0x044, 1u << (a & 31));
    twr(3, 0x048, 1);
}
static uint32_t sh_status() { return treg(5, 0x004); }

// ---- save / restore -----------------------------------------------------------------------
static void model_reset() { now_ps = 0; clk_next = CLK_HALF; cpu_next = CPU_HALF; cpu_on = false; }
static void save(const char *name)
{
    VerilatedSave os;
    os.open(name);
    os << ticks << page_now << *top;
    os.close();
}
static void restore(const char *name)
{
    VerilatedRestore is;
    is.open(name);
    is >> ticks >> page_now >> *top;
    is.close();
    top->clk = 0; top->clk2x = 0; c2_next = NEVER; top->ui_clk = 0; top->cpu_clk = 0;
    top->ign_dbg = getenv("ULTSYS_NEGATIVE") != nullptr;   // control run: the bench must FAIL
    top->eval();
    model_reset();
}

// ---- machine control ------------------------------------------------------------------
static void wait_halted(bool on)
{
    for (int n = 0; n < 3 * FRAME; n++) {
        if (top->halted == on) { tick(); tick(); return; }
        tick();
    }
    printf("FAIL: machine %s\n", on ? "did not stop" : "did not go on");
    exit(4);
}
static void run_to_gclk(uint32_t g)
{
    while (top->gclk_edges < g) tick();
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
static void type(const char *t)
{
    for (int i = 0; t[i]; i++) poke(0x0277 + i, uint8_t(t[i]));
    poke(0x00C6, uint8_t(strlen(t)));
}

static void init()
{
    model_reset();
    top->rst = 1; top->ui_rst = 1; top->cpu_rst = 1;
    cpu_on = true;                              // ultimate_sys's reset needs its clock
    top->dbg_re = 0; top->dbg_addr = 0; top->dbg_we = 0; top->dbg_wdata = 0;
    top->jtag_halt = 0; top->jtag_cpu_only = 0; top->ign_dbg = 0;
    top->j_re = 0; top->j_we = 0; top->j_addr = 0; top->j_wdata = 0;
    top->ult_keys = 0; top->ult_joys = 0; top->ult_restore = 0;
    for (int i = 0; i < 64; i++) tick();
    top->rst = 0; top->ui_rst = 0; top->cpu_rst = 0;
    run_ticks(16);
    cpu_on = false;
    io(IO_C64_MODE, true, 0x08);                // the app releases the C64's reset (it is held from power on)
}

static int boot(const char *state)
{
    init();
    bool ready = false;
    for (int n = 0; n < 400 && !ready; n++) {
        for (int k = 0; k < 16; k++) {
            run_ticks(20000);
            if (getenv("ULTSYS_VERBOSE")) { printf("."); fflush(stdout); }
        }
        if (getenv("ULTSYS_VERBOSE")) printf("   %llu clocks, gclk %u, halted %d, app_stop %d, row0 '%s'\n", (unsigned long long)ticks, top->gclk_edges, top->halted, top->app_stop_o, row(0).c_str());
        for (int r = 0; r < 25 && !ready; r++)
            if (row(r).rfind("READY.", 0) == 0) ready = true;
    }
    if (!ready) { printf("no READY\n"); return 3; }
    run_ticks(640000);
    save(state);
    printf("READY after %llu clocks\n", (unsigned long long)ticks);
    return 0;
}

// ---- the programs ----------------------------------------------------------------------
// X at $C000: SEI, the CIA 1/2 interrupts masked with their flags read away, the VIC's raster
// interrupt enabled for line $30 and never acknowledged: CIA 1's ICR flag (timer A) and $D019
// b0 are set whenever the machine stops, reading them in the stop must not clear them
static const uint16_t X_LOOP = 0xC021;
static const uint8_t PROG_X[] = {
    0x78,                                       // C000 SEI
    0xA9, 0x7F, 0x8D, 0x0D, 0xDC, 0xAD, 0x0D, 0xDC,   // CIA 1: mask, read
    0xA9, 0x7F, 0x8D, 0x0D, 0xDD, 0xAD, 0x0D, 0xDD,   // CIA 2
    0xA9, 0x30, 0x8D, 0x12, 0xD0,                     // raster line $30
    0xA9, 0x01, 0x8D, 0x1A, 0xD0,                     // raster irq enabled
    0xA9, 0x01, 0x8D, 0x19, 0xD0,                     // C01C: acknowledge once
    0xEA,
    0xEE, 0xF0, 0xC0,                                 // C021 loop: INC $C0F0
    0x4C, 0x21, 0xC0,                                 // JMP loop
};
// Q: a raster IRQ program. Two interrupts per frame (lines $50 and $C8), the handler at $C100
// logs {$D012, $DC04} per interrupt into $C200.. and re-arms the other line
static void install(uint16_t at, const uint8_t *p, size_t n) { for (size_t i = 0; i < n; i++) poke(at + i, p[i]); }

// ---- the machine at a stop -------------------------------------------------------------
// wait for the next stop record after `stops0` and the debugger's stop being in force
static void wait_stop(unsigned stops0)
{
    for (uint64_t t0 = ticks; !(dstops() != stops0 && top->dbg_stop_o);) {
        run_ticks(4);
        if (ticks - t0 > 6ull * FRAME) { printf("FAIL: no stop\n"); exit(4); }
    }
}
static void settle() { run_ticks(40); }

static const char *MID = "us_mid.state";
static std::vector<uint8_t> window(uint16_t lo, uint16_t hi)
{
    std::vector<uint8_t> v;
    for (uint32_t a = lo; a <= hi; a++) v.push_back(rd(uint16_t(a)));
    return v;
}
// the debugger's stop at the loop of program X; C64_STOP stays 0
static void dbg_stop_at_loop()
{
    restore(MID);
    unsigned s0 = dstops();
    bmp_set(X_LOOP);
    wait_stop(s0);
    CHECK(!top->app_stop_o, "C64_STOP is set in a debugger stop of this bench");
}
// what an app freeze (C64_STOP + Ultimax, through ultimate_sys) of the machine at the clock edge g shows
static void freeze_at(uint32_t g)
{
    restore(MID);
    twr(0, 0x100, 1);                           // GATE_CTRL b0: C64_STOP pauses the whole machine
    run_to_gclk(g - 16);
    c64_stop(true);
    wait_halted(true);
    CHECK(top->gclk_edges == g, "the app's freeze is on clock edge %u, the debugger's stop on %u", top->gclk_edges, g);
    c64_ultimax(true);
}

// ---- 1. the D page through the window in a debugger stop = the app freeze's -----------------
static void test_window()
{
    dbg_stop_at_loop();
    uint32_t g = top->gclk_edges;
    ck(sh_status() == 0, "SH_STATUS %08X: no session", sh_status());
    uint32_t cy0 = cycles(), dma0 = top->dma_count;
    std::vector<uint8_t> a = window(0xD000, 0xDFFF);
    uint32_t dma1 = top->dma_count;
    ck(dma1 - dma0 == 3072, "ultimate_sys handed over %u bytes ($D400-$D7FF reads $FF there: no SID)", dma1 - dma0);
    ck(cycles() - cy0 >= 3072, "window reads of the D page are bus cycles (CYCLES +%u)", cycles() - cy0);
    freeze_at(g);
    std::vector<uint8_t> b = window(0xD000, 0xDFFF);
    int d = 0;
    for (size_t i = 0; i < a.size(); i++) {
        // colour RAM is 4 bits wide: the high nibble of a read is the open bus, whatever the mapping left on it;
        // IO1/IO2 ($DE00-$DFFF) have no cartridge here: the bus floats, and what it shows is the last data on it
        uint8_t m = (i >= 0x800 && i < 0xC00) ? 0x0F : (i >= 0xE00) ? 0x00 : 0xFF;
        if ((a[i] & m) != (b[i] & m) && d++ < 6) printf("   $%04zX: stop %02X, freeze %02X\n", 0xD000 + i, a[i], b[i]);
    }
    ck(d == 0, "4096 D-page bytes of the debugger's stop equal the app's freeze on the same edge (colour RAM: low nibble, IO1/IO2 open bus left out) (%d differ)", d);
    int cram_e = 0;
    for (int i = 0; i < 0x400; i++) cram_e += (a[0x800 + i] & 0x0F) == 0x0E;
    printf("   $D800 %02X, $D020 %02X, $D021 %02X, $DC0D %02X, $D012 %02X\n", a[0x800], a[0x020], a[0x021], a[0xC0D], a[0x012]);
    ck(cram_e >= 900, "colour RAM $D800-$DBFF reads the real values (%d of 1024 light blue)", cram_e);
    std::vector<int> seen(256);
    for (uint8_t v : a) seen[v] = 1;
    int distinct = 0;
    for (int i : seen) distinct += i;
    ck(distinct > 8, "the window shows chip registers (%d distinct values)", distinct);
}

// ---- 2. flags and side effects ----------------------------------------------------------
static void test_flags()
{
    dbg_stop_at_loop();
    std::vector<uint32_t> w0 = sb_dump();
    uint32_t cy0 = cycles();
    uint8_t icr_sb = sb_cia(1, 0xD), d019_sb = sb_vic(0x19);
    for (int i = 0; i < 6; i++) { sb_cia(1, 0xD); sb_vic(0x19); }
    std::vector<uint32_t> w1 = sb_dump();
    ck(cycles() == cy0, "state bus looks cost no machine cycle (CYCLES +%u)", cycles() - cy0);
    ck(sb_diff(w0, w1, "before/after looks") == 0, "$DC0D, $D019 looked at over the state bus: the words are as they were");
    ck((icr_sb & 1) && (d019_sb & 1), "ICR flag %02X and $D019 flag %02X are there", icr_sb, d019_sb);
    uint32_t c0 = cycles();
    uint8_t o1 = rd(0xDC0D), o2 = rd(0xDC0D), o3 = rd(0xDC0D);
    uint32_t c1 = cycles();
    printf("   window: ICR reads %02X %02X %02X, CYCLES +%u\n", o1, o2, o3, c1 - c0);
    ck((o1 & 1) && !(o3 & 1), "the window's reads clear the ICR flag (%02X %02X %02X)", o1, o2, o3);
    ck(c1 - c0 >= 3, "window reads of the D page are machine cycles (+%u for 3)", c1 - c0);
}

// ---- 3. writes in the stop --------------------------------------------------------------
static void test_writes()
{
    dbg_stop_at_loop();
    uint8_t b0 = rd(0xD020), v0 = rd(0xD021);
    uint8_t want20 = uint8_t(b0 + 5) & 0x0F, want21 = uint8_t(v0 + 1) & 0x0F;
    wr(0xD020, want20);
    wr(0xD021, want21);
    wr(0xD800, 0x05);
    wr(0xDBE7, 0x09);
    ck((sb_vic(0x20) & 0x0F) == want20 && (sb_vic(0x21) & 0x0F) == want21, "$D020/$D021 are in the chip at once (%02X, %02X)", sb_vic(0x20), sb_vic(0x21));
    ck((rd(0xD800) & 0x0F) == 5 && (rd(0xDBE7) & 0x0F) == 9, "colour RAM $D800/$DBE7 read back ($%02X, $%02X)", rd(0xD800), rd(0xDBE7));
    CHECK(!top->app_stop_o, "C64_STOP is set");
    // RUN: the machine goes on
    twr(3, 0x048, 0);                           // the bitmap off, or the loop stops again at once
    dcmd(CMD_RUN);
    bool b6 = false, shut0 = false;
    for (uint64_t t0 = ticks; ticks - t0 < 3ull * FRAME && !shut0;) {      // the resume waits for frame alignment
        uint32_t s = dstat();
        if ((s & 0x41) == 0x41) b6 = true;
        if (!(s & 1)) shut0 = true;
    }
    ck(b6 && shut0, "after RUN the gate stays shut while the hold is off (b6), then opens");
    run_ticks(4000);
    ck(!top->dbg_hold_o, "after RUN: no hold");
    uint8_t cnt = peek(0xC0F0);
    run_ticks(20000);
    ck(peek(0xC0F0) != cnt, "the program goes on after RUN");
}

// ---- 4. MEMONLY ---------------------------------------------------------------------------
static void test_memonly()
{
    dbg_stop_at_loop();
    CHECK(!top->app_stop_o, "C64_STOP is set");
    uint32_t c0 = cycles();
    uint8_t o1 = rd(0xDC0D);
    ck(cycles() > c0, "without MEMONLY the D page is a bus cycle (CYCLES +%u, ICR read %02X)", cycles() - c0, o1);
    ck(top->dbg_stop_o, "the debugger's stop holds through the window's bus cycles");
    poke(0xD020, 0xA7);
    memonly(true);
    uint32_t c2 = cycles();
    ck(rd(0xD020) == 0xA7, "MEMONLY: $D020 reads the RAM under the I/O (%02X)", rd(0xD020));
    ck(cycles() == c2, "MEMONLY: no machine cycle");
    memonly(false);
}

// ---- 5. KEYB_COL / KEYB_ROW / KEYB_JOY ---------------------------------------------------------
static void test_keyb()
{
    restore(MID);
    io(IO_KEYB_COL, true, 0xFE);
    ck(io(IO_KEYB_ROW, false, 0) == 0xFF, "no key: KEYB_ROW %02X", io(IO_KEYB_ROW, false, 0));
    top->ult_keys = 1ull << (1 * 8 + 2);        // A: column 1, row 2
    io(IO_KEYB_COL, true, 0xFD);
    uint8_t a = io(IO_KEYB_ROW, false, 0);
    io(IO_KEYB_COL, true, 0xFE);
    uint8_t other = io(IO_KEYB_ROW, false, 0);
    io(IO_KEYB_COL, true, 0x00);
    uint8_t all = io(IO_KEYB_ROW, false, 0);
    ck(a == 0xFB && other == 0xFF && all == 0xFB, "A in column 1: KEYB_ROW %02X (column 0 selected: %02X, all columns: %02X)", a, other, all);
    top->ult_keys = 0;
    io(IO_KEYB_COL, true, 0xFF);
    ck(io(IO_KEYB_JOY, false, 0) == 0xFF, "no joystick: KEYB_JOY %02X", io(IO_KEYB_JOY, false, 0));
    top->ult_joys = 0x01 << 5;                  // port 2 up
    uint8_t p2 = io(IO_KEYB_JOY, false, 0);
    io(IO_KEYB_JOY, true, 1);                   // the line reads port 1 now
    uint8_t p2_on_p1 = io(IO_KEYB_JOY, false, 0);
    top->ult_joys = 0x10;                       // port 1 fire
    uint8_t p1 = io(IO_KEYB_JOY, false, 0);
    io(IO_KEYB_JOY, true, 0);
    uint8_t p1_on_p2 = io(IO_KEYB_JOY, false, 0);
    top->ult_joys = 0;
    ck(p2 == 0xFE && p2_on_p1 == 0xFF && p1 == 0xEF && p1_on_p2 == 0xFF,
       "KEYB_JOY: port 2 up %02X, port 2 up read on port 1 %02X, port 1 fire %02X, read on port 2 %02X", p2, p2_on_p1, p1, p1_on_p2);
}

// ---- 7. T62: RESTORE level, scan lines bit, the debug register ($D7FF) -------------------------
static const uint8_t PROG_D7[] = {              // at $C100: $D7FF <- $C3, mark $C0F1 = $7E, wait for $C0F2, $D7FF <- $11, mark $7F
    0xA9, 0xC3, 0x8D, 0xFF, 0xD7,               // C100 LDA #$C3, STA $D7FF
    0xA9, 0x7E, 0x8D, 0xF1, 0xC0,               // C105 first mark
    0xAD, 0xF2, 0xC0, 0xF0, 0xFB,               // C10A wait: LDA $C0F2, BEQ wait
    0xA9, 0x11, 0x8D, 0xFF, 0xD7,               // C10F LDA #$11, STA $D7FF
    0xA9, 0x7F, 0x8D, 0xF1, 0xC0,               // C114 second mark
    0x4C, 0x19, 0xC1,                           // C119 loop
};
static void test_t62_regs()
{
    restore(MID);
    enum { IO_RESTORE = 0x100402, IO_SCAN = 0x180000, IO_DEBUG = 0x181800 };
    ck(io(IO_RESTORE, false, 0) == 0, "RESTORE released: U64_RESTORE_REG %02X", io(IO_RESTORE, false, 0));
    top->ult_restore = 1;
    ck(io(IO_RESTORE, false, 0) == 1, "RESTORE held: U64_RESTORE_REG %02X (the app tests == 1)", io(IO_RESTORE, false, 0));
    top->ult_restore = 0;
    ck(io(IO_RESTORE, false, 0) == 0, "RESTORE released again");
    ck(top->ult_scan_o == 0, "scanlines off after reset");
    io(IO_SCAN, true, 1);
    ck(top->ult_scan_o == 1, "C64_SCANLINES = 1 -> scanlines");
    io(IO_SCAN, true, 0);
    ck(top->ult_scan_o == 0, "C64_SCANLINES = 0 -> no scanlines");
    ck(io(IO_DEBUG, false, 0) == 0, "debug register starts at 0 (%02X)", io(IO_DEBUG, false, 0));
    io(IO_DEBUG, true, 0x5A);
    ck(io(IO_DEBUG, false, 0) == 0x5A, "the app's write to the debug register reads back (%02X)", io(IO_DEBUG, false, 0));
    // the C64 itself: program X runs at $C000; its loop is sent to $C100
    install(0xC100, PROG_D7, sizeof PROG_D7);
    poke(0xC0F1, 0); poke(0xC0F2, 0);
    poke(0xC025, 0x00);                         // X's JMP $C021 becomes JMP $C000 (X again, harmless) ...
    poke(0xC026, 0xC1);                         // ... and then JMP $C100
    run_ms(5);
    ck(peek(0xC0F1) == 0x7E, "the C64 program ran to its first mark (%02X)", peek(0xC0F1));
    ck(top->c64_d7ff_seen, "c64_core saw the write to $D7FF");
    // the 100 MHz clock of ultimate_sys runs only during an access here, so the two writes are taken one at a time
    uint8_t v = io(IO_DEBUG, false, 0);
    ck(v == 0xC3, "debug register after the C64's write of $C3 = %02X (the app's $5A is gone)", v);
    poke(0xC0F2, 1);
    run_ms(2);
    ck(peek(0xC0F1) == 0x7F, "the C64 program ran to its second mark (%02X)", peek(0xC0F1));
    v = io(IO_DEBUG, false, 0);
    ck(v == 0x11, "debug register after the C64's second write $11 = %02X", v);
    io(IO_DEBUG, true, 0x33);
    ck(io(IO_DEBUG, false, 0) == 0x33, "the app writes after the C64: %02X", io(IO_DEBUG, false, 0));
}

// ---- 6. the app's freeze: no bus cycle ------------------------------------------------------
static void test_freeze_nocycle()
{
    restore(MID);
    twr(0, 0x100, 1);                           // GATE_CTRL b0: C64_STOP pauses the whole machine
    c64_stop(true);
    wait_halted(true);
    memonly(true);                              // C64::freeze_slim
    uint32_t g0 = top->gclk_edges, cy0 = cycles();
    std::vector<uint8_t> cb(1024), rb(1024);
    for (int i = 0; i < 1024; i++) cb[i] = io(0x18F000 + i, false, 0);
    for (int i = 0; i < 1024; i++) rb[i] = rd(uint16_t(0x0400 + i));
    for (int i = 0; i < 1024; i++) { io(0x18F000 + i, true, uint8_t(i % 15 + 1)); wr(uint16_t(0x0400 + i), uint8_t(i * 7)); }
    int bad = 0;
    for (int i = 0; i < 1024; i++) {
        bad += (io(0x18F000 + i, false, 0) & 0x0F) != (i % 15 + 1);
        bad += rd(uint16_t(0x0400 + i)) != uint8_t(i * 7);
    }
    ck(bad == 0, "the menu through the RAM port and the colour RAM aperture reads back (%d wrong)", bad);
    ck(top->gclk_edges == g0 && cycles() == cy0, "no machine cycle: C64 clock edges +%u, CYCLES +%u", top->gclk_edges - g0, cycles() - cy0);
    ck((cb[0] & 0xF0) == 0xF0, "colour RAM read gives $Fx (%02X)", cb[0]);
    for (int i = 0; i < 1024; i++) { io(0x18F000 + i, true, cb[i] & 0x0F); wr(uint16_t(0x0400 + i), rb[i]); }
    memonly(false);
    c64_stop(false);
    wait_halted(false);
}

static int run(const char *state)
{
    restore(state);
    uint32_t id = treg(0, 0x000);
    ck(id == 0x32585254, "ID %08X", id);
    install(0xC000, PROG_X, sizeof PROG_X);
    type("SYS49152\r");
    run_ms(100);
    uint8_t c0 = peek(0xC0F0);
    run_ms(5);
    ck(peek(0xC0F0) != c0, "program X runs");
    save(MID);
    const char *only = getenv("ULTSYS_ONLY");
    auto on = [&](char c) { return !only || strchr(only, c); };
    if (on('1')) { printf("1. D page through ultimate_sys in a debugger stop = the app freeze's\n"); test_window(); }
    if (on('2')) { printf("2. flags\n"); test_flags(); }
    if (on('3')) { printf("3. writes\n"); test_writes(); }
    if (on('4')) { printf("4. MEMONLY\n"); test_memonly(); }
    if (on('6')) { printf("6. the app's freeze through ultimate_sys: no machine cycle\n"); test_freeze_nocycle(); }
    if (on('7')) { printf("7. RESTORE level, scan lines bit, debug register $D7FF\n"); test_t62_regs(); }
    if (on('5')) { printf("5. KEYB_COL / KEYB_ROW / KEYB_JOY\n"); test_keyb(); }
    printf("%d checks ok, %d failed\n", oks, fails);
    printf(fails ? "FAILED\n" : "PASS\n");
    return fails ? 1 : 0;
}

int main(int argc, char **argv)
{
    Verilated::commandArgs(argc, argv);
    top = new Vtb_us_top;
    if (argc >= 2 && !strcmp(argv[1], "speed")) {       // clocks per second of wall time
        init();
        clock_t t0 = clock();
        run_ticks(20000);
        printf("20000 clocks in %.2f s\n", double(clock() - t0) / CLOCKS_PER_SEC);
        return 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "boot")) return boot(argv[2]);
    if (argc >= 3 && !strcmp(argv[1], "run")) return run(argv[2]);
    printf("usage: tb_ultsys boot <state> | run <state>\n");
    return 2;
}
