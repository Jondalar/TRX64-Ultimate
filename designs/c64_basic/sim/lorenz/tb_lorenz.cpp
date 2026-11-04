// SPDX-License-Identifier: GPL-3.0-or-later
// c64_core under Verilator, for the Lorenz 6510/CIA tests (tests/lorenz.py
// does the same on the board over JTAG).
//
//   tb_lorenz boot <state>                 cold start to READY, save the model
//   tb_lorenz run <state> <prg> <cycles> [wait]
//                                          restore, run <wait> cycles (moves the
//                                          start against the KERNAL's timer IRQ),
//                                          load the PRG, type RUN, and run until
//                                          the test passes, fails or the C64
//                                          cycle budget is spent
//
// Pass: the test loads the next one from drive 8, which is not there ("LOAD
// ERROR"). Fail: it turns the border light red ($D020 = 10, with $D7FF = $FF
// for VICE), prints what went wrong and waits for a key. The green of a pass
// is no sign: waitborder decrements $D020 from 14 to 13 while it waits. Exit code 0 = ok,
// 1 = FAIL, 2 = timeout. The screen rows 4-24 are printed in every case.
// Environment: FILLRAM=seed fills the RAM from $0801 with random bytes first;
// KEEPIO=1 leaves the RAM under $D000-$DFFF as it is (it is cleared by default).
// VMODE=0..3 (T40 System Mode: PAL, NTSC, PAL-60, NTSC-50) for boot.
//
//   tb_lorenz modes <state> [new mode]     (T40) restore a booted state, start a
//                                          probe at $C000 (TOD from 0, a loop
//                                          counter, copies of $D018/$D020/$D011)
//                                          and check the VIC frame (lines, dot4x
//                                          clocks per line and frame), $02A6, the
//                                          jiffy clock and the TOD rate against
//                                          the mode; with a new mode switch to it
//                                          live (c64_core restarts the VIC's
//                                          timing) and check it all again, plus
//                                          that the program and the VIC registers
//                                          went on unchanged
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "Vc64_core.h"
#include "verilated.h"
#include "verilated_save.h"
#ifdef TRACE
#include "verilated_vcd_c.h"
#include "Vc64_core___024root.h"
#endif

static Vc64_core *top;
static uint64_t clocks;
static int border = -1;            // colour of one pixel in the top border, sampled every frame
#ifdef TRACE
static VerilatedVcdC *tfp;
static uint64_t trace_from = ~0ull, trace_to = 0;
static FILE *cpulog;                // CPULOG=from,to: one line per CPU cycle (trace build only)
static uint64_t log_from = ~0ull, log_to = 0;
static bool phi_q;
#endif

static void tick()
{
    // clk2x (the SID engine's clock, T25.1): rises with clk and in its middle
    top->clk = 1; top->gclk = 1; top->snd_clk = 1; top->cart_wclk = 1; top->clk2x = 1;
    top->eval();
    top->clk2x = 0;
    top->eval();
#ifdef TRACE
    if (tfp && clocks >= trace_from && clocks < trace_to) tfp->dump(clocks * 2);
    if (cpulog && clocks >= log_from && clocks < log_to) {
        auto *r = top->rootp;
        bool phi = r->c64_core__DOT__c64__DOT__cpu6510_0__DOT__phi2_l;
        if (phi && !phi_q) {
            bool we = r->c64_core__DOT__c64__DOT__cpu6510_0__DOT__we;
            fprintf(cpulog, "%llu %04X %c %02X%s\n", (unsigned long long)(clocks / 32),
                    r->c64_core__DOT__c64__DOT__cpu6510_0__DOT__addr, we ? 'W' : 'r',
                    we ? r->c64_core__DOT__c64__DOT__cpu6510_0__DOT__dout
                       : r->c64_core__DOT__c64__DOT__cpu6510_0__DOT__din_i,
                    r->c64_core__DOT__c64__DOT__cpu6510_0__DOT__rdy_latched ? "" : " (not ready)");
        }
        phi_q = phi;
    }
#endif
    top->clk = 0; top->gclk = 0; top->snd_clk = 0; top->cart_wclk = 0; top->clk2x = 1;
    top->eval();
    top->clk2x = 0;
    top->eval();
    if (top->pix_y == 40 && top->pix_x == 200) border = top->pix_color;
#ifdef TRACE
    if (tfp && clocks >= trace_from && clocks < trace_to) tfp->dump(clocks * 2 + 1);
#endif
    clocks++;
}

// 32 clocks of the 31.5 MHz dot4x clock per PAL C64 cycle
static void run_cycles(uint64_t n) { for (uint64_t i = 0; i < n * 32; i++) tick(); }

// through c64_mem's debug port, like trxdbg over JTAG
static uint8_t peek(uint16_t a)
{
    top->dbg_addr = a; top->dbg_req = 1;
    tick(); tick();
    top->dbg_req = 0;
    return top->dbg_rdata;
}

static void poke(uint16_t a, uint8_t v)
{
    top->dbg_addr = a; top->dbg_wdata = v; top->dbg_we = 1; top->dbg_req = 1;
    tick();
    top->dbg_we = 0; top->dbg_req = 0;
}

static std::string row(int r)
{
    std::string s;
    for (int i = 0; i < 40; i++) {
        uint8_t c = peek(0x0400 + r * 40 + i) & 0x7F;
        if (c == 0) s += '@';
        else if (c < 27) s += char('A' + c - 1);
        else if (c < 64) s += char(c == 0x1C ? '\\' : c == 0x1E ? '^' : c == 0x1F ? '_' : c);
        else s += '.';
    }
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

static void show(int from = 4)
{
    for (int r = from; r < 25; r++) {
        std::string s = row(r);
        if (!s.empty()) printf("  | %s\n", s.c_str());
    }
}

static int vmode()
{
    const char *v = getenv("VMODE");
    return v ? atoi(v) & 3 : 0;
}

static void init()
{
    top->vmode = vmode();
    top->rst = 1; top->snd_rst = 1; top->audio_sample_en = 0;
    top->keys = 0; top->joy1 = 0; top->joy2 = 0; top->restore = 0;
    top->dbg_addr = 0; top->dbg_we = 0; top->dbg_wdata = 0;
    top->reu_en = 1; top->reu_size = 5; top->reu_cmd_ready = 1; top->reu_rdata = 0; top->reu_rdata_valid = 0;
    top->cpu_reset_req = 0; top->cpu_pause_req = 0;
    top->cart_type = 0; top->cart_kill_tgl = 0; top->cart_force_tgl = 0; top->cart_ack_tgl = 0;
    top->cart_fill_be = 0; top->cart_fill_addr = 0; top->cart_fill_data = 0;
    top->dma_req = 0; top->dma_addr = 0; top->dma_we = 0; top->dma_wdata = 0; top->ultimax = 0;
    top->iec_atn_i = 1; top->iec_clk_i = 1; top->iec_data_i = 1;
    for (int i = 0; i < 64; i++) tick();
    top->rst = 0; top->snd_rst = 0;
}

static int boot(const char *state)
{
    init();
    for (int n = 0; n < 400; n++) {         // up to 4 M cycles
        run_cycles(10000);
        for (int r = 0; r < 25; r++)
            if (row(r).rfind("READY.", 0) == 0) {
                run_cycles(20000);
                VerilatedSave os;
                os.open(state);
                os << clocks << *top;
                printf("READY after %llu cycles\n", (unsigned long long)(clocks / 32));
                return 0;
            }
    }
    printf("no READY\n");
    show(0);
    return 3;
}

static int run(const char *state, const char *prgfile, uint64_t budget, uint64_t wait)
{
    VerilatedRestore is;
    is.open(state);
    is >> clocks >> *top;
#ifdef TRACE
    if (tfp) { trace_from += clocks; trace_to += clocks; }   // the windows count from here
    if (cpulog) { log_from += clocks; log_to += clocks; }
#endif
    run_cycles(wait);
    // FILLRAM=seed: random RAM from $0801 up, as earlier programs leave it on
    // the board. The RAM under the I/O area is cleared as tests/lorenz.py does
    // (TRAP16/17's waitborder reads $D011/$D012 there with $01 = all RAM).
    if (const char *f = getenv("FILLRAM")) {
        srand(atoi(f));
        for (unsigned a = 0x0801; a < 0x10000; a++) poke(uint16_t(a), uint8_t(rand()));
    }
    if (!getenv("KEEPIO"))
        for (unsigned a = 0xD000; a < 0xE000; a++) poke(uint16_t(a), 0);
    FILE *f = fopen(prgfile, "rb");
    if (!f) { perror(prgfile); return 3; }
    std::vector<uint8_t> prg;
    int c;
    while ((c = fgetc(f)) != EOF) prg.push_back(uint8_t(c));
    fclose(f);
    uint16_t load = prg[0] | prg[1] << 8, end = uint16_t(load + prg.size() - 2);
    for (size_t i = 2; i < prg.size(); i++) poke(uint16_t(load + i - 2), prg[i]);
    for (uint16_t zp : {0x2D, 0x2F, 0x31}) { poke(zp, end & 0xFF); poke(zp + 1, end >> 8); }
    const char *cmd = "RUN\r";
    for (int i = 0; i < 4; i++) poke(0x0277 + i, uint8_t(cmd[i]));
    poke(0x00C6, 4);
    uint64_t start = clocks / 32;
    int red = 0;
    while (clocks / 32 - start < budget) {
        run_cycles(20000);
        for (int r = 4; r < 25; r++)
            if (row(r).find("LOAD ERROR") != std::string::npos) {
                printf("ok %llu\n", (unsigned long long)(clocks / 32 - start));
                show();
                return 0;
            }
        red = border == 10 ? red + 1 : 0;
        if (red == 10) {                        // red for 10 samples: waiting for a key
            printf("FAIL %llu\n", (unsigned long long)(clocks / 32 - start));
            show();
            return 1;
        }
    }
    printf("timeout %llu\n", (unsigned long long)(clocks / 32 - start));
    show();
    return 2;
}

// ---- T40: System Modes ----------------------------------------------------------
static int fails;
#define EXPECT(c, ...) do { if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)
static bool is60(int m) { return m == 1 || m == 2; }
static const double CLK_HZ[4] = {31527778, 32725694, 32765152, 31584821};

// the VIC's frame as c64_core's pixel stream shows it
static void measure(int &lines, int &line_clk, uint64_t &frame_clk)
{
    while (!(top->pix_y == 0 && top->pix_x == 0)) tick();      // start of a frame
    uint64_t f0 = clocks; int maxy = 0;
    while (top->pix_y == 0) tick();
    uint64_t l0 = 0; line_clk = 0;
    int px_q = top->pix_x;
    while (top->pix_y != 0) {
        if (top->pix_y > maxy) maxy = top->pix_y;
        tick();
        if (top->pix_x == 0 && px_q != 0) { if (l0) line_clk = int(clocks - l0); l0 = clocks; }
        px_q = top->pix_x;
    }
    lines = maxy + 1;
    frame_clk = clocks - f0;
}

static void check_mode(int m, const char *when)
{
    int lines, lclk; uint64_t fclk;
    measure(lines, lclk, fclk);
    printf("%s: mode %d, %d lines, %d clocks per line (%d cycles), %llu clocks per frame\n", when, m, lines,
           lclk, lclk / 32, (unsigned long long)fclk);
    EXPECT(lines == (is60(m) ? 263 : 312), "%s: lines", when);
    EXPECT(lclk == (is60(m) ? 65 : 63) * 32, "%s: clocks per line", when);
    EXPECT(fclk == (is60(m) ? 547040u : 628992u), "%s: clocks per frame", when);
}

// TOD tenths ($C100) and the jiffy clock ($A2) over n clocks
static void rates(int m, int boot_m, const char *when)
{
    // wait for a tenths change, then time the next two
    uint8_t t = peek(0xC100);
    while (peek(0xC100) == t) run_cycles(100);
    uint64_t c0 = clocks; t = peek(0xC100);
    uint8_t j0 = peek(0xA2), loop0 = peek(0xC103);
    while (peek(0xC100) == t) run_cycles(100);
    t = peek(0xC100);
    while (peek(0xC100) == t) run_cycles(100);
    double tenth = double(clocks - c0) / 2;
    uint8_t j1 = peek(0xA2), loop1 = peek(0xC103);
    // CRA bit 7 = 0 (KERNAL): six TOD input ticks per tenth; the input runs at
    // 120/100 toggles per second of the mode's clock
    double want = 6.0 * 2.0 * CLK_HZ[m] / (is60(m) ? 120.0 : 100.0);
    // the KERNAL's jiffy timer: 16421 + 1 (PAL) or 17045 + 1 (NTSC) cycles per IRQ
    double jiffy = double(uint8_t(j1 - j0)) / (double(clocks - c0) / 32.0 / ((boot_m == 1 || boot_m == 2) ? 17046.0 : 16422.0));
    printf("%s: TOD tenth every %.0f clocks (want %.0f), jiffies %.2f of expected, loop went on %d\n", when, tenth, want,
           jiffy, uint8_t(loop1 - loop0));
    EXPECT(tenth > want * 0.995 && tenth < want * 1.005, "%s: TOD rate", when);
    EXPECT(jiffy > 0.9 && jiffy < 1.1, "%s: jiffy clock", when);
    EXPECT(uint8_t(loop1 - loop0) != 0, "%s: the probe loop stopped", when);
}

static int modes(const char *state, int newm)
{
    VerilatedRestore is;
    is.open(state);
    is >> clocks >> *top;
    int m = top->vmode;
    // probe at $C000, started with SYS49152 through the keyboard buffer
    static const uint8_t prog[] = {
        0xA9, 0x00, 0x8D, 0x0B, 0xDC, 0x8D, 0x0A, 0xDC, 0x8D, 0x09, 0xDC, 0x8D, 0x08, 0xDC,
        0xAD, 0x08, 0xDC, 0x8D, 0x00, 0xC1, 0xAD, 0x09, 0xDC, 0x8D, 0x01, 0xC1,
        0xEE, 0x02, 0xC1, 0xD0, 0x03, 0xEE, 0x03, 0xC1,
        0xAD, 0x18, 0xD0, 0x8D, 0x04, 0xC1, 0xAD, 0x20, 0xD0, 0x8D, 0x05, 0xC1,
        0xAD, 0x11, 0xD0, 0x29, 0x7F, 0x8D, 0x06, 0xC1, 0x4C, 0x0E, 0xC0};
    for (size_t i = 0; i < sizeof prog; i++) poke(uint16_t(0xC000 + i), prog[i]);
    const char *cmd = "SYS49152\r";
    for (int i = 0; i < 9; i++) poke(0x0277 + i, uint8_t(cmd[i]));
    poke(0x00C6, 9);
    run_cycles(100000);
    uint8_t pal = peek(0x02A6);
    printf("$02A6 = %d\n", pal);
    EXPECT(pal == (is60(m) ? 0 : 1), "$02A6 for mode %d", m);
    check_mode(m, "booted");
    rates(m, m, "booted");
    uint8_t regs[3] = {peek(0xC104), peek(0xC105), peek(0xC106)};
    if (newm >= 0) {
        uint8_t loop0 = peek(0xC103), j0 = peek(0xA2);
        top->vmode = newm;
        run_cycles(2000);
        check_mode(newm, "switched");
        rates(newm, m, "switched");
        uint8_t now[3] = {peek(0xC104), peek(0xC105), peek(0xC106)};
        printf("VIC $D018 %02X->%02X  $D020 %02X->%02X  $D011 %02X->%02X; loop %d, jiffies %d since the switch\n",
               regs[0], now[0], regs[1], now[1], regs[2], now[2], uint8_t(peek(0xC103) - loop0), uint8_t(peek(0xA2) - j0));
        EXPECT(!memcmp(regs, now, 3), "VIC registers changed");
        EXPECT(peek(0x02A6) == pal, "$02A6 changed");
    }
    printf(fails ? "FAIL\n" : "PASS\n");
    return fails ? 1 : 0;
}

int main(int argc, char **argv)
{
    Verilated::commandArgs(argc, argv);
    top = new Vc64_core;
#ifdef TRACE
    if (const char *t = getenv("TRACE_CYCLES")) {   // "from,to" in C64 cycles after the start of the run
        unsigned long long a, b;
        if (sscanf(t, "%llu,%llu", &a, &b) == 2) {
            Verilated::traceEverOn(true);
            tfp = new VerilatedVcdC;
            top->trace(tfp, getenv("TRACE_DEPTH") ? atoi(getenv("TRACE_DEPTH")) : 99);
            tfp->open(getenv("TRACE_FILE") ? getenv("TRACE_FILE") : "trace.vcd");
            trace_from = a * 32; trace_to = b * 32;
        }
    }
    if (const char *t = getenv("CPULOG")) {
        unsigned long long a, b;
        if (sscanf(t, "%llu,%llu", &a, &b) == 2) {
            cpulog = fopen(getenv("CPULOG_FILE") ? getenv("CPULOG_FILE") : "cpu.log", "w");
            log_from = a * 32; log_to = b * 32;
        }
    }
#endif
    int rc = 3;
    if (argc >= 3 && !strcmp(argv[1], "boot")) rc = boot(argv[2]);
    else if (argc >= 5 && !strcmp(argv[1], "run")) {
        rc = run(argv[2], argv[3], strtoull(argv[4], nullptr, 0), argc > 5 ? strtoull(argv[5], nullptr, 0) : 0);
    }
    else if (argc >= 3 && !strcmp(argv[1], "modes")) rc = modes(argv[2], argc > 3 ? atoi(argv[3]) : -1);
    else fprintf(stderr, "usage: %s boot <state> | run <state> <prg> <cycles> [wait] | modes <state> [mode]\n", argv[0]);
#ifdef TRACE
    if (tfp) tfp->close();
#endif
    top->final();
    delete top;
    return rc;
}
