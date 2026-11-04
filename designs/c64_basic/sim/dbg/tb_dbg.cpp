// SPDX-License-Identifier: GPL-3.0-or-later
// T21 §9.1 under Verilator on the debug hardware (tb_dbg_top.v: sim/drive's
// bench with trx_dbg.v in c64_core and drv_dbg.v on drive A).
//
//   tb_dbg boot <state> <rom>   as tb_pause boot: both up to READY, save
//   tb_dbg run <state>          a test loop at $C000 (no bad lines, IRQ off),
//                               then: 1 PAUSE_IB, registers and IPC; 2
//                               STEP_INSTR through the loop against the opcode
//                               table, STEP_CYCLES; 3 the exec bitmap; 4 watch
//                               units (write, read with a data mask, PASS/HITS,
//                               chain); 5 n over a JSR with a breakpoint inside;
//                               6 ret; 7 raster source; CYC_CMP and the log
//                               action; 8 CPU writes (registers, redirect)
//                               at an IB; 9 single cycle steps leave nothing but
//                               time behind; 11 the drive's step, comparator
//                               and the C64 following it. Exit code 0 = ok.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "Vtb_dbg_top.h"
#include "Vtb_dbg_top___024root.h"
#include "verilated.h"
#include "verilated_save.h"

static Vtb_dbg_top *top;
static uint64_t now_ps, next_clk, next_sys, next_ui;
static uint64_t sys_cycles, clk_edges;
static int fails;
static const uint64_t CLK_HALF = 15859;     // 31.5278 MHz
static const uint64_t SYS_HALF = 5000;      // 100 MHz
static const uint64_t UI_HALF = 6667;       // 75 MHz
static const uint32_t FRAME = 628992;       // dot4x clocks per PAL frame

#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

// ---- the MIG model's memory (16 MB, 128-bit words) ----------------------------
#define MEM top->rootp->tb_dbg_top__DOT__mig__DOT__mem
static uint8_t ddr_rd(uint32_t a) { return MEM[(a >> 4) & 0xFFFFF][(a >> 2) & 3] >> (8 * (a & 3)); }

// ---- clocks: the next edge of any; the IEC port idle -----------------------------
static uint64_t next_c2 = ~0ull;
static void step()
{
    uint64_t t = next_clk < next_sys ? next_clk : next_sys;
    if (next_ui < t) t = next_ui;
    if (next_c2 < t) t = next_c2;
    now_ps = t;
    // clk2x (the T25.1 SID engine's clock): rises with every clk edge, falls halfway
    if (next_c2 == t) { top->clk2x = 0; next_c2 = ~0ull; }
    if (next_sys == t) {
        if (!top->sys_clk) sys_cycles++;
        top->sys_clk = !top->sys_clk; next_sys += SYS_HALF;
    }
    if (next_clk == t) {
        if (!top->clk) clk_edges++;
        top->clk = !top->clk; next_c2 = next_clk + CLK_HALF / 2; next_clk += CLK_HALF; top->clk2x = 1;
    }
    if (next_ui == t) { top->ui_clk = !top->ui_clk; next_ui += UI_HALF; }
    top->port_atn_i = top->port_clk_i = top->port_data_i = 0;
    top->eval();
}

static void sys_tick() { uint64_t c = sys_cycles; while (sys_cycles == c) step(); }
static void clk_tick() { uint64_t c = clk_edges; while (clk_edges == c) step(); while (top->clk) step(); }
static void run_us(double us) { uint64_t end = now_ps + uint64_t(us * 1e6); while (now_ps < end) step(); }
static void run_clks(uint64_t n) { for (uint64_t i = 0; i < n; i++) clk_tick(); }

// ---- the RISC-V's side ---------------------------------------------------------
static void io_write(uint16_t a, uint8_t v)
{
    top->io_addr = a; top->io_wdata = v; top->io_write = 1;
    sys_tick();
    top->io_write = 0;
    for (int n = 0; n < 100 && !top->io_ack; n++) sys_tick();
    if (!top->io_ack) { printf("FAIL: no IO ack at %04X\n", a); exit(4); }
    sys_tick();
}
static uint8_t io_read(uint16_t a)
{
    top->io_addr = a; top->io_read = 1;
    sys_tick();
    top->io_read = 0;
    for (int n = 0; n < 100 && !top->io_ack; n++) sys_tick();
    uint8_t v = top->io_rdata;
    sys_tick();
    return v;
}
static void io_write32(uint16_t a, uint32_t v) { for (int i = 0; i < 4; i++) io_write(a + i, v >> (8 * i)); }
static void host_write(uint32_t a, uint32_t v)
{
    top->host_address = a; top->host_wdata = v; top->host_byte_en = 0xF;
    top->host_rwn = 0; top->host_request = 1;
    for (int n = 0; n < 100000; n++) {
        sys_tick();
        if (top->mem_rack && top->mem_rack_tag == 0x21) { top->host_request = 0; return; }
    }
    printf("FAIL: host write %08X not taken\n", a);
    exit(4);
}

// ---- the C64: RAM through c64_mem's second port (clk, not the C64's clock) -----
static uint8_t peek(uint16_t a) { top->dbg_addr = a; clk_tick(); clk_tick(); return top->dbg_rdata; }
static void poke(uint16_t a, uint8_t v)
{
    top->dbg_addr = a; top->dbg_wdata = v; top->dbg_we = 1;
    clk_tick();
    top->dbg_we = 0;
}
static std::vector<uint8_t> ram()
{
    std::vector<uint8_t> m(65536);
    for (int a = 0; a < 65536; a++) m[a] = peek(a);
    return m;
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

// one byte as the app's bus DMA (c64_core's dma port): clk cycles taken, the
// C64 clock edges it cost
static uint8_t dma(uint16_t a, bool we, uint8_t d, uint32_t *edges = nullptr)
{
    uint32_t g0 = top->gclk_edges;
    top->dma_addr = a; top->dma_we = we; top->dma_wdata = d; top->dma_req = 1;
    clk_tick();
    top->dma_req = 0;
    for (int n = 0; n < 200000; n++) {
        if (top->dma_done) {
            uint8_t v = top->dma_rdata;
            clk_tick();
            if (edges) *edges = top->gclk_edges - g0;
            return v;
        }
        clk_tick();
    }
    printf("FAIL: DMA %04X not done\n", a);
    exit(4);
}

static void halt(bool on)
{
    top->halt_req = on;
    for (int n = 0; n < 2 * FRAME; n++) {
        if (top->halted == on) return;
        clk_tick();
    }
    printf("FAIL: machine %s\n", on ? "did not stop" : "did not go on");
    exit(4);
}
static uint32_t drive_cycles() { return top->drv_dbg[7] & 0xFFFF; }
// a state bus word (c64_core.v's map), read without side effects
static uint32_t sb(int chip, int word)
{
    top->sb_addr = chip << 7 | word;
    clk_tick(); clk_tick(); clk_tick();
    return top->sb_rdata;
}
static std::vector<uint32_t> sb_all()
{
    std::vector<uint32_t> w;
    static const int n[5] = {1, 3, 12, 5, 5};
    for (int c = 1; c < 5; c++) for (int i = 0; i < n[c]; i++) w.push_back(sb(c, i));
    return w;
}
static uint32_t raster() { return top->pix_y << 10 | top->pix_x; }
static uint16_t counter() { return peek(0xC100) | peek(0xC101) << 8; }
// gclk edges left out so far, modulo a frame
static uint32_t skew() { return (clk_edges - top->gclk_edges) % FRAME; }

// ---- save / restore (the model, the clocks, the memory) ------------------------
static void save(const char *name)
{
    VerilatedSave os;
    os.open(name);
    os << now_ps << next_clk << next_sys << next_ui << sys_cycles << clk_edges << *top;
}
static void restore(const char *name)
{
    VerilatedRestore is;
    is.open(name);
    is >> now_ps >> next_clk >> next_sys >> next_ui >> sys_cycles >> clk_edges >> *top;
}

static const uint32_t DRIVE_RAM = 0x0EE0000;   // ultimate_logic_32 g_ram_base
static const uint32_t GCR_BASE  = 0x0100000;

static int boot(const char *state, const char *rom_name)
{
    std::vector<uint8_t> rom(32768);
    FILE *f = fopen(rom_name, "rb");
    if (!f) { printf("no %s\n", rom_name); return 2; }
    size_t n = fread(rom.data(), 1, rom.size(), f);
    fclose(f);
    if (n == 16384) memcpy(&rom[16384], &rom[0], 16384);   // c1541.cc: a 16 K ROM twice
    else if (n != 32768) { printf("ROM %s: %zu bytes\n", rom_name, n); return 2; }

    top->rst = 1; top->sys_rst = 1; top->cpu_reset_req = 0; top->c64_reset_n = 0;
    next_clk = CLK_HALF; next_sys = SYS_HALF; next_ui = UI_HALF;
    for (int i = 0; i < 100; i++) sys_tick();
    top->rst = 0; top->sys_rst = 0;
    for (int i = 0; i < 100; i++) sys_tick();

    // as tb_drive boot (c1541.cc init, drive_power), with no disk
    const uint32_t dummy = GCR_BASE + 0x80000, dummy_len = 0x1E0C, rot = 100000000 / 20;
    for (int i = 0; i < 256; i++) {
        io_write32(0x1000 + 8 * i, dummy);
        io_write32(0x1000 + 8 * i + 4, ((rot / dummy_len) << 16) | 0x100);
    }
    for (int a = 0; a < 32768; a += 4)
        host_write(DRIVE_RAM + 0x8000 + a, rom[a] | rom[a + 1] << 8 | rom[a + 2] << 16 | (uint32_t)rom[a + 3] << 24);
    io_write(5, 0x00);
    io_write(2, 0x00);
    io_write(0, 1);
    io_write(1, 1);
    run_us(1000);
    io_write(1, 0x02);
    io_write(0x800, 0x80);

    top->c64_reset_n = 1;
    for (int k = 0; k < 60; k++) {
        run_us(50000);
        if (find_row("READY.") >= 0) break;
    }
    run_us(300000);
    CHECK(find_row("READY.") >= 0, "C64: no READY");
    std::string msg;
    for (int i = 0; i < 24; i++) { uint8_t c = ddr_rd(DRIVE_RAM + 0x2D5 + i); msg += (c >= 32 && c < 127) ? char(c) : '.'; }
    printf("drive $02D5: %s\n", msg.c_str());
    CHECK(msg.find("CBM DOS V2.6 1541") != std::string::npos, "drive: no CBM DOS V2.6 1541");
    CHECK(io_read(0) == 1, "drive: POWER reads back wrong");
    save(state);
    printf("%s, %d failures\n", fails ? "BOOT FAILED" : "boot ok", fails);
    return fails ? 1 : 0;
}

// ---- the debug registers (trx_dbg.v, drv_dbg.v): word index {page 3, word} ----------
enum {
    OWNER = 0x046, CYCLES = 0x048, CYC_HI = 0x049, INSTRS = 0x04A, CYC_CMP = 0x04C, CYC_EN = 0x04D,
    CMD = 0x400, ARG = 0x401, STATUS = 0x402, STOP0 = 0x404, STOP1 = 0x405,
    BMP_IDX = 0x410, BMP_DATA = 0x411, DBG_CTRL = 0x412, UNIT = 0x420, HITS = 0x42D,
    DRV_CMP = 0x430, DRV_STAT = 0x431, DCYCLES = 0x432
};
enum { U_CTRL, U_RANGE, U_DATA, U_REG, U_PASS, U_HITS };
enum { C_PAUSE_CB = 1, C_PAUSE_IB = 2, C_RUN = 3, C_STEP_I = 5, C_STEP_C = 6, C_CLEAR = 0x20,
       C_DPAUSE = 0x10, C_DRUN = 0x11, C_DSTEP = 0x12 };
// unit CTRL bits
enum { EN = 1, T_IB = 1 << 1, T_RD = 2 << 1, T_WR = 3 << 1, T_RW = 4 << 1, T_CYC = 5 << 1,
       SRC_RASTER = 1 << 4, A_COUNT = 1 << 5, A_LOG = 2 << 5, ONESHOT = 1 << 8, CHAIN = 1 << 9,
       PREV_RET = 1 << 14 };

static void dw(int a, uint32_t v)
{
    top->dr_addr = a; top->dr_wdata = v; top->dr_we = 1;
    clk_tick();
    top->dr_we = 0;
    clk_tick();
}
static uint32_t dr(int a)
{
    top->dr_addr = a;
    clk_tick(); clk_tick();
    uint32_t v = top->dr_rdata;
    top->dr_re = 1;
    clk_tick();
    top->dr_re = 0;
    return v;
}
static void cmd(int c, uint32_t arg = 0) { if (c == C_STEP_I || c == C_STEP_C || c == C_DSTEP) dw(ARG, arg); dw(CMD, c); }
static void unit(int n, uint32_t ctrl, uint16_t lo, uint16_t hi, uint32_t data = 0, uint32_t reg = 0,
                 uint16_t pass = 0)
{
    int b = UNIT + 8 * n;
    dw(b + U_CTRL, 0);
    dw(b + U_RANGE, (uint32_t)hi << 16 | lo);
    dw(b + U_DATA, data);
    dw(b + U_REG, reg);
    dw(b + U_PASS, pass);
    dw(b + U_HITS, 0);
    dw(b + U_CTRL, ctrl);
}
static uint64_t cycles() { uint32_t lo = dr(CYCLES); return (uint64_t)dr(CYC_HI) << 32 | lo; }
static uint32_t status() { return dr(STATUS); }
static bool shut() { return status() & 1; }
// wait for a stop record (STOP_NEW) and the gate shut
static bool wait_stop(double max_us = 100000)
{
    uint64_t end = now_ps + uint64_t(max_us * 1e6);
    while (now_ps < end) {
        for (int i = 0; i < 64; i++) step();
        uint32_t s = top->dr_rdata;
        (void)s;
        top->dr_addr = STATUS;
        clk_tick();
        if ((top->dr_rdata & 9) == 9) return true;
    }
    return false;
}
struct Stop { int reason, unit, flags, data, pos, addr, ipc; };
static Stop stop_rec()
{
    uint32_t a = dr(STOP0), b = dr(STOP1);
    return Stop{int(a & 15), int(a >> 4 & 1), int(a >> 5 & 7), int(a >> 8 & 0xFF), int(a >> 16),
                int(b >> 16), int(b & 0xFFFF)};
}
static uint32_t ipc() { return sb(1, 3) & 0xFFFF; }
static int reg_a() { return sb(1, 0) & 0xFF; }
static int reg_x() { return sb(1, 0) >> 8 & 0xFF; }
static int reg_y() { return sb(1, 0) >> 16 & 0xFF; }
static int reg_s() { return sb(1, 0) >> 24 & 0xFF; }
static void bmp_set(uint16_t a, bool on)
{
    dw(BMP_IDX, a >> 5);
    uint32_t w = dr(BMP_DATA);
    dw(BMP_IDX, a >> 5);
    dw(BMP_DATA, on ? w | 1u << (a & 31) : w & ~(1u << (a & 31)));
}
static void bmp_clear() { dw(BMP_IDX, 0); for (int i = 0; i < 2048; i++) dw(BMP_DATA, 0); }
// run to the next stop: RUN, then wait for the record
static bool run_to_stop(double max_us = 100000) { cmd(C_RUN); return wait_stop(max_us); }


// ---- CPU writes (T21 §3.3) as the app makes them: the hidden CPU words (chip 1
// w8-w14) staged back with the registers changed, loaded on the next opening
// edge. At an IB the CPU's update for the fetch (9 clocks after the phi2 fall)
// is still to come: the staged A X Y S P are the post-update values (w4/w5)
// with the written ones replaced, and the pending write-back is taken out
// (IR = NOP, no reg_update/copy_d2p/flags_update), so the update only
// takes the opcode. A redirect loads PC and the fetched byte (din_r) too.
enum { W_A = 1, W_X = 2, W_Y = 4, W_S = 8, W_P = 16, W_PC = 32 };
static void put_bits(uint32_t *w, int lo, int n, uint32_t v)
{
    for (int i = 0; i < n; i++) {
        int b = lo + i;
        w[b >> 5] = (w[b >> 5] & ~(1u << (b & 31))) | (((v >> i) & 1u) << (b & 31));
    }
}
static void cpu_write(int mask, int a, int x, int y, int sp, int p, uint16_t pc = 0)
{
    uint32_t w[7];
    for (int i = 0; i < 7; i++) w[i] = sb(1, 8 + i);
    uint32_t nx = sb(1, 4), np = sb(1, 5) & 0xFF;
    put_bits(w, 0, 8, mask & W_A ? a : nx & 0xFF);
    put_bits(w, 8, 8, mask & W_X ? x : nx >> 8 & 0xFF);
    put_bits(w, 16, 8, mask & W_Y ? y : nx >> 16 & 0xFF);
    put_bits(w, 24, 8, mask & W_S ? sp : nx >> 24);
    put_bits(w, 32, 8, mask & W_P ? p : np);
    put_bits(w, 80, 8, 0xEA);                   // IR: a NOP, nothing to write back
    put_bits(w, 102 + 12, 1, 0);                // copy_d2p
    put_bits(w, 102 + 15, 1, 0);                // flags_update
    put_bits(w, 102 + 16, 1, 0);                // trigger_upd
    if (mask & W_PC) {
        uint8_t op = peek(pc);
        put_bits(w, 40, 16, pc);                // the fetch's address: PC, +1 by the update
        put_bits(w, 130, 8, op);                // din_r: the byte the fetch read
        dw(0x406, 1u << 24 | (uint32_t)op << 16 | pc);   // IPC_SET, still an IB
    }
    for (int i = 0; i < 7; i++) {
        top->sb_waddr = 1 << 7 | (8 + i); top->sb_wdata = w[i]; top->sb_we = 1;
        clk_tick();
        top->sb_we = 0;
        clk_tick();
    }
    top->sb_ldmask = 2; top->sb_ldreq = 1;
    clk_tick();
    top->sb_ldreq = 0;
    clk_tick();
}
static int nx_a() { return sb(1, 4) & 0xFF; }
static int nx_p() { return sb(1, 5) & 0xFF; }
static bool step_i(int n) { cmd(C_STEP_I, n); return wait_stop(20000); }

// the test program at $C000: interrupts off, screen off (no bad lines: RDY
// stays high, cycle counts are the opcode table's), a loop that stores A X Y,
// calls a subroutine, counts in $C103, bumps the border, reads CIA 1's ICR
static const uint8_t PROG[] = {
    0x78,                               // C000 SEI
    0xA9, 0x0B, 0x8D, 0x11, 0xD0,       // C001 LDA #$0B, STA $D011
    0xA2, 0x05,                         // C006 LDX #$05
    0xA0, 0x07,                         // C008 LDY #$07
    0xA9, 0x42,                         // C00A LDA #$42           loop
    0x8D, 0x00, 0xC1,                   // C00C STA $C100
    0x8E, 0x01, 0xC1,                   // C00F STX $C101
    0x8C, 0x02, 0xC1,                   // C012 STY $C102
    0x20, 0x40, 0xC0,                   // C015 JSR $C040
    0xEE, 0x03, 0xC1,                   // C018 INC $C103
    0xEE, 0x20, 0xD0,                   // C01B INC $D020
    0xAD, 0x0D, 0xDC,                   // C01E LDA $DC0D
    0x8D, 0x04, 0xC1,                   // C021 STA $C104
    0x4C, 0x0A, 0xC0,                   // C024 JMP $C00A
};
static const uint8_t SUB[] = { 0xE8, 0xC8, 0xEA, 0x60 };   // C040 INX INY NOP RTS

// ---- tests ----------------------------------------------------------------------
static int run(const char *state)
{
    restore(state);
    for (unsigned i = 0; i < sizeof PROG; i++) poke(0xC000 + i, PROG[i]);
    for (unsigned i = 0; i < sizeof SUB; i++) poke(0xC040 + i, SUB[i]);
    const char *sys = "SYS49152\r";
    for (int i = 0; sys[i]; i++) poke(0x0277 + i, sys[i]);
    poke(0x00C6, strlen(sys));
    run_us(100000);
    CHECK(peek(0xC103) != 0, "test loop does not run");
    bmp_clear();

    // 1. PAUSE_IB, registers, IPC
    cmd(C_PAUSE_IB);
    CHECK(wait_stop(), "1: no stop after PAUSE_IB");
    Stop s = stop_rec();
    uint32_t st = status();
    printf("1. PAUSE_IB: reason %d, IPC %04X, flags %X, status %08X\n", s.reason, s.ipc, s.flags, st);
    CHECK(s.reason == 1 && (s.flags & 1) && (st & 4), "1: not an IB stop");
    CHECK(s.ipc == (int)ipc() && (sb(1, 3) >> 24 & 1), "1: IPC word differs");
    // to the JSR: registers as the program stored them
    bmp_set(0xC015, true); dw(DBG_CTRL, 1);
    CHECK(run_to_stop(), "1: no stop at the bitmap bit");
    s = stop_rec();
    printf("   bitmap stop: reason %d, IPC %04X, A %02X X %02X Y %02X S %02X, opcode %02X\n", s.reason,
           s.ipc, reg_a(), reg_x(), reg_y(), reg_s(), sb(1, 3) >> 16 & 0xFF);
    CHECK(s.reason == 3 && s.ipc == 0xC015 && ipc() == 0xC015, "3: exec bitmap stop at $C015");
    CHECK(reg_a() == 0x42 && reg_x() == peek(0xC101) && reg_y() == peek(0xC102), "1: A X Y");
    CHECK((sb(1, 3) >> 16 & 0xFF) == 0x20, "1: opcode at IPC");
    bmp_set(0xC015, false);
    const char *save_ib = "dbg_ib.state";
    save(save_ib);

    // 2. STEP_INSTR 1 through one loop: IPC and the cycle count per instruction
    static const struct { uint16_t pc; int cyc; } path[] = {
        {0xC015, 6}, {0xC040, 2}, {0xC041, 2}, {0xC042, 2}, {0xC043, 6}, {0xC018, 6}, {0xC01B, 6},
        {0xC01E, 4}, {0xC021, 4}, {0xC024, 3}, {0xC00A, 2}, {0xC00C, 4}, {0xC00F, 4}, {0xC012, 4},
    };
    int bad = 0;
    uint32_t i0 = dr(INSTRS);
    for (unsigned k = 0; k + 1 < sizeof path / sizeof path[0]; k++) {
        uint64_t c0 = cycles();
        cmd(C_STEP_I, 1);
        if (!wait_stop(1000)) { bad++; break; }
        uint64_t dc = cycles() - c0;
        Stop r = stop_rec();
        if (r.reason != 2 || r.ipc != path[k + 1].pc || (int)dc != path[k].cyc) {
            printf("   step %u: IPC %04X (want %04X), %llu cycles (want %d), reason %d\n", k, r.ipc,
                   path[k + 1].pc, (unsigned long long)dc, path[k].cyc, r.reason);
            bad++;
        }
    }
    printf("2. STEP_INSTR through the loop: %d mismatches, INSTRS +%u\n", bad, dr(INSTRS) - i0);
    CHECK(bad == 0 && dr(INSTRS) - i0 == 13, "2: steps");
    // STEP_CYCLES: 1 cycle each, then 7 at once
    uint64_t c0 = cycles();
    for (int k = 0; k < 5; k++) { cmd(C_STEP_C, 1); CHECK(wait_stop(100), "2: cycle step"); }
    cmd(C_STEP_C, 7); CHECK(wait_stop(100), "2: 7-cycle step");
    printf("   STEP_CYCLES 5 x 1 + 7: CYCLES +%llu\n", (unsigned long long)(cycles() - c0));
    CHECK(cycles() - c0 == 12, "2: cycle steps");


    // 8. CPU writes at an IB (zero cycles): stop after LDA #$42 at $C00C
    restore(save_ib);
    CHECK(step_i(11) && ipc() == 0xC00C, "8: no IB at $C00C");
    printf("8. at $C00C after LDA #$42: A %02X on the CPU, %02X after its update (P %02X -> %02X)\n",
           reg_a(), nx_a(), sb(1, 1) >> 16 & 0xFF, nx_p());
    CHECK(nx_a() == 0x42, "8: the post-update A");
    const char *save_c00c = "dbg_c00c.state";
    save(save_c00c);
    // a write of nothing changes nothing: 200 instructions on, RAM, registers, raster, cycles
    uint64_t cy8 = cycles();
    CHECK(step_i(200), "8: reference steps");
    std::vector<uint8_t> ref8 = ram();
    uint32_t ref8_r = raster(), ref8_w0 = sb(1, 0), ref8_w1 = sb(1, 1), ref8_ipc = ipc();
    uint64_t ref8_c = cycles() - cy8;
    restore(save_c00c);
    cpu_write(0, 0, 0, 0, 0, 0);
    CHECK(step_i(200), "8: steps after an empty write");
    std::vector<uint8_t> got8 = ram();
    printf("   empty write, 200 steps: RAM %s, A X Y S %08X vs %08X, P/PC %08X vs %08X, IPC %04X vs %04X,"
           " cycles %llu vs %llu, raster %u vs %u\n", got8 == ref8 ? "equal" : "DIFFERS", sb(1, 0), ref8_w0,
           sb(1, 1), ref8_w1, ipc(), ref8_ipc, (unsigned long long)(cycles() - cy8),
           (unsigned long long)ref8_c, raster(), ref8_r);
    CHECK(got8 == ref8 && sb(1, 0) == ref8_w0 && sb(1, 1) == ref8_w1 && ipc() == ref8_ipc &&
          cycles() - cy8 == ref8_c && raster() == ref8_r, "8: an empty write is invisible");
    // A and X written: STA/STX store them, no cycle added
    restore(save_c00c);
    cy8 = cycles();
    cpu_write(W_A | W_X, 0x77, 0x99, 0, 0, 0);
    CHECK(step_i(2), "8: steps after r a x");
    printf("   r a=77 x=99: $C100 %02X $C101 %02X, %llu cycles for STA+STX\n", peek(0xC100), peek(0xC101),
           (unsigned long long)(cycles() - cy8));
    CHECK(peek(0xC100) == 0x77 && peek(0xC101) == 0x99 && cycles() - cy8 == 8, "8: r a x");
    // only X written: LDA's own write-back of A stays
    restore(save_c00c);
    cpu_write(W_X, 0, 0x55, 0, 0, 0);
    CHECK(step_i(2), "8: steps after r x");
    CHECK(peek(0xC100) == 0x42 && peek(0xC101) == 0x55, "8: LDA's write-back kept under r x");
    // redirect to $C018 (INC $C103): IPC at once, the instruction there, 6 cycles, nothing more
    restore(save_c00c);
    uint8_t c103 = peek(0xC103);
    cy8 = cycles();
    uint32_t in8 = dr(INSTRS);
    cpu_write(W_PC, 0, 0, 0, 0, 0, 0xC018);
    uint32_t ipc_now = ipc(), st_now = status();
    CHECK((st_now & 4) && (sb(1, 3) >> 24 & 1) && (sb(1, 3) >> 16 & 0xFF) == 0xEE, "8: IB, IPC and opcode after IPC_SET");
    CHECK(step_i(1), "8: step after g c018");
    printf("   pc=C018: IPC %04X before the step, %04X after, $C103 %02X -> %02X, %llu cycles, INSTRS +%u\n",
           ipc_now, ipc(), c103, peek(0xC103), (unsigned long long)(cycles() - cy8), dr(INSTRS) - in8);
    CHECK(ipc_now == 0xC018 && ipc() == 0xC01B && peek(0xC103) == (uint8_t)(c103 + 1) && cycles() - cy8 == 6 &&
          dr(INSTRS) - in8 == 1, "8: redirect");
    // flags: into a routine at $C200 with A=$10; C set by r right after CLC (whose
    // flag write is still to come at the stop) must reach ADC
    static const uint8_t R200[] = { 0x38, 0x18, 0x69, 0x01, 0x8D, 0x10, 0xC1, 0x08, 0x68, 0x8D, 0x11, 0xC1,
                                    0x4C, 0x0A, 0xC0 };
    restore(save_c00c);
    for (unsigned i = 0; i < sizeof R200; i++) poke(0xC200 + i, R200[i]);
    cpu_write(W_PC | W_A, 0x10, 0, 0, 0, 0, 0xC200);
    CHECK(step_i(2) && ipc() == 0xC202, "8: SEC CLC");
    int p_clc = nx_p();
    cpu_write(W_P, 0, 0, 0, 0, p_clc | 1);
    CHECK(step_i(1) && ipc() == 0xC204, "8: ADC");
    int a_on = reg_a(), a_nx = nx_a();
    CHECK(step_i(5) && ipc() == 0xC00A, "8: back in the loop");
    printf("   fl after CLC (P %02X -> %02X): ADC #1 with A=10 gives %02X (A %02X on the CPU, %02X after the"
           " update), stored %02X, P pushed %02X\n", p_clc, p_clc | 1, a_nx, a_on, a_nx, peek(0xC110),
           peek(0xC111));
    CHECK(!(p_clc & 1) && a_nx == 0x12 && peek(0xC110) == 0x12 && a_on == 0x10, "8: r fl after CLC");
    uint8_t n8 = peek(0xC103);
    cmd(C_RUN);
    run_us(20000);
    CHECK(peek(0xC103) != n8, "8: the loop runs on after the writes");
    cmd(C_PAUSE_IB);
    wait_stop();

    // 4. watch units
    restore(save_ib);
    unit(0, EN | T_WR, 0xD020, 0xD020);
    CHECK(run_to_stop(), "4: no write stop");
    s = stop_rec();
    uint32_t vic20 = sb(2, 8) & 0xFF;
    printf("4. write $D020: reason %d unit %d, ADDR %04X DATA %02X (VIC $D020 %02X), IPC %04X, flags %X\n",
           s.reason, s.unit, s.addr, s.data, vic20, s.ipc, s.flags);
    CHECK(s.reason == 5 && s.unit == 0 && s.addr == 0xD020 && s.ipc == 0xC01B && !(s.flags & 1) &&
          (s.data & 15) == (int)(vic20 & 15), "4: write watch");
    // read watch on the ICR, only when bit 0 (timer A) is read set
    unit(0, 0, 0, 0);
    unit(1, EN | T_RD, 0xDC0D, 0xDC0D, 0x0101);
    CHECK(run_to_stop(), "4: no read stop");
    s = stop_rec();
    // the CIA clears the flags with its next clock (reDIP's ICR clear pipeline):
    // stopped on the read's edge, the clear is still pending and comes with the
    // machine's next cycle, as it would have without the stop
    uint32_t icr = sb(3, 3) >> 8 & 0xFF;
    cmd(C_STEP_C, 1); wait_stop(100);
    uint32_t icr1 = sb(3, 3) >> 8 & 0xFF;
    printf("   read $DC0D with bit 0: unit %d, DATA %02X, ICR at the stop %02X, a cycle later %02X, IPC %04X\n",
           s.unit, s.data, icr, icr1, s.ipc);
    CHECK(s.unit == 1 && s.addr == 0xDC0D && (s.data & 0x81) == 0x81 && (icr1 & 1) == 0, "4: read watch");
    // PASS: the 4th execution of $C00A
    restore(save_ib);
    unit(0, EN | T_IB, 0xC00A, 0xC00A, 0, 0, 3);
    uint8_t n0 = peek(0xC103);
    CHECK(run_to_stop(), "4: no PASS stop");
    s = stop_rec();
    uint32_t h = dr(HITS);
    printf("   PASS 3: stop at %04X, HITS %u, loops %d\n", s.ipc, h & 0xFFFF, (uint8_t)(peek(0xC103) - n0));
    CHECK(s.ipc == 0xC00A && (h & 0xFFFF) == 4 && (uint8_t)(peek(0xC103) - n0) == 4, "4: PASS/HITS");
    // chain: a write to $C100-$C104 that writes $42
    restore(save_ib);
    unit(0, EN | T_WR | CHAIN, 0xC100, 0xC104);
    unit(1, EN | T_WR | A_COUNT, 0x0000, 0xFFFF, 0xFF42);
    CHECK(run_to_stop(), "4: no chain stop");
    s = stop_rec();
    printf("   chain: unit %d ADDR %04X DATA %02X, unit 1 HITS %u\n", s.unit, s.addr, s.data, dr(HITS) >> 16);
    CHECK(s.unit == 0 && s.addr == 0xC100 && s.data == 0x42, "4: chain");

    // 5. n over the JSR: exec at $C018 with S >= S0, a breakpoint in the subroutine first
    restore(save_ib);
    int s0 = reg_s();
    unit(0, EN | T_IB | ONESHOT, 0xC018, 0xC018, 0, 0xFF000304u | (uint32_t)s0 << 16);
    bmp_set(0xC041, true);
    CHECK(run_to_stop(), "5: no stop");
    s = stop_rec();
    int sin = reg_s();
    printf("5. n: first stop reason %d at %04X (S %02X, S0 %02X)", s.reason, s.ipc, sin, s0);
    CHECK(s.reason == 3 && s.ipc == 0xC041, "5: the breakpoint in the subroutine");
    bmp_set(0xC041, false);
    CHECK(run_to_stop(), "5: no second stop");
    s = stop_rec();
    printf(", then reason %d at %04X, S %02X, unit 0 %s\n", s.reason, s.ipc, reg_s(),
           (status() >> 16 & 1) ? "still on" : "off (one-shot)");
    CHECK(s.reason == 5 && s.ipc == 0xC018 && reg_s() == s0 && !(status() >> 16 & 1), "5: n");

    // 6. ret from the subroutine: any IB after RTS/RTI with S > S_in
    restore(save_ib);
    bmp_set(0xC042, true);
    CHECK(run_to_stop(), "6: no stop in the subroutine");
    bmp_set(0xC042, false);
    sin = reg_s();
    unit(1, EN | T_IB | PREV_RET | ONESHOT, 0x0000, 0xFFFF, 0, 0xFF000304u | (uint32_t)((sin + 1) & 0xFF) << 16);
    CHECK(run_to_stop(), "6: no ret stop");
    s = stop_rec();
    printf("6. ret: stop at %04X, unit %d, S %02X (inside %02X)\n", s.ipc, s.unit, reg_s(), sin);
    CHECK(s.ipc == 0xC018 && s.unit == 1 && reg_s() == sin + 2, "6: ret");

    // 7. raster: line 100, cycle 10, every phi2 cycle
    restore(save_ib);
    unit(0, EN | T_CYC | SRC_RASTER | ONESHOT, 100 << 7 | 10, 100 << 7 | 10);
    CHECK(run_to_stop(), "7: no raster stop");
    s = stop_rec();
    printf("7. raster: reason %d, POS line %d cycle %d, pix_y %d\n", s.reason, s.pos >> 7, s.pos & 127, top->pix_y);
    CHECK(s.reason == 6 && s.pos == (100 << 7 | 10), "7: raster");

    // CYC_CMP and the log action
    restore(save_ib);
    uint64_t cc = cycles() + 10000;
    dw(CYC_CMP, (uint32_t)cc);
    unit(1, EN | T_IB | A_LOG, 0xC00A, 0xC00A);
    uint32_t l0 = top->log_puts;
    CHECK(run_to_stop(), "CYC_CMP: no stop");
    s = stop_rec();
    printf("   CYC_CMP: reason %d, CYCLES %llu (CYC_CMP %llu); log records %u, unit 1 HITS %u\n", s.reason,
           (unsigned long long)cycles(), (unsigned long long)cc, top->log_puts - l0, dr(HITS) >> 16);
    CHECK(s.reason == 7 && cycles() == cc + 1, "CYC_CMP");
    CHECK(top->log_puts - l0 == dr(HITS) >> 16 && top->log_puts - l0 > 0, "log action");

    // 9. invisibility: steps change nothing but time
    restore(save_ib);
    unit(1, 0, 0, 0);
    uint64_t target = cycles() + 3 * 19656 + 777;
    dw(CYC_CMP, (uint32_t)target);
    CHECK(run_to_stop(1000000), "9: reference: no stop");
    std::vector<uint8_t> ref = ram();
    uint32_t ref_r = raster(), ref_t = sb(3, 1);
    cmd(C_RUN); run_us(50000);             // the frame lock while running again
    uint32_t ref_sk = skew();
    restore(save_ib);
    for (int k = 0; k < 1000; k++) { cmd(C_STEP_C, 1); if (!wait_stop(100)) { CHECK(0, "9: step %d", k); break; } }
    dw(CYC_CMP, (uint32_t)target);
    CHECK(run_to_stop(1000000), "9: no stop");
    std::vector<uint8_t> got = ram();
    uint32_t got_r = raster(), got_t = sb(3, 1);
    cmd(C_RUN); run_us(50000);
    printf("9. 1000 single cycle steps, then run to the same cycle: RAM %s, raster %u/%u vs %u/%u,"
           " CIA 1 timers %08X vs %08X; frame skew running on %u vs %u\n", got == ref ? "equal" : "DIFFERS",
           got_r >> 10, got_r & 0x3FF, ref_r >> 10, ref_r & 0x3FF, got_t, ref_t, skew(), ref_sk);
    CHECK(got == ref && got_r == ref_r && skew() == ref_sk && got_t == ref_t, "9: invisibility");

    // 11. drive: step it alone, then its comparator stops the drive and the C64
    restore(save_ib);
    cmd(C_CLEAR);
    uint32_t d0 = dr(DRV_STAT), dc0 = dr(DCYCLES);
    uint64_t cy0 = cycles();
    cmd(C_DSTEP, 1);
    CHECK(wait_stop(200), "11: no drive step stop");
    s = stop_rec();
    run_us(5);
    uint32_t d1 = dr(DRV_STAT), dc1 = dr(DCYCLES);
    printf("11. drive step: reason %d unit %d, DPC %04X -> %04X, drive cycles +%u, C64 cycles +%llu, stat %X\n",
           s.reason, s.unit, d0 >> 16, d1 >> 16, dc1 - dc0, (unsigned long long)(cycles() - cy0), d1 & 15);
    CHECK(s.reason == 9 && dc1 != dc0 && cycles() == cy0 && (d1 & 3) == 3, "11: drive step");
    for (int k = 0; k < 5; k++) { cmd(C_CLEAR); cmd(C_DSTEP, 1); wait_stop(200); }
    run_us(5);
    uint16_t dpc = dr(DRV_STAT) >> 16;
    dw(DRV_CMP, 1 << 16 | dpc);
    uint64_t hit_edges = 0, g_hit = 0;
    cmd(C_RUN);
    uint64_t t_end = now_ps + 60000000000ull;   // the resume waits for frame alignment
    while (now_ps < t_end && !hit_edges) {
        step();
        if (top->drv_stop_ev_o && top->clk) { hit_edges = clk_edges; g_hit = top->gclk_edges; }
    }
    CHECK(wait_stop(1000), "11: no C64 stop after the drive's");
    s = stop_rec();
    printf("    drive comparator at %04X: C64 reason %d flags %X, C64 clock edges after the drive's stop %llu,"
           " drive stat %X, DPC %04X\n", dpc, s.reason, s.flags, (unsigned long long)(top->gclk_edges - g_hit),
           dr(DRV_STAT) & 15, dr(DRV_STAT) >> 16);
    CHECK(hit_edges && s.reason == 8 && (s.flags & 2) && top->gclk_edges - g_hit <= 32 &&
          (dr(DRV_STAT) >> 16) == dpc, "11: drive comparator");
    dw(DRV_CMP, 0);
    cmd(C_RUN);
    run_us(20000);
    CHECK(!(dr(DRV_STAT) & 1) && !shut(), "11: RUN does not release both");

    printf("%s, %d failures\n", fails ? "FAILED" : "PASS", fails);
    return fails ? 1 : 0;
}

int main(int argc, char **argv)
{
    Verilated::commandArgs(argc, argv);
    top = new Vtb_dbg_top;
    if (argc >= 4 && !strcmp(argv[1], "boot")) return boot(argv[2], argv[3]);
    if (argc >= 3 && !strcmp(argv[1], "run")) return run(argv[2]);
    printf("usage: tb_dbg boot <state> <rom> | run <state>\n");
    return 2;
}
