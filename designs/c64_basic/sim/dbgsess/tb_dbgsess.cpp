// SPDX-License-Identifier: GPL-3.0-or-later
// The debugger's stop under Verilator (T12 §14.8, T21 P4; SLIM-state-path.md): c64_core with trx_dbg,
// dma_win.v (the app's window and the lens unit) and trx_sys.v, and the cartridge's DDR2 side as sim/cart.
// There is no freeze session: a stop is the gate shut, and what the app does to the stopped machine is
// a bus cycle (SLIM D1).
//
//   tb_dbgsess boot <state> [<file.crt> <state2>]...   cold start to READY, save; and per CRT cold
//        start with the cartridge to the end of its test program, save
//   tb_dbgsess run <state> [<name> <state2> <crt>]...   restore, then:
//     1. a stop of every kind (exec bitmap hit, PAUSE_CB, PAUSE_IB, a step's end) holds the machine
//        with no session; a window read of every byte $D000-$DFFF in it (a bus cycle each) equals the
//        read in an app freeze (C64_STOP, Ultimax) on the same clock edge, and costs one machine
//        cycle per byte; the cartridge reads (ROML, ROMH, IO1, IO2) likewise, with an EasyFlash and a
//        normal 16K cartridge (the states of boot)
//     2. the state bus words are free of side effects ($DC0D, $D019 stay set however often they are
//        looked at); the window read of $DC0D is a bus cycle that clears the flag and costs a machine
//        cycle; a window write reaches the chip at once
//     3. a window write in the stop reaches the chip at once, in the order written ($D020, $D021, a
//        CIA timer's latch before its LOAD), and RUN / a step go on from there
//     4. resume is bit-identical: a run with 14 breakpoint stops + RUN (a raster IRQ handler), one
//        with a step chain at each of them, against the same run never stopped (CYCLES, INSTRS,
//        CPU and chip state words, RAM, the IRQ log (line and timer), the pixel CRC); in the stops
//        what the monitor does: the state bus, the RAM through the second port; and a third run in
//        which every stop draws the app's freeze menu (screen, charset area, colour RAM through the
//        second ports, no bus cycle) and puts it back
//     5. the session registers are gone: SH_CTRL, SH_STATUS and the counters (page 5 0x000-0x01C) read
//        0 and ignore writes; the stop works the same
//     7. the latency RUN or a step adds: one clock
//     8. a foreground app holds C64_STOP while the debugger owns the machine: every stop works, the
//        colour RAM reads right as bus cycles
//   Exit code 0 = ok. The cartridge tests need the CRTs from gen_crts.py (run.sh makes them).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <deque>
#include <algorithm>
#include "Vtb_dbgs_top.h"
#include "verilated.h"
#include "verilated_save.h"
#include "trx_cart_desc.h"

static Vtb_dbgs_top *top;
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

// ---- clocks, DDR2 and the MIG app port (sim/cart's model) -------------------------------
static uint64_t now_ps, clk_next, ui_next, ticks, c2_next = ~0ull;
static const uint64_t CLK_HALF = 15859, UI_HALF = 6667;
static const uint32_t DDR_BYTES = 0x4000000, CART_BASE = 0x3C00000, CART_MAX = 0x400000;
static std::vector<uint8_t> ddr(DDR_BYTES, 0);
struct Rd { uint32_t addr; uint64_t due; };
static std::deque<Rd> rdq;
static std::deque<uint32_t> wcmd;
static std::deque<std::pair<std::vector<uint8_t>, uint16_t>> wdat;
static uint64_t ui_cycle;
static uint32_t rnd = 12345;
static uint32_t rand32() { rnd ^= rnd << 13; rnd ^= rnd >> 17; rnd ^= rnd << 5; return rnd; }

static void mig_write_pair()
{
    while (!wcmd.empty() && !wdat.empty()) {
        uint32_t byte = wcmd.front() * 2;
        auto &d = wdat.front();
        for (int i = 0; i < 16; i++)
            if (!(d.second & (1 << i)) && byte + i < DDR_BYTES) ddr[byte + i] = d.first[i];
        wcmd.pop_front();
        wdat.pop_front();
    }
}
static void mig_edge()       // around a ui_clk rising edge: sample, clock, update
{
    bool cmd  = top->app_en && top->app_rdy;
    bool wd   = top->app_wdf_wren && top->app_wdf_rdy;
    bool rdv  = top->app_rd_data_valid;
    uint32_t a = top->app_addr & ~7u;
    int c = top->app_cmd;
    std::vector<uint8_t> data(16);
    for (int i = 0; i < 4; i++)
        for (int b = 0; b < 4; b++) data[i * 4 + b] = uint8_t(top->app_wdf_data[i] >> (8 * b));
    uint16_t mask = top->app_wdf_mask;
    top->ui_clk = 1;
    top->eval();
    ui_cycle++;
    if (rdv) rdq.pop_front();
    if (cmd) {
        if (c == 1) rdq.push_back({a, ui_cycle + 26 + (rand32() % 8)});
        else wcmd.push_back(a);
    }
    if (wd) wdat.push_back({data, mask});
    mig_write_pair();
    top->app_rdy = (rand32() % 8) != 0;
    top->app_wdf_rdy = (rand32() % 8) != 0;
    if (!rdq.empty() && rdq.front().due <= ui_cycle && (rand32() % 4) != 0) {
        uint32_t byte = rdq.front().addr * 2;
        for (int i = 0; i < 4; i++) {
            uint32_t w = 0;
            for (int b = 0; b < 4; b++) w |= uint32_t(byte + i * 4 + b < DDR_BYTES ? ddr[byte + i * 4 + b] : 0) << (8 * b);
            top->app_rd_data[i] = w;
        }
        top->app_rd_data_valid = 1;
    } else {
        top->app_rd_data_valid = 0;
    }
    top->eval();
}
// the pixel CRC: one word per C64 clock edge that ran (the gate's gclk)
static bool crc_on;
static uint32_t pix_crc = 0xFFFFFFFF, last_g;
static uint32_t crc_word(uint32_t c, uint32_t w)
{
    for (int i = 0; i < 4; i++) {
        c ^= (w >> (8 * i)) & 0xFF;
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320 & -(c & 1));
    }
    return c;
}
static void step()
{
    uint64_t t = std::min(std::min(clk_next, ui_next), c2_next);
    now_ps = t;
    // clk2x rises with every clk edge and falls halfway (T29: the CPU port and c64_mem's second port run on it)
    if (c2_next == t) { top->clk2x = 0; c2_next = ~0ull; top->eval(); }
    if (ui_next == t) {
        if (!top->ui_clk) mig_edge();
        else { top->ui_clk = 0; top->eval(); }
        ui_next += UI_HALF;
    }
    if (clk_next == t) {
        top->clk = !top->clk;
        top->clk2x = 1;
        c2_next = t + CLK_HALF / 2;
        top->eval();
        clk_next += CLK_HALF;
    }
}
// one clk period: a rising edge, then the falling one (the gate's CE is taken there)
static void tick()
{
    while (!top->clk) step();
    while (top->clk) step();
    ticks++;
    if (crc_on && top->gclk_edges != last_g) {
        last_g = top->gclk_edges;
        pix_crc = crc_word(pix_crc, (uint32_t(top->pix_y) << 14) | (uint32_t(top->pix_x) << 4) | top->pix_color);
    }
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

// ---- the app's DMA window (ultimate_sys's handshake) ------------------------------------
static uint8_t dma(uint16_t a, bool we, uint8_t d, bool bus = true, bool rom = false)
{
    uint8_t done0 = top->dma_done_tgl;
    top->dma_addr = a; top->dma_we = we; top->dma_wdata = d; top->dma_bus = bus; top->dma_rom = rom;
    top->dma_req_tgl ^= 1;
    for (int n = 0; n < 32 * 200000; n++) {
        tick();
        if (top->dma_done_tgl != done0) return top->dma_rdata;
    }
    printf("FAIL: DMA %04X not done\n", a);
    exit(4);
}
static uint8_t rd(uint16_t a) { return dma(a, false, 0); }
static void wr(uint16_t a, uint8_t v) { dma(a, true, v); }

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

// all state bus words of chips 0-4 (glue 1, CPU 3, VIC 12, CIA 5 + 5)
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
static uint32_t crc32(const uint8_t *p, size_t n, uint32_t c = 0xFFFFFFFF)
{
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320 & -(c & 1));
    }
    return c;
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
static uint32_t sh_ctrl() { return treg(5, 0x000); }
static void sh_set(uint32_t v) { twr(5, 0x000, v); }
static uint32_t sh_status() { return treg(5, 0x004); }
static uint32_t sh_regs_or()            // SH_CTRL .. SH_COMMIT: all gone, they read 0
{
    uint32_t v = 0;
    for (int o = 0x000; o <= 0x01C; o += 4) v |= treg(5, o);
    return v;
}

// ---- save / restore (the C++ model of the MIG starts again, deterministic) ---------------
static void model_reset()
{
    rdq.clear(); wcmd.clear(); wdat.clear();
    ui_cycle = 0; rnd = 12345;
    now_ps = 0; clk_next = CLK_HALF; ui_next = UI_HALF;
}
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
    top->clk = 0; top->ui_clk = 0; top->clk2x = 0; c2_next = ~0ull; top->eval();
    model_reset();
    crc_on = false; pix_crc = 0xFFFFFFFF; last_g = top->gclk_edges;
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
    top->rst = 1; top->ui_rst = 1;
    top->dbg_re = 0; top->dbg_addr = 0; top->dbg_we = 0; top->dbg_wdata = 0;
    top->jtag_halt = 0; top->jtag_cpu_only = 0; top->app_stop = 0; top->app_ultimax = 0; top->app_reset = 0;
    top->dma_req_tgl = 0; top->dma_addr = 0; top->dma_we = 0; top->dma_wdata = 0; top->dma_bus = 0;
    top->dma_rom = 0;
    top->j_re = 0; top->j_we = 0; top->j_addr = 0; top->j_wdata = 0;
    top->rq_tgl = 0; top->rq_addr = 0; top->rq_we = 0; top->rq_wdata = 0;
    top->keys = 0; top->joy1 = 0; top->joy2 = 0;
    top->cart_type = 0; top->cart_kill_tgl = 0;
    top->app_rdy = 1; top->app_wdf_rdy = 1; top->app_rd_data_valid = 0;
    for (int i = 0; i < 64; i++) tick();
    top->rst = 0; top->ui_rst = 0;
}

// ---- the cartridge image in DDR2, as C64_CRT::read_crt lays it out (sim/cart) ------------
static uint16_t be16(const uint8_t *p) { return (p[0] << 8) | p[1]; }
static uint32_t be32(const uint8_t *p) { return (uint32_t(p[0]) << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
static int load_crt(const char *name)
{
    FILE *f = fopen(name, "rb");
    if (!f) { printf("cannot open %s\n", name); exit(2); }
    std::vector<uint8_t> file;
    uint8_t buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) file.insert(file.end(), buf, buf + n);
    fclose(f);
    if (file.size() < 0x40 || memcmp(file.data(), "C64 CARTRIDGE   ", 16)) { printf("not a C64 CRT\n"); exit(2); }
    uint8_t *img = &ddr[CART_BASE];
    memset(img, 0xFF, CART_MAX);
    int hw = be16(&file[0x16]), exrom = file[0x18], game = file[0x19];
    uint32_t pos = be32(&file[0x10]);
    if (pos < 0x40) pos = 0x40;
    int highest = 0;
    while (pos + 16 <= file.size() && !memcmp(&file[pos], "CHIP", 4)) {
        uint32_t len = be32(&file[pos + 4]);
        int type = be16(&file[pos + 8]), bank = be16(&file[pos + 10]);
        int load = be16(&file[pos + 12]), size = be16(&file[pos + 14]);
        uint32_t off = uint32_t(bank) * 0x4000 + (load & 0x2000);
        if (size && (type == 0 || type == 2) && load != 0xDE00 && off < CART_MAX) {
            memcpy(img + off, &file[pos + 16], size);
            if (bank > highest) highest = bank;
            uint32_t got = size;
            if (got < 8192) {
                for (uint32_t i = 256; i < uint32_t(size); i *= 2) if (got < i) { got = i; break; }
                while (got < 8192) { memcpy(img + off + got, img + off, got); got <<= 1; }
            }
        }
        pos += len;
    }
    int sz = 1;
    while (sz <= highest) sz <<= 1;
    while (sz < int(CART_MAX / 0x4000)) { memcpy(img + sz * 0x4000, img, sz * 0x4000); sz <<= 1; }
    int t = 0;
    if (hw == 0) t = (exrom == 1 && game == 0) ? 0xA1 : (exrom == 0 && game == 0) ? 0x01 : (exrom == 0 && game == 1) ? 0x41 : 0;
    else if (hw == 32) t = 0x11;
    else { printf("CRT type %d not in this bench\n", hw); exit(2); }
    const trx_cart_desc_t *e = nullptr;
    for (auto &c : c_cart_descs) if (c.cart_type == t) e = &c;
    for (int w = 0; w < 10; w++) top->cart_desc[w] = 0;
    if (e) for (int b = 0; b < TRX_CART_DESC_BYTES; b++) top->cart_desc[b / 4] |= uint32_t(e->desc[b]) << (8 * (b % 4));
    else { printf("no descriptor for CART_TYPE $%02X\n", t); exit(2); }
    return t;
}

static int boot(const char *state, int argc, char **argv)
{
    init();
    bool ready = false;
    for (int n = 0; n < 400 && !ready; n++) {
        run_ticks(320000);
        for (int r = 0; r < 25 && !ready; r++)
            if (row(r).rfind("READY.", 0) == 0) ready = true;
    }
    if (!ready) { printf("no READY\n"); return 3; }
    run_ticks(640000);
    save(state);
    printf("READY after %llu clocks\n", (unsigned long long)ticks);
    // cartridges: from power on with the CRT, until its test program (the runner at $0800) has ended
    for (int i = 0; i + 1 < argc; i += 2) {
        init();
        top->app_reset = 1;                     // C64::start_cartridge: reset, CART_TYPE, unreset
        int type = load_crt(argv[i]);
        run_ticks(64);
        top->cart_type = type;
        run_ticks(32 * 100);
        top->app_reset = 0;
        bool done = false;
        for (int n = 0; n < 400 && !done; n++) {
            run_ticks(320000);
            done = peek(0x07E7) == 0xAA;
        }
        if (!done) { printf("FAIL: %s: the test program did not end\n", argv[i]); return 3; }
        run_ticks(64000);
        save(argv[i + 1]);
        printf("%s: CART_TYPE $%02X, cartridge active %d, state %s\n", argv[i], type, top->cart_active, argv[i + 1]);
    }
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
static const uint16_t Q_HANDLER = 0xC100;
static const uint8_t PROG_Q_MAIN[] = {
    0x78,                                       // C000 SEI
    0xA9, 0x7F, 0x8D, 0x0D, 0xDC, 0xAD, 0x0D, 0xDC,
    0xA9, 0x00, 0x8D, 0x14, 0x03, 0xA9, 0xC1, 0x8D, 0x15, 0x03,   // $0314 = $C100
    0xA9, 0x50, 0x8D, 0x12, 0xD0, 0xA9, 0x1B, 0x8D, 0x11, 0xD0,
    0xA9, 0x01, 0x8D, 0x1A, 0xD0, 0x8D, 0x19, 0xD0,
    0xA9, 0x00, 0x8D, 0xF1, 0xC0, 0x8D, 0xF2, 0xC0,               // idx, flag
    0x58,                                       // CLI
    0xEE, 0xF0, 0xC0,                           // loop: INC $C0F0
    0x4C, 0x2E, 0xC0,                           //       JMP loop ($C02E)
};
static const uint8_t PROG_Q_IRQ[] = {
    0xAD, 0x12, 0xD0,                           // C100 LDA $D012 (the bk address)
    0xAE, 0xF1, 0xC0,                           //      LDX idx
    0x9D, 0x00, 0xC2,                           //      STA $C200,X
    0xAD, 0x04, 0xDC,                           //      LDA $DC04
    0x9D, 0x01, 0xC2,                           //      STA $C201,X
    0xE8, 0xE8, 0x8E, 0xF1, 0xC0,               //      INX INX STX idx
    0xAD, 0xF2, 0xC0, 0x49, 0x01, 0x8D, 0xF2, 0xC0,   // flag ^= 1
    0xF0, 0x04, 0xA9, 0xC8, 0xD0, 0x02, 0xA9, 0x50,   //      BEQ a / LDA #$C8 / BNE s / a: LDA #$50
    0x8D, 0x12, 0xD0,                           //      s: STA $D012
    0xA9, 0x01, 0x8D, 0x19, 0xD0,               //      acknowledge
    0x4C, 0x81, 0xEA,                           //      JMP $EA81 (pull Y, X, A, RTI)
};
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
// what an app freeze (C64_STOP + Ultimax) of the machine at the clock edge g shows
static void freeze_at(const char *state, uint32_t g, bool gate_frz = true)
{
    restore(state);
    if (gate_frz) twr(0, 0x100, 1);
    run_to_gclk(g - 16);
    top->app_stop = 1;
    wait_halted(true);
    CHECK(top->gclk_edges == g, "the app's freeze is on clock edge %u, the debugger's stop on %u", top->gclk_edges, g);
    top->app_ultimax = 1;
}
static std::vector<uint8_t> window(uint16_t lo, uint16_t hi)
{
    std::vector<uint8_t> v;
    for (uint32_t a = lo; a <= hi; a++) v.push_back(rd(a));
    return v;
}
// the lens unit (page 5 LK_REQ 0x020 / LK_RES 0x024, T21 §3.4.1): a look at the cartridge without a
// bus cycle, class 0 ROML, 1 ROMH, 2 IO1, 3 IO2
static uint8_t lens(int cls, uint16_t a)
{
    twr(5, 0x020, uint32_t(cls) << 16 | a);
    for (int n = 0; n < 200; n++) {
        uint32_t r = treg(5, 0x024);
        if (r & 0x100) return r & 0xFF;
    }
    printf("FAIL: the lens unit gave no answer\n");
    exit(4);
}
static std::vector<uint8_t> lens_window(int cls, uint16_t lo, uint16_t hi)
{
    std::vector<uint8_t> v;
    for (uint32_t a = lo; a <= hi; a++) v.push_back(lens(cls, a));
    return v;
}
static int vdiff(const std::vector<uint8_t> &a, const std::vector<uint8_t> &b, uint16_t base, const char *what)
{
    int d = 0;
    for (size_t i = 0; i < a.size(); i++)
        if (a[i] != b[i] && d++ < 6) printf("   %s $%04zX: stop %02X, freeze %02X\n", what, base + i, a[i], b[i]);
    return d;
}

static const char *MID = "dbgs_mid.state";
static const char *MIDQ = "dbgs_midq.state";

// ---- 1. a stop of every kind holds the machine; its window equals the freeze's --------------
enum Kind { K_BK, K_CB, K_IB, K_STEP };
static const char *KN[] = {"exec bitmap hit", "PAUSE_CB", "PAUSE_IB", "step end"};
static void stop_kind(Kind k)
{
    restore(MID);
    unsigned s0 = dstops();
    if (k == K_BK || k == K_STEP) bmp_set(X_LOOP);
    if (k == K_CB) dcmd(CMD_PAUSE_CB);
    if (k == K_IB) dcmd(CMD_PAUSE_IB);
    wait_stop(s0);
    if (k == K_STEP) {
        ck(dreason() == 3, "bk stop first (reason %u)", dreason());
        twr(3, 0x004, 1);
        s0 = dstops();
        dcmd(CMD_STEP_I);
        wait_stop(s0);
        ck(dreason() == 2, "the step's end is a stop of reason 2 (reason %u)", dreason());
        ck(!(treg(3, 0x010) >> 7 & 1), "a step with no window access before it has no held-CPU flag");
    }
}
static void test_window(const char *state, bool cart, const char *crt = nullptr)
{
    if (cart) load_crt(crt);                    // the image in DDR2 (the state has the cache and the type)
    for (int k = 0; k < (cart ? 3 : 4); k++) {
        if (cart && k != 2) continue;           // the cartridge program is a loop in RAM: PAUSE_IB
        if (cart) {
            restore(state);
            unsigned s0 = dstops();
            dcmd(CMD_PAUSE_IB);
            wait_stop(s0);
        } else {
            stop_kind(Kind(k));
        }
        top->app_ultimax = 1;                   // as the app's freeze has it: the I/O range is always there
        uint32_t g = top->gclk_edges;
        uint32_t cy_lens = cycles();
        ck(sh_status() == 0 && top->halted, "%s: no session (SH_STATUS %08X), the machine is held", cart ? "cart" : KN[k], sh_status());
        uint32_t cy0 = cycles();
        std::vector<uint8_t> a, ar, ah, io1;
        if (!cart) a = window(0xD000, 0xDFFF);
        else {                                  // the cartridge through the lens: no bus cycle
            a = lens_window(0, 0x8000, 0x9FFF);
            ar = lens_window(1, 0xE000, 0xFFFF);
            ah = lens_window(1, 0xA000, 0xBFFF);
            io1 = lens_window(2, 0xDE00, 0xDEFF);
            std::vector<uint8_t> io2 = lens_window(3, 0xDF00, 0xDFFF);
            io1.insert(io1.end(), io2.begin(), io2.end());
        }
        size_t n = a.size() + ar.size() + ah.size() + io1.size();
        if (!cart) ck(cycles() - cy0 >= n, "%s: %zu window reads are bus cycles (CYCLES +%u)", KN[k], n, cycles() - cy0);
        else ck(cycles() == cy_lens, "cart: %zu lens looks cost no machine cycle (CYCLES +%u)", n, cycles() - cy_lens);
        freeze_at(cart ? state : MID, g);
        std::vector<uint8_t> b, br, bh, bio;
        if (!cart) b = window(0xD000, 0xDFFF);
        else {
            b = lens_window(0, 0x8000, 0x9FFF);
            br = lens_window(1, 0xE000, 0xFFFF);
            bh = lens_window(1, 0xA000, 0xBFFF);
            bio = lens_window(2, 0xDE00, 0xDEFF);
            std::vector<uint8_t> bio2 = lens_window(3, 0xDF00, 0xDFFF);
            bio.insert(bio.end(), bio2.begin(), bio2.end());
        }
        int d = vdiff(a, b, cart ? 0x8000 : 0xD000, "window");
        if (cart) d += vdiff(ar, br, 0xE000, "ROMH") + vdiff(ah, bh, 0xA000, "$A000") + vdiff(io1, bio, 0xDE00, "IO");
        ck(d == 0, "%s: %zu %s bytes of the stop equal the app's freeze on the same edge (%d differ)",
           cart ? "cart" : KN[k], n, cart ? "lens" : "window", d);
        if (!cart) {
            std::vector<int> seen(256);
            for (uint8_t v : a) seen[v] = 1;
            int distinct = 0;
            for (int i : seen) distinct += i;
            ck(distinct > 8, "the window shows chip registers (%d distinct values)", distinct);
        }
        if (cart) {
            // the cartridge answers (not $FF, not RAM): the chips are P(bank, half, off), the boot code of
            // the test cartridge (the runner, LDX #0 = $A2 at $100) is in ROMH for the EasyFlash, in ROML
            // for the normal 16K one
            bool ef = strstr(crt, "ef.crt") != nullptr;
            auto P = [](int half, int off) { return uint8_t(half + (off >> 8) * 37 + 0x11); };
            int bad = 0;
            for (int off : {0x0600, 0x0745, 0x1234, 0x1FF0})        // beyond the boot code and the vectors
                if (a[off] != P(0, off) || ar[off] != P(1, off)) bad++;
            uint8_t want_l = ef ? P(0, 0x100) : 0xA2, want_h = ef ? 0xA2 : P(1, 0x100);
            printf("   ROML $8100 %02X (want %02X), ROMH $E100 %02X (want %02X), $A100 %02X, IO1 $DE00 %02X, IO2 $DF00 %02X\n",
                   a[0x100], want_l, ar[0x100], want_h, ah[0x100], io1[0], io1[0x100]);
            ck(bad == 0 && a[0x100] == want_l && ar[0x100] == want_h, "%s: ROML and ROMH of bank 0 show the cartridge's bytes in the stop (the lens)", crt);
        }
        top->app_stop = 0;
        top->app_ultimax = 0;
    }
}

// ---- 2. flags and side effects ---------------------------------------------------------
static void test_flags()
{
    restore(MID);
    unsigned s0 = dstops();
    bmp_set(X_LOOP);
    wait_stop(s0);
    std::vector<uint32_t> w0 = sb_dump();
    uint32_t cy0 = cycles();
    uint8_t icr_sb = sb_cia(1, 0xD), d019_sb = sb_vic(0x19);
    for (int i = 0; i < 6; i++) { sb_cia(1, 0xD); sb_vic(0x19); sb_cia(2, 0xD); }
    std::vector<uint32_t> w1 = sb_dump();
    ck(cycles() == cy0, "state bus looks cost no machine cycle (CYCLES +%u)", cycles() - cy0);
    ck(sb_diff(w0, w1, "state bus looks") == 0, "$DC0D, $DD0D, $D019 looked at over the state bus: the words are as they were");
    ck((icr_sb & 1) && (sb_cia(1, 0xD) & 1), "ICR flag is there and stays there (%02X)", icr_sb);
    ck((d019_sb & 1) && (sb_vic(0x19) & 1), "$D019 flag b0 is there and stays (%02X)", d019_sb);
    // the app's window: the same reads are bus cycles (as on a real C64 with a freezer cartridge)
    top->app_ultimax = 1;
    std::vector<uint32_t> o0 = sb_dump();
    uint32_t c0 = cycles();
    uint8_t o1 = rd(0xDC0D), o2 = rd(0xDC0D), o3 = rd(0xDC0D);
    uint32_t c1 = cycles();
    for (int i = 0; i < 5; i++) rd(0xDC04 + i % 2);
    uint32_t c2 = cycles();
    std::vector<uint32_t> oe = sb_dump();
    printf("   window: ICR reads %02X %02X %02X, CYCLES +%u\n", o1, o2, o3, c1 - c0);
    ck((o1 & 1) && !(o3 & 1), "the window's reads clear the ICR flag (%02X %02X %02X; the chip shows it for one more cycle)", o1, o2, o3);
    ck(c1 - c0 >= 3 && c2 - c1 == 5, "window reads are machine cycles (+%u for 3, then +%u for 5)", c1 - c0, c2 - c1);
    ck(sb_diff(o0, oe, "window reads") != 0, "the state words changed under the window's reads");
    top->app_ultimax = 0;
    dcmd(CMD_RUN);
    run_ticks(4000);
}

// ---- 3. writes in the stop ----------------------------------------------------------------
static void test_writes()
{
    restore(MID);
    unsigned s0 = dstops();
    bmp_set(X_LOOP);
    wait_stop(s0);
    // The first bus cycle of a stop lets the CPU run one or two cycles (c64_top: cpu_paused follows the
    // hold at the next phi2 fall) before the cycle comes: with the app's Ultimax on, the program would
    // fetch its operands from an Ultimax map. The app's freeze makes this cycle with Ultimax off first.
    rd(0x0002);
    top->app_ultimax = 1;
    uint8_t b0 = rd(0xD020), v0 = rd(0xD021);
    uint32_t ins0 = instrs();
    uint32_t cyc0 = cycles();
    uint8_t c0f0 = peek(0xC0F0);
    uint8_t want20 = uint8_t(b0 + 5) & 0x0F, want21 = uint8_t(v0 + 1) & 0x0F;
    wr(0xD020, want20);
    wr(0xD021, want21);
    wr(0xDC04, 0x34);                           // CIA 1 timer A latch, then LOAD: the order matters
    wr(0xDC05, 0x12);
    wr(0xDC0E, 0x11);
    for (int i = 0; i < 4; i++) rd(0xC000);     // the CIA takes the force-load a cycle later
    ck(cycles() - cyc0 >= 5, "the window writes are bus cycles (CYCLES +%u)", cycles() - cyc0);
    ck((sb_vic(0x20) & 0x0F) == want20, "$D020 is in the chip at once (%02X)", sb_vic(0x20));
    ck((sb_vic(0x21) & 0x0F) == want21, "$D021 is in the chip at once (%02X)", sb_vic(0x21));
    // the latch was written before the LOAD: the timer counts down from $1234
    uint32_t ta = sb_cia(1, 4) | sb_cia(1, 5) << 8;
    ck(ta <= 0x1234 && ta > 0x1200, "timer A loaded from the latch written first (%04X)", ta);
    ck((sb_cia(1, 0xE) & 0x0F) == 0x01, "CRA %02X", sb_cia(1, 0xE));
    // STEP: one instruction runs from the stop, with the app's Ultimax off as the app leaves it
    top->app_ultimax = 0;
    s0 = dstops();
    twr(3, 0x004, 1);
    dcmd(CMD_STEP_I);
    wait_stop(s0);
    uint32_t fl = treg(3, 0x010) >> 5 & 7;
    printf("   step after window accesses: stop record FLAGS %u (b2 = released a CPU held by bus DMA)\n", fl);
    ck(fl & 4, "the step after window bus cycles reports the CPU held by bus DMA (FLAGS %u)", fl);
    ck(instrs() == ins0 + 1, "one instruction ran (INSTRS +%u)", instrs() - ins0);
    ck(peek(0xC021) == 0xEE && peek(0xC0F0) == uint8_t(c0f0 + 1), "the instruction did what it does ($C0F0 %02X -> %02X)", c0f0, peek(0xC0F0));
    // RUN: the status shows the resume waiting for frame alignment (hold off, gate shut)
    twr(3, 0x048, 0);                           // the bitmap off, or the loop stops again at once
    dcmd(CMD_RUN);
    bool b6 = false, shut0 = false;
    for (uint64_t t0 = ticks; ticks - t0 < 3ull * FRAME && !shut0;) {
        uint32_t s = dstat();
        if ((s & 0x41) == 0x41) b6 = true;
        if (!(s & 1)) shut0 = true;
    }
    ck(b6, "after RUN the status showed hold off with the gate still shut (b6)");
    ck(shut0, "the gate opened after RUN");
    run_ticks(4000);
    ck(!top->dbg_hold_o, "after RUN: no hold");
    uint8_t cnt = peek(0xC0F0);
    run_ticks(20000);
    ck(peek(0xC0F0) != cnt, "the program goes on after RUN");
}


// ---- 5. the session registers are gone ---------------------------------------------------------
static void test_gone()
{
    restore(MID);
    ck(sh_regs_or() == 0, "SH_CTRL .. SH_COMMIT read 0 after reset (%08X)", sh_regs_or());
    sh_set(0x30F);                              // mode, SIDEFX, PROTO, abort, discard: nothing to take them
    run_ticks(100);
    ck(sh_regs_or() == 0, "and still read 0 after a write");
    unsigned s0 = dstops();
    dcmd(CMD_PAUSE_CB);
    wait_stop(s0);
    run_ticks(400);
    ck(top->dbg_hold_o && top->dbg_stop_o && sh_status() == 0, "a pause is a stop and no session (SH_STATUS %08X)", sh_status());
    uint32_t c0 = cycles();
    top->app_ultimax = 1;
    for (int i = 0; i < 8; i++) rd(0xDC04 + i % 2);
    uint32_t c1 = cycles();
    ck(c1 - c0 >= 8, "8 window reads are bus cycles (CYCLES +%u)", c1 - c0);
    wr(0xD020, 0x07);
    ck((sb_vic(0x20) & 0x0F) == 7, "a write reaches the chip at once (%02X)", sb_vic(0x20));
    top->app_ultimax = 0;
    dcmd(CMD_RUN);
    run_ticks(4000);
    ck(!top->dbg_hold_o, "RUN lets go");
    // TX_VIEW (page 5 0x028): the text layer's view, reset value and read/write
    ck(treg(5, 0x028) == 0x0243, "TX_VIEW reset value %04X (text mode, screen $0400, charset $0800, bank 0)", treg(5, 0x028));
    twr(5, 0x028, 0x1234);
    ck(treg(5, 0x028) == 0x1234, "TX_VIEW holds what was written (%04X)", treg(5, 0x028));
}

// ---- 7. the latency RUN or a step adds: from the command to the request being taken --------
static int rel_latency(uint32_t cmd)
{
    jpage(3);
    top->j_addr = 0; top->j_wdata = cmd; top->j_we = 1;
    tick();
    top->j_we = 0;
    int n = 1;
    while (!top->dbg_rel_o && n < 200) { tick(); n++; }
    int up = n;
    while (top->dbg_rel_o && n < 20000) { tick(); n++; }
    return n - up;
}
static void test_latency()
{
    for (int k = 0; k < 2; k++) {
        restore(MID);
        unsigned s0 = dstops();
        bmp_set(X_LOOP);
        wait_stop(s0);
        top->app_ultimax = 1;
        for (int i = 0; i < k * 20; i++) wr(0xDC06, uint8_t(i + 1));     // bus cycles before RUN change nothing
        top->app_ultimax = 0;
        twr(3, 0x048, 0);
        int l = rel_latency(CMD_RUN);
        printf("   RUN after %d window writes: the request is taken after %d clocks\n", k * 20, l);
        ck(l <= 3, "RUN after %d window writes is taken at once (%d clocks)", k * 20, l);
    }
}

// ---- 4. resume is bit-identical -----------------------------------------------------------
struct Result {
    uint32_t cyc = 0, ins = 0, ramcrc = 0, pixcrc = 0, bks = 0, stops = 0, looks = 0;
    std::vector<uint32_t> sb;
    std::vector<uint8_t> tab;
    uint32_t cyc_bad = 0;
};
static uint32_t TARGET;
// in a stop: what the monitor does, which must leave no trace: the state bus (the chips' words with
// their flags), the RAM through the second port, the window's RAM reads (dma_bus 0)
// the app's freeze menu: screen, charset area and colour RAM saved, drawn over and put back through the RAM's
// second port and the colour RAM's aperture (dma_rom, 0x6000 + offset): no bus cycle
static void menu_cycle()
{
    std::vector<uint8_t> rb(3072), cb(1024);
    for (int i = 0; i < 3072; i++) rb[i] = dma(0x0400 + i, false, 0, false);
    for (int i = 0; i < 1024; i++) cb[i] = dma(0x6000 + i, false, 0, false, true) & 0x0F;
    for (int i = 0; i < 3072; i++) dma(0x0400 + i, true, uint8_t(i * 7 + 3), false);
    for (int i = 0; i < 1024; i++) dma(0x6000 + i, true, uint8_t(i % 15 + 1), false, true);
    int bad = 0;
    for (int i = 0; i < 1024; i++) bad += (dma(0x6000 + i, false, 0, false, true) & 0x0F) != uint8_t(i % 15 + 1);
    for (int i = 0; i < 3072; i++) bad += dma(0x0400 + i, false, 0, false) != uint8_t(i * 7 + 3);
    CHECK(bad == 0, "the menu reads back wrong (%d)", bad);
    for (int i = 0; i < 3072; i++) dma(0x0400 + i, true, rb[i], false);
    for (int i = 0; i < 1024; i++) dma(0x6000 + i, true, cb[i], false, true);
}
static void stop_actions(Result &r, bool menu = false)
{
    uint32_t cy0 = cycles();
    if (menu) menu_cycle();
    sb_cia(1, 0xD); sb_vic(0x19); sb_vic(0x12); sb_cia(2, 0xD); sb_cia(1, 4);
    (void)sb_dump();
    peek(0xC0F0); peek(0xC200);
    dma(0xC0F1, false, 0, false);
    if (cycles() != cy0) r.cyc_bad++;
    r.looks++;
}
static Result run_q(int mode)           // 0 never stopped, 1 bk + RUN, 2 bk + a step chain + RUN, 3 bk + the app's menu + RUN
{
    restore(MIDQ);
    Result r;
    crc_on = true;
    uint32_t c0 = cycles();
    printf("   run %d: CYCLES at start %u, target %u\n", mode, c0, mode ? TARGET : c0 + 7 * 19656 + 500);
    if (mode == 0) TARGET = c0 + 7 * 19656 + 500;
    twr(0, 0x130, TARGET);                      // CYC_CMP: the stop that ends all three runs
    if (mode) bmp_set(Q_HANDLER);
    unsigned last = dstops();
    for (int guard = 0; guard < 400; guard++) {
        for (uint64_t t0 = ticks; dstops() == last;) if (ticks - t0 > 30ull * FRAME) { printf("FAIL: no stop (guard %d, %u handler stops, STATUS %08X, CYCLES %u)\n", guard, r.bks, dstat(), cycles()); exit(4); }
        last = dstops();
        for (int n = 0; n < 600 && !top->dbg_stop_o; n++) tick();
        unsigned why = dreason();
        if (why == 7) break;
        CHECK(why == 3, "stop reason %u at a handler stop", why);
        r.bks++;
        stop_actions(r, mode == 3);
        if (mode == 2) {
            bool end = false;
            for (int i = 0; i < 4 && !end; i++) {
                twr(3, 0x004, i == 3 ? 3 : 1);
                dcmd(i == 3 ? CMD_STEP_C : CMD_STEP_I);
                unsigned s1 = dstops();
                for (uint64_t t0 = ticks; dstops() == s1;) if (ticks - t0 > 2ull * FRAME) { printf("FAIL: step did not end\n"); exit(4); }
                last = dstops();
                for (int n = 0; n < 600 && !top->dbg_stop_o; n++) tick();
                if (dreason() == 7) end = true;
                else { CHECK(dreason() == 2, "step end reason %u", dreason()); stop_actions(r); }
            }
            if (end) break;
        }
        dcmd(CMD_RUN);
    }
    r.stops = dstops();
    r.cyc = cycles();
    r.ins = instrs();
    r.sb = sb_dump();
    crc_on = false;
    r.pixcrc = ~pix_crc;
    std::vector<uint8_t> m = ram();
    r.ramcrc = ~crc32(m.data(), m.size());
    r.tab.assign(m.begin() + 0xC200, m.begin() + 0xC240);
    return r;
}
static void test_resume()
{
    Result f = run_q(0);
    printf("   never stopped: CYCLES %u INSTRS %u, RAM crc %08X, pixel crc %08X; the IRQ log:", f.cyc, f.ins, f.ramcrc, f.pixcrc);
    for (int i = 0; i < 12; i += 2) printf(" %02X/%02X", f.tab[i], f.tab[i + 1]);
    printf(" ...\n");
    ck(f.tab[0] == 0x50 || f.tab[0] == 0x51, "the first logged raster line is the first interrupt's ($%02X)", f.tab[0]);
    int n_irq = 0;
    for (int i = 0; i < 64; i += 2) if (f.tab[i]) n_irq++;
    ck(n_irq >= 12, "%d interrupts logged in the free run", n_irq);
    for (int mode = 1; mode <= 3; mode++) {
        Result s = run_q(mode);
        const char *nm = mode == 1 ? "breakpoint + RUN" : mode == 2 ? "breakpoint + step chain + RUN" : "breakpoint + the app's menu (no bus cycle) + RUN";
        printf("   %s: %u breakpoint stops, %u stops with looks, %u stop records; CYCLES %u INSTRS %u, RAM crc %08X, pixel crc %08X\n",
               nm, s.bks, s.looks, s.stops, s.cyc, s.ins, s.ramcrc, s.pixcrc);
        ck(s.bks >= 10, "%s: %u stops (at least 10)", nm, s.bks);
        ck(s.cyc_bad == 0, "%s: the state bus, RAM and menu accesses in the stops cost no machine cycle", nm);
        ck(s.cyc == f.cyc && s.ins == f.ins, "%s: CYCLES %u/%u and INSTRS %u/%u equal the free run's", nm, s.cyc, f.cyc, s.ins, f.ins);
        ck(sb_diff(f.sb, s.sb, nm) == 0, "%s: CPU and chip state words equal the free run's", nm);
        ck(s.ramcrc == f.ramcrc, "%s: RAM (with the IRQ log: line and timer of every interrupt, none missed) equals", nm);
        ck(s.tab == f.tab, "%s: the IRQ log equals entry by entry", nm);
        ck(s.pixcrc == f.pixcrc, "%s: the pixel CRC over all frames equals (%08X / %08X)", nm, s.pixcrc, f.pixcrc);
    }
}

// ---- 8. a foreground app holds C64_STOP while the debugger owns the machine ------------------
// The loader holds C64_STOP for a foreground app (trxmon) the whole time. top.v masks it for the
// gate with OWNER: every debugger stop works however often it comes, and the colour RAM reads as
// bus cycles on the held machine.
static int cram_bad()
{
    int bad = 0;
    for (uint16_t a = 0xD800; a < 0xDC00 - 24; a++)
        if ((rd(a) & 0x0F) != 0x0E) bad++;      // BASIC's screen: colour 14 everywhere
    return bad;
}
static void test_foreground()
{
    restore(MID);
    own(true);
    top->app_stop = 1;                          // the loader's C64_STOP for the foreground app
    run_ticks(200);
    ck(!top->halted, "C64_STOP with OWNER does not halt the machine");
    bmp_set(X_LOOP);
    unsigned s0 = dstops();
    for (int i = 0; i < 6; i++) {
        wait_stop(s0);
        ck(sh_status() == 0, "stop %d with C64_STOP held: no session (SH_STATUS %08X)", i + 1, sh_status());
        uint32_t cy0 = cycles();
        int bad = cram_bad();
        ck(bad == 0 && cycles() - cy0 >= 1000, "stop %d: colour RAM reads right as bus cycles (%d of 1000 cells wrong, CYCLES +%u)", i + 1, bad, cycles() - cy0);
        s0 = dstops();
        if (i == 2) {                           // a step in the chain: its end is a stop as well
            twr(3, 0x004, 1);
            dcmd(CMD_STEP_I);
            wait_stop(s0);
            ck(cram_bad() == 0, "the step's end: colour RAM reads right");
            s0 = dstops();
        }
        dcmd(CMD_RUN);
    }
    // every iteration's stop came right after a RUN: that is the stop-after-resume case. The
    // last RUN leaves one more stop to come; the app lets go and the machine goes on
    own(false);
    top->app_stop = 0;
}

// ---- 9. the stops outlive run control (U5 D6) ------------------------------------------
// The firmware gives run control back whenever it stops the C64 for itself: trx_io.cc's
// trx_dbg_yield() writes OWNER 0 and then RUN, from C64::stop(), hard_stop() and
// freeze_slim(). What the debugger armed has to survive that, or a breakpoint set in the
// monitor would quietly stop firing after the first freeze, menu or reset — which is the
// whole promise of the debug service (docs/specs/U5-observer-service.md D6).
static void test_handback()
{
    restore(MID);
    unsigned s0 = dstops();
    bmp_set(X_LOOP);
    own(true);
    wait_stop(s0);
    ck(dreason() == 3, "the exec bitmap stops the machine (reason %u)", dreason());
    ck((dstat() >> 20) & 1, "and run control is the debugger's");

    own(false);        // exactly what trx_dbg_yield() writes
    dcmd(CMD_RUN);
    settle();
    ck(!((dstat() >> 20) & 1), "run control given back: OWNER is gone");

    unsigned s1 = dstops();
    wait_stop(s1);     // the program comes past the same address again
    ck(dreason() == 3, "still armed without OWNER: the bitmap stops it again (reason %u)", dreason());
    ck(!((dstat() >> 20) & 1), "and it stopped while the firmware owned the gate");
    dcmd(CMD_RUN);
    run_ticks(4000);
}

static int run(const char *state, int argc, char **argv)
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
    restore(state);
    install(0xC000, PROG_Q_MAIN, sizeof PROG_Q_MAIN);
    install(Q_HANDLER, PROG_Q_IRQ, sizeof PROG_Q_IRQ);
    type("SYS49152\r");
    run_ms(150);
    ck(peek(0xC0F1) >= 4, "program Q: %u interrupts so far", peek(0xC0F1) / 2);
    save(MIDQ);

    const char *only = getenv("DBGS_ONLY");        // a subset while developing: "1", "12", ...
    auto on = [&](char c) { return !only || strchr(only, c); };
    if (on('1')) {
        printf("1. stop, window = freeze\n");
        test_window(nullptr, false);
        for (int i = 0; i + 2 < argc; i += 3) {
            printf("1. cartridge look: %s\n", argv[i]);
            test_window(argv[i + 1], true, argv[i + 2]);
        }
    }
    if (on('2')) { printf("2. flags\n"); test_flags(); }
    if (on('3')) { printf("3. writes\n"); test_writes(); }
    if (on('4')) { printf("4. resume\n"); test_resume(); }
    if (on('5')) { printf("5. the session registers are gone\n"); test_gone(); }
    if (on('7')) { printf("7. latency\n"); test_latency(); }
    if (on('8')) { printf("8. foreground app holds C64_STOP\n"); test_foreground(); }
    if (on('9')) { printf("9. the stops outlive run control\n"); test_handback(); }
    printf("%d checks ok, %d failed\n", oks, fails);
    printf(fails ? "FAILED\n" : "PASS\n");
    return fails ? 1 : 0;
}

int main(int argc, char **argv)
{
    Verilated::commandArgs(argc, argv);
    top = new Vtb_dbgs_top;
    if (argc >= 3 && !strcmp(argv[1], "boot")) return boot(argv[2], argc - 3, argv + 3);
    if (argc >= 3 && !strcmp(argv[1], "run")) return run(argv[2], argc - 3, argv + 3);
    printf("usage: tb_dbgsess boot <state> [<crt> <state2>]... | run <state> [<name> <state2> <crt>]...\n");
    return 2;
}
