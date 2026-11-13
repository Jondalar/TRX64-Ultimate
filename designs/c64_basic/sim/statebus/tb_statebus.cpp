// SPDX-License-Identifier: GPL-3.0-or-later
// The state bus, the capture control, the app's freeze on a stopped machine and the C64 text layer under
// Verilator (ARCH-lean-state-access A1, T12 §8.1; SLIM-state-path.md).
//
//   tb_statebus boot <state>   cold start to READY, save the model: the ROM
//        block starts blank (no TRX_SIM_ROMS), the C64 is held in reset (the
//        app's C64_MODE) while the ROMs from mem/c64_rom.mem go in through
//        the app's ROM window (dma_rom, as ultimate_sys forwards the
//        apertures) and are read back through it; then the reset goes
//   tb_statebus run <state>    restore, then:
//     1. registers: ID and JTAG page; the app's byte path equals JTAG's words; SH_CTRL .. SH_COMMIT
//        (the freeze session of T12) read 0; TX_VIEW; the lens unit's registers
//     2. capture in one clock: CAP_EVERY = 1 while a program runs; the C64
//        clock never stops for it; the captured words equal the words of a
//        stop at the same edge (a second run from the same state, halted on
//        the capture's clock)
//     3. no side effects: halted, the state bus is read again and again: the
//        same words, no C64 clock; CIA 1's ICR flag stays set until a real
//        bus read clears it; VIC and CIA registers equal the bus reads
//     4. the app's freeze as C64::freeze_slim() makes it: C64_STOP stops the machine, the menu's screen,
//        charset and colour RAM go through the RAM's second port and the colour RAM's aperture (0x6000 +
//        offset, dma_rom): ZERO bus cycles, ZERO C64 clock edges, every word of every chip as it was, and
//        5 frames after the resume equal a run without the freeze (RAM, colour RAM, state bus, CRC;
//        STATEBUS_BUSCOLOR=1 plays the previous app with bus cycles for the colour RAM: it must FAIL);
//        inside the freeze the C64 text layer (c64_text.v) shows the menu byte-exact
//        on the 1080p output (every pixel of the text area, with the app's RAM window and JTAG using
//        the ports meanwhile), opaque with TX_VIEW's background, from the char ROM too, hidden by
//        TX_VIEW's bitmap/ECM bits and by TX_CTRL b0 = 0; the app puts the memory back, the machine
//        goes on; 4b: the text goes off when the machine runs, with the show bit still set; 4c: explicit
//        ROM reads through the window are bus cycles
//     5. a stop without Ultimax is real: a bus write lands in the VIC
//     7. GATE_CTRL b0 = 0 (reset value): the app's stop pauses only the CPU,
//        bus cycles reach the chips, the program goes on afterwards
//   tb_statebus look <state>   restore, then U13: the lens unit's look at the whole I/O page. A program sets
//        a CIA 1 ICR flag (IRQ masked), a sprite-sprite collision, a finished REU transfer and the turbo
//        registers ($D030 = 0, $D031 = $70: still 1 MHz); halted, every address $D000-$DFFF is looked at
//        twice (the same bytes, no C64 clock edge, the state bus unchanged), then each address: a look, a
//        REAL read (one bus cycle, side effects), a look: the REAL byte is the first or the second look
//        outside the open-bus bits; the side effects are seen (first look != second) where they exist; the
//        turbo registers, which a DMA read does not see, against the program's own reads of them
//   Exit code 0 = ok.
//
//   build.sh && cd build/run && ../tb_statebus boot ../tb.state && ../tb_statebus run ../tb.state
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "Vtb_sb_top.h"
#include "verilated.h"
#include "verilated_save.h"

static Vtb_sb_top *top;
static uint64_t ticks;
static int fails;

#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

// ---- the text layer's video side ------------------------------------------------
// The pixel clock runs only while pix_on: 125/26 of the C64 clock (vic_scaler.v).
// Each frame's text area (1280 x 800 from cx 1088, cy 185) is kept; pixels
// outside it that are not the flat picture below are counted.
static bool pix_on;
static uint32_t pacc;
static const int TXW = 1280, TXH = 800, TXX = 1088, TXY = 185;
static const uint32_t BELOW = 0x123456;
static std::vector<uint32_t> fb(TXW * TXH);
static uint32_t frames_done, outside_bad;
static void ptick()
{
    top->pclk = 1; top->eval();
    int x = top->o_cx, y = top->o_cy;
    if (x >= TXX && x < TXX + TXW && y >= TXY && y < TXY + TXH)
        fb[(y - TXY) * TXW + x - TXX] = top->o_rgb;
    else if (top->o_de && top->o_rgb != BELOW)
        outside_bad++;
    if (x == 2687 && y == 1124) frames_done++;
    top->pclk = 0; top->eval();
}
static void tick()
{
    // clk2x rises with every clk edge and falls halfway (T29)
    top->clk = 1; top->clk2x = 1; top->eval();
    top->clk2x = 0; top->eval();
    top->clk = 0; top->clk2x = 1; top->eval();
    top->clk2x = 0; top->eval();
    if (pix_on)
        for (pacc += 125; pacc >= 26; pacc -= 26) ptick();
    ticks++;
}
static void run_ticks(uint64_t n) { for (uint64_t i = 0; i < n; i++) tick(); }
static void run_ms(double ms) { run_ticks(uint64_t(ms * 31527.778)); }
static const uint32_t FRAME = 628992;

// ---- the C64 RAM's second port (JTAG's: it goes first) --------------------------
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

// ---- the app's DMA window (ultimate_sys's handshake) ------------------------------
static uint64_t dma_ticks;
static uint32_t bus_calls;               // window accesses made as bus cycles
static uint8_t dma(uint16_t a, bool we, uint8_t d, bool bus = true, bool rom = false)
{
    uint8_t done0 = top->dma_done_tgl;
    top->dma_addr = a; top->dma_we = we; top->dma_wdata = d; top->dma_bus = bus; top->dma_rom = rom;
    top->dma_req_tgl ^= 1;
    if (bus && !rom) bus_calls++;
    for (int n = 0; n < 32 * 200000; n++) {
        tick();
        if (top->dma_done_tgl != done0) { dma_ticks += n + 1; return top->dma_rdata; }
    }
    printf("FAIL: DMA %04X not done\n", a);
    exit(4);
}
static uint8_t rd(uint16_t a) { return dma(a, false, 0); }
static void wr(uint16_t a, uint8_t v) { dma(a, true, v); }
// RAM through the window's second port (what C64_DMA_MEMONLY gives the app) and the colour RAM through its
// aperture (dma_rom, 0x6000 + offset): no bus cycle. STATEBUS_BUSCOLOR=1 plays the previous app, which read
// and wrote the colour RAM as bus cycles with Ultimax on: the zero-cycle checks below must then fail.
static bool bus_color = getenv("STATEBUS_BUSCOLOR") != nullptr;
static uint8_t prd(uint16_t a) { return dma(a, false, 0, false); }
static void pwr(uint16_t a, uint8_t v) { dma(a, true, v, false); }
static uint8_t crd(int i) { return bus_color ? rd(0xD800 + i) : dma(0x6000 + i, false, 0, false, true); }
static void cwr(int i, uint8_t v) { if (bus_color) wr(0xD800 + i, v); else dma(0x6000 + i, true, v, false, true); }

// ---- trx_sys: JTAG words, the app's bytes ----------------------------------------
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
// a register at byte offset off of page p
static uint32_t treg(uint8_t p, uint16_t off) { jpage(p); return jrd(off >> 2); }
static void twr(uint8_t p, uint16_t off, uint32_t v) { jpage(p); jwr(off >> 2, v); }
static uint8_t brd(uint16_t a)
{
    uint8_t r0 = top->rs_tgl;
    top->rq_addr = a; top->rq_we = 0; top->rq_tgl ^= 1;
    for (int n = 0; n < 1000; n++) { tick(); if (top->rs_tgl != r0) return top->rs_rdata; }
    printf("FAIL: byte read %04X\n", a);
    exit(4);
}
static void bwr(uint16_t a, uint8_t v)
{
    uint8_t r0 = top->rs_tgl;
    top->rq_addr = a; top->rq_we = 1; top->rq_wdata = v; top->rq_tgl ^= 1;
    for (int n = 0; n < 1000; n++) { tick(); if (top->rs_tgl != r0) return; }
    printf("FAIL: byte write %04X\n", a);
    exit(4);
}
static uint32_t brd32(uint16_t a) { uint32_t v = 0; for (int i = 0; i < 4; i++) v |= uint32_t(brd(a + i)) << (8 * i); return v; }
static void bwr32(uint16_t a, uint32_t v) { for (int i = 0; i < 4; i++) bwr(a + i, v >> (8 * i)); }

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
static int sb_diff(const std::vector<uint32_t> &a, const std::vector<uint32_t> &b, const char *what, int skip_glue = 0)
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

// ---- machine control ------------------------------------------------------------
static void wait_halted(bool on)
{
    for (int n = 0; n < 3 * FRAME; n++) {
        if (top->halted == on) { tick(); tick(); return; }
        tick();
    }
    printf("FAIL: machine %s\n", on ? "did not stop" : "did not go on");
    exit(4);
}
static void jhalt(bool on) { top->jtag_halt = on; wait_halted(on); }
// run on until gclk_edges reaches g, then stop (at the next phi2 falling edge)
static void stop_at_gclk(uint32_t g)
{
    while (top->gclk_edges + 64 < g) tick();
    top->jtag_halt = 1;
    wait_halted(true);
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
    top->rst = 1;
    top->dbg_re = 0; top->dbg_addr = 0; top->dbg_we = 0; top->dbg_wdata = 0;
    top->jtag_halt = 0; top->jtag_cpu_only = 0; top->app_stop = 0; top->app_ultimax = 0; top->app_reset = 0;
    top->dma_req_tgl = 0; top->dma_addr = 0; top->dma_we = 0; top->dma_wdata = 0; top->dma_bus = 0;
    top->dma_rom = 0;
    top->j_re = 0; top->j_we = 0; top->j_addr = 0; top->j_wdata = 0;
    top->rq_tgl = 0; top->rq_addr = 0; top->rq_we = 0; top->rq_wdata = 0;
    top->keys = 0; top->joy1 = 0; top->joy2 = 0;
    top->tx_we = 0; top->tx_re = 0; top->tx_addr = 0; top->tx_wdata = 0; top->pclk = 0;
    for (int i = 0; i < 64; i++) tick();
    top->rst = 0;
}

static void save(const char *name)
{
    VerilatedSave os;
    os.open(name);
    os << ticks << page_now << *top;
}
static void restore(const char *name)
{
    VerilatedRestore is;
    is.open(name);
    is >> ticks >> page_now >> *top;
}

// the ROM block's bytes from rom2mem's file (char 0x0000, KERNAL 0x2000,
// BASIC 0x4000; 0x1000-0x1FFF and 0x6000-0x7FFF unused)
static std::vector<uint8_t> rom_file()
{
    std::vector<uint8_t> img;
    FILE *f = fopen("mem/c64_rom.mem", "r");
    unsigned w;
    while (f && img.size() < 0x8000 && fscanf(f, "%x", &w) == 1) {
        img.push_back(uint8_t(w));
        img.push_back(uint8_t(w >> 8));
    }
    if (f) fclose(f);
    return img;
}

static int boot(const char *state)
{
    init();
    // the app's boot (C64::init_system_roms) with the C64 held in reset
    std::vector<uint8_t> img = rom_file();
    if (img.size() != 0x8000) { printf("FAIL: no mem/c64_rom.mem\n"); return 1; }
    top->app_reset = 1;
    run_ticks(64);
    static const struct { uint16_t off, len; } part[] = {{0x0000, 0x1000}, {0x2000, 0x2000}, {0x4000, 0x2000}};
    for (auto &p : part)
        for (int i = 0; i < p.len; i++) dma(p.off + i, true, img[p.off + i], false, true);
    int bad = 0;
    for (auto &p : part)
        for (int i = 0; i < p.len; i++)
            if (dma(p.off + i, false, 0, false, true) != img[p.off + i] && bad++ < 4)
                printf("FAIL: ROM block %04X reads back wrong\n", p.off + i);
    if (bad) return 1;
    printf("ROMs written and read back through the app's window (20 KB)\n");
    top->app_reset = 0;
    for (int n = 0; n < 400; n++) {
        run_ticks(320000);
        for (int r = 0; r < 25; r++)
            if (row(r).rfind("READY.", 0) == 0) {
                run_ticks(640000);
                save(state);
                printf("READY after %llu clocks\n", (unsigned long long)ticks);
                return 0;
            }
    }
    printf("no READY\n");
    return 3;
}

// program B at $C000: SID voice 3 sawtooth, TOD started, then a loop that
// counts in $C100/1, copies $DC04, $D41B, $DC08, $D012 to $C102-5 and puts
// the raster line into the border (KERNAL IRQ on, screen on)
static const uint8_t PROG_B[] = {
    0xA9, 0xFF, 0x8D, 0x0E, 0xD4, 0x8D, 0x0F, 0xD4,
    0xA9, 0x21, 0x8D, 0x12, 0xD4,
    0xA9, 0x01, 0x8D, 0x0B, 0xDC,
    0xA9, 0x00, 0x8D, 0x0A, 0xDC, 0x8D, 0x09, 0xDC, 0x8D, 0x08, 0xDC,
    0xEE, 0x00, 0xC1, 0xD0, 0x03, 0xEE, 0x01, 0xC1,                 // $C01D
    0xAD, 0x04, 0xDC, 0x8D, 0x02, 0xC1,
    0xAD, 0x1B, 0xD4, 0x8D, 0x03, 0xC1,
    0xAD, 0x08, 0xDC, 0x8D, 0x04, 0xC1,
    0xAD, 0x12, 0xD0, 0x8D, 0x05, 0xC1, 0x8D, 0x20, 0xD0,
    0x4C, 0x1D, 0xC0,
};
// program A at $C200: SEI, CIA 1 interrupts masked (pending flags read
// away), then INC $C300 forever: CIA 1's timer A keeps setting its ICR flag
static const uint8_t PROG_A[] = {
    0x78, 0xA9, 0x7F, 0x8D, 0x0D, 0xDC, 0xAD, 0x0D, 0xDC,
    0xEE, 0x00, 0xC3, 0x4C, 0x09, 0xC2,                             // $C209
};
static uint16_t counter() { return peek(0xC100) | peek(0xC101) << 8; }

// the menu's charset and screen (tb_dma_io's patterns)
static uint8_t glyph(int c, int r) { return uint8_t(c * 0x1D + r * 0x47 + 0x11); }
static uint8_t scr_code(int i) { return uint8_t(i * 7 + i / 40); }
static uint8_t scr_col(int i) { return uint8_t(1 + i % 15); }

struct Snap { std::vector<uint8_t> ram, color; std::vector<uint32_t> sb; uint32_t crc; };
// RAM through the port, colour RAM through the window (bus cycles, Ultimax on so the I/O is visible),
// the state bus
static Snap snap(bool color)
{
    Snap s;
    s.ram = ram();
    if (color) { s.color.resize(1024); for (int i = 0; i < 1024; i++) s.color[i] = crd(i) & 0x0F; }
    s.sb = sb_dump();
    uint32_t c = crc32(s.ram.data(), s.ram.size());
    if (color) c = crc32(s.color.data(), s.color.size(), c);
    for (size_t i = 1; i < s.sb.size(); i++) c = crc32((const uint8_t *)&s.sb[i], 4, c);  // not the glue word
    s.crc = ~c;
    return s;
}
static int ram_diff(const std::vector<uint8_t> &a, const std::vector<uint8_t> &b, const char *what)
{
    int d = 0;
    for (size_t i = 0; i < a.size(); i++)
        if (a[i] != b[i] && d++ < 8) printf("   %s $%04zX: %02X vs %02X\n", what, i, a[i], b[i]);
    return d;
}

// the text layer's registers (page 6 offsets; byte 3 commits, byte 0 latches)
static void txwr(uint16_t off, uint32_t v)
{
    for (int i = 0; i < 4; i++) {
        top->tx_addr = off + i; top->tx_wdata = uint8_t(v >> (8 * i)); top->tx_we = 1;
        tick();
        top->tx_we = 0;
    }
}
static uint32_t txrd(uint16_t off)
{
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        top->tx_addr = off + i; top->tx_re = 1;
        tick();
        top->tx_re = 0;
        v |= uint32_t(top->tx_rdata) << (8 * i);
    }
    return v;
}
static const uint32_t PAL[16] = {
    0x000000, 0xFFFFFF, 0x813338, 0x75CEC8, 0x8E3C97, 0x56AC4D, 0x2E2C9B, 0xEDF171,
    0x8E5029, 0x553800, 0xC46C71, 0x4A4A4A, 0x7B7B7B, 0xA9FF9F, 0x706DEB, 0xB2B2B2,
};
// runs the pixel clock until a whole frame has gone out after now, calling
// busy() meanwhile (the app and JTAG using the ports)
static void pix_frame(void (*busy)(int))
{
    pix_on = true;
    uint32_t f0 = frames_done;
    int i = 0;
    while (frames_done < f0 + 2) {
        if (busy) busy(i++);
        else run_ticks(64);
    }
}
static void busy_ports(int i)
{
    dma(0x0400 + i % 1000, false, 0, false);    // RAM through the window's second port
    dma(0x0800 + i % 1000, true, glyph((i % 1000) / 8, (i % 1000) & 7), false);
    peek(0x0800 + i % 2048);                    // JTAG
}
// the frame against the expected text: gl(code, row) per C64 pixel, 4 x 4
// output pixels, colour col, background bg (-1: the picture below)
static int fb_diff(uint8_t (*code)(int), uint8_t (*gl)(int, int), uint8_t (*col)(int), int bg, bool all_below)
{
    int d = 0;
    for (int y = 0; y < TXH; y++)
        for (int x = 0; x < TXW; x++) {
            int px = x / 4, py = y / 4, cell = (py / 8) * 40 + px / 8;
            uint32_t want = BELOW;
            if (!all_below) {
                bool fg = gl(code(cell), py & 7) & (0x80 >> (px & 7));
                want = fg ? PAL[col(cell) & 15] : bg < 0 ? BELOW : PAL[bg];
            }
            uint32_t got = fb[y * TXW + x];
            if (got != want && d++ < 6)
                printf("   text layer: (%d, %d) cell %d: %06X, want %06X\n", x, y, cell, got, want);
        }
    return d;
}
static std::vector<uint8_t> char_rom;
static uint8_t rom_glyph(int c, int r) { return char_rom[c * 8 + r]; }
static uint8_t ram_glyph(int c, int r) { return glyph(c, r); }
static uint8_t s_code(int i) { return scr_code(i); }
static uint8_t s_col(int i) { return scr_col(i); }

static int run(const char *state)
{
    restore(state);
    {
        // the char ROM: c64_rom.mem's first 4 KB (16-bit words, low byte first)
        FILE *f = fopen("mem/c64_rom.mem", "r");
        unsigned w;
        while (f && char_rom.size() < 4096 && fscanf(f, "%x", &w) == 1) {
            char_rom.push_back(uint8_t(w));
            char_rom.push_back(uint8_t(w >> 8));
        }
        if (f) fclose(f);
        if (char_rom.size() < 4096) { printf("FAIL: no mem/c64_rom.mem\n"); return 1; }
    }

    // ---- 1. registers ----
    uint32_t id = treg(0, 0x000);
    printf("1. ID %08X, GATE_CTRL %08X, SH_CTRL %08X\n", id, treg(0, 0x100), treg(5, 0x000));
    CHECK(id == 0x32585254, "ID");
    CHECK((treg(0, 0x100) & 1) == 0, "GATE_CTRL FRZ not 0 after reset");
    {
        uint32_t o = 0;
        for (int k = 0; k <= 0x1C; k += 4) o |= treg(5, k);
        CHECK(o == 0, "SH_CTRL .. SH_COMMIT read %08X, the session is gone", o);
        twr(5, 0x000, 0x30F);
        CHECK(treg(5, 0x000) == 0 && treg(5, 0x004) == 0, "SH_CTRL takes writes");
        CHECK(treg(5, 0x080) == 0 && treg(5, 0x100) == 0, "the overlay window reads 0");
        CHECK(treg(5, 0x028) == 0x0243, "TX_VIEW reset value %04X", treg(5, 0x028));
        twr(5, 0x028, 0x5A5A);
        CHECK(brd32(0x5028) == 0x5A5A, "TX_VIEW by bytes %08X", brd32(0x5028));
        twr(5, 0x028, 0x0243);
    }
    twr(1, 0x000, 0x0103);
    CHECK(treg(1, 0x000) == 0x103, "SB_ADDR");
    bwr32(0x1000, 0x0281);                      // the app's bytes
    CHECK(treg(1, 0x000) == 0x281, "SB_ADDR by bytes");
    CHECK(brd32(0x5004) == treg(5, 0x004), "SH_STATUS by bytes");
    {   // the lens unit's registers (LK_REQ 0x5020, LK_RES 0x5024): no cartridge, so a look finds nothing, the
        // REAL cycle is refused while the machine runs
        bwr32(0x5020, 0x8000);
        uint32_t r = 0;
        for (int n = 0; n < 200; n++) { r = brd32(0x5024); if (!(r >> 31)) break; }
        CHECK((r & 0x80001FFF) == 0x1FF, "LK_RES after a look without a cartridge: %08X", r);
        bwr32(0x5020, 0x48000);
        for (int n = 0; n < 200; n++) { r = brd32(0x5024); if (!(r >> 31)) break; }
        CHECK((r & 0x80001FFF) == 0x11FF, "LK_RES REAL while running: %08X", r);
        printf("   lens unit registers: look %08X, REAL refused %08X\n", 0x1FF, r);
    }

    // program B, then the middle state for the runs below
    for (unsigned i = 0; i < sizeof PROG_B; i++) poke(0xC000 + i, PROG_B[i]);
    type("SYS49152\r");
    run_ms(100);
    uint16_t c0 = counter();
    run_ms(20);
    CHECK(counter() != c0, "program B does not run");
    const char *mid = "sb_mid.state";
    save(mid);

    // ---- 2. capture while running, against a stop at the same edge ----
    twr(0, 0x104, 1);                           // CAP_EVERY = 1
    uint8_t t0 = top->cap_tgl;
    uint64_t k0 = ticks;
    uint32_t g_before = top->gclk_edges;
    while (top->cap_tgl == t0 && ticks - k0 < 3 * FRAME) tick();
    CHECK(top->cap_tgl != t0, "no frame capture");
    uint64_t p_tick = ticks;                    // the tick whose edge was the snap
    uint32_t g_cap = top->gclk_edges;
    run_ticks(100);
    uint32_t g_after = top->gclk_edges;
    printf("2. capture at clock %llu (C64 clock edges %u): in the 100 clocks after it %u edges\n",
           (unsigned long long)(p_tick - k0), g_cap - g_before, g_after - g_cap);
    CHECK(g_after - g_cap == 100, "the C64 clock stopped for the capture");
    std::vector<uint32_t> w_cap = sb_dump();
    run_ms(5);                                  // the shadows hold the capture while it runs
    std::vector<uint32_t> w_cap2 = sb_dump();
    CHECK(sb_diff(w_cap, w_cap2, "capture then 5 ms later", 1) == 0, "shadows moved between captures");
    uint32_t cs = treg(0, 0x108);
    printf("   CAP_GEN %u, frames without a snap %u; PC %04X, raster line %u\n", cs & 0xFFFF, cs >> 16,
           w_cap[2] & 0xFFFF, (w_cap[4 + 4] >> 16 & 0xFF) | (w_cap[4 + 4] >> 7 & 0x100));
    CHECK((cs >> 16) == 0, "a frame without a snap");
    // the same run, stopped on that edge
    restore(mid);
    twr(0, 0x104, 1);
    while (ticks < p_tick - 1) tick();
    top->jtag_halt = 1;
    tick();
    wait_halted(true);
    std::vector<uint32_t> w_stop = sb_dump();
    printf("   stopped on the capture's edge: C64 clock edges %u (capture %u)\n",
           top->gclk_edges - g_before, g_cap - g_before);
    CHECK(top->gclk_edges == g_cap, "the stop is not on the capture's edge");
    CHECK(sb_diff(w_cap, w_stop, "capture vs stop", 1) == 0, "running capture differs from the stop");

    // ---- 3. no side effects ----
    restore(state);
    for (unsigned i = 0; i < sizeof PROG_A; i++) poke(0xC200 + i, PROG_A[i]);
    type("SYS49664\r");
    run_ms(60);
    jhalt(true);
    uint32_t g0 = top->gclk_edges;
    std::vector<uint32_t> s1 = sb_dump();
    std::vector<uint32_t> s2;
    for (int k = 0; k < 20; k++) s2 = sb_dump();
    uint8_t icr = sb_cia(1, 0xD);
    uint16_t ta = sb_cia(1, 4) | sb_cia(1, 5) << 8;
    printf("3. halted: CIA 1 ICR %02X, timer A %04X; 21 dumps, C64 clock edges +%u\n", icr, ta,
           top->gclk_edges - g0);
    CHECK(sb_diff(s1, s2, "dump 1 vs 21") == 0, "state bus words moved while halted");
    CHECK(top->gclk_edges == g0, "state bus reads cost C64 clocks");
    CHECK(icr & 0x01, "no timer A flag pending");
    // VIC and CIA registers against real bus reads (one cycle each)
    int vbad = 0, cbad = 0;
    for (int r = 0; r < 0x2F; r++) {
        if (r == 0x11 || r == 0x12 || r == 0x13 || r == 0x14 || r == 0x1E || r == 0x1F) continue;
        uint8_t s = sb_vic(r), b = rd(0xD000 + r);
        if (s != b && vbad++ < 4) printf("   $D0%02X: state bus %02X, bus %02X\n", r, s, b);
    }
    for (int r = 0; r < 0x10; r++) {
        if ((r >= 4 && r <= 0xB) || r == 0xD) continue;   // timers, TOD, ICR: below
        uint8_t s = sb_cia(1, r), b = rd(0xDC00 + r);
        if (s != b && cbad++ < 4) printf("   $DC%02X: state bus %02X, bus %02X\n", r, s, b);
        s = sb_cia(2, r); b = rd(0xDD00 + r);
        if (s != b && cbad++ < 4) printf("   $DD%02X: state bus %02X, bus %02X\n", r, s, b);
    }
    CHECK(vbad == 0, "%d VIC registers differ from bus reads", vbad);
    CHECK(cbad == 0, "%d CIA registers differ from bus reads", cbad);
    uint16_t ta_sb = sb_cia(1, 4) | sb_cia(1, 5) << 8;
    uint8_t lo = rd(0xDC04);
    uint16_t ta_bus = lo | rd(0xDC05) << 8;
    printf("   timer A: state bus %04X then bus %04X\n", ta_sb, ta_bus);
    CHECK((uint16_t)(ta_sb - ta_bus) <= 2, "timer A: state bus and bus apart");
    // a real read clears the flags; the 8521 still shows them in the next
    // cycle (reDIP's icr_prev), so the second bus read sees them too
    uint8_t icr_sb = sb_cia(1, 0xD);
    uint8_t icr_bus = rd(0xDC0D);
    uint8_t icr_bus2 = rd(0xDC0D);
    uint8_t icr_sb2 = sb_cia(1, 0xD);
    uint8_t icr_bus3 = rd(0xDC0D);
    printf("   ICR: state bus %02X, bus %02X %02X, state bus %02X, bus %02X\n", icr_sb, icr_bus, icr_bus2,
           icr_sb2, icr_bus3);
    CHECK((icr_sb & 1) && (icr_bus & 1), "ICR flag lost by state bus reads");
    CHECK(!(icr_sb2 & 1) && !(icr_bus3 & 1), "the bus read did not clear ICR (or the state bus does not follow)");
    // the app's byte path: SB_DATA as JTAG's word
    bwr32(0x1000, 0x0180);                      // CIA 1 w0
    uint32_t bw = brd32(0x1004);
    uint32_t jw = sb_word(3, 0);
    printf("   CIA 1 w0 by bytes %08X, by JTAG %08X\n", bw, jw);
    CHECK(bw == jw && bw != 0, "byte path and JTAG differ");
    jhalt(false);

    // ---- 4. the app's freeze on the slim core (C64::freeze_slim): no machine cycle, bit-identical resume ----
    // The reference: the same stop, no freeze, 5 frames on.
    restore(mid);
    twr(0, 0x100, 1);                           // GATE_CTRL: the app's stop pauses the machine
    top->app_stop = 1;
    wait_halted(true);
    uint32_t gs = top->gclk_edges;
    top->app_stop = 0;
    wait_halted(false);
    stop_at_gclk(gs + 5 * FRAME);
    uint32_t g_ref = top->gclk_edges;
    Snap ref = snap(true);
    top->jtag_halt = 0;

    restore(mid);
    twr(0, 0x100, 1);
    top->app_stop = 1;                          // C64::stop
    wait_halted(true);
    uint32_t gf = top->gclk_edges;
    CHECK(gf == gs, "not the reference's stop");
    if (bus_color) { top->app_ultimax = 1; run_ticks(4); }
    CHECK(treg(5, 0x004) == 0, "a session");
    Snap before = snap(true);                   // RAM through JTAG, colour RAM through the aperture, the state bus
    std::vector<uint8_t> ram0 = before.ram;
    bus_calls = 0;
    dma_ticks = 0;
    // the backup of what the menu overwrites (screen, charset area, colour RAM), then the charset and the menu
    std::vector<uint8_t> ram_backup(2048), color_backup(1024), screen_backup(1024);
    for (int i = 0; i < 2048; i++) ram_backup[i] = bus_color ? rd(0x0800 + i) : prd(0x0800 + i);
    for (int i = 0; i < 1024; i++) color_backup[i] = crd(i) & 0x0F;
    for (int i = 0; i < 1024; i++) screen_backup[i] = bus_color ? rd(0x0400 + i) : prd(0x0400 + i);
    for (int i = 0; i < 2048; i++) { if (bus_color) wr(0x0800 + i, glyph(i / 8, i & 7)); else pwr(0x0800 + i, glyph(i / 8, i & 7)); }
    for (int i = 0; i < 1000; i++) {
        if (bus_color) wr(0x0400 + i, scr_code(i)); else pwr(0x0400 + i, scr_code(i));
        cwr(i, scr_col(i));
    }
    bool same = true;
    for (int i = 0; i < 2048; i++) same &= ram_backup[i] == ram0[0x0800 + i];
    for (int i = 0; i < 1024; i++) same &= screen_backup[i] == ram0[0x0400 + i];
    CHECK(same, "RAM backup through the second port differs from the RAM");
    int cb = 0, sbd = 0, chb = 0;
    for (int i = 0; i < 1000; i++) {
        if ((crd(i) & 0x0F) != scr_col(i)) cb++;
        if (peek(0x0400 + i) != scr_code(i)) sbd++;
    }
    for (int i = 0; i < 2048; i++) if (peek(0x0800 + i) != glyph(i / 8, i & 7)) chb++;
    CHECK(cb == 0, "%d colour RAM bytes read back wrong", cb);
    CHECK(sbd == 0 && chb == 0, "menu in RAM: %d screen, %d charset bytes wrong", sbd, chb);
    // the C64 text layer: on, a frame to start, then a frame while the app's window and JTAG use the ports
    {
        uint32_t ctl0 = txrd(0x0C0);
        txwr(0x0C0, 1);
        uint32_t pc0 = top->tx_port_clocks;
        pix_frame(nullptr);
        outside_bad = 0;
        pix_frame(busy_ports);
        for (int i = 0; i < 1000; i++) poke(0x0800 + i, glyph(i / 8, i & 7));      // busy_ports wrote the same values
        uint32_t st = txrd(0x0C4);
        int d = fb_diff(s_code, ram_glyph, s_col, -1, false);
        printf("   text layer: TX_CTRL %08X, status %08X, %d of %d pixels wrong, %u outside, RAM port %u clocks\n",
               ctl0, st, d, TXW * TXH, outside_bad, top->tx_port_clocks - pc0);
        CHECK(ctl0 == 0x02000000, "TX_CTRL reads %08X", ctl0);
        CHECK((st & 1) && ((st >> 8) & 0xFF) == 0, "TX_STATUS %08X", st);
        CHECK(d == 0 && outside_bad == 0, "text layer not the menu");
        // opaque: the background is TX_VIEW's $D021 (reset 0), then colour 6
        txwr(0x0C0, 3);
        pix_frame(nullptr);
        int d2 = fb_diff(s_code, ram_glyph, s_col, 0, false);
        CHECK(d2 == 0, "opaque text layer: %d pixels wrong", d2);
        twr(5, 0x028, 0x0243 | 6 << 2);
        pix_frame(nullptr);
        int d2b = fb_diff(s_code, ram_glyph, s_col, 6, false);
        CHECK(d2b == 0, "opaque text layer with TX_VIEW background 6: %d pixels wrong", d2b);
        twr(5, 0x028, 0x0243);
        // the char ROM: TX_VIEW's $D018 = $14 (charset $1000 in bank 0)
        twr(5, 0x028, 0x14 >> 1 << 6 | 3);
        txwr(0x0C0, 1);
        pix_frame(nullptr);
        int d3 = fb_diff(s_code, rom_glyph, s_col, -1, false);
        printf("   opaque frame: %d pixels wrong; char ROM frame: %d\n", d2, d3);
        CHECK(d3 == 0, "char ROM text layer: %d pixels wrong", d3);
        twr(5, 0x028, 0x0243);
        // bitmap mode, ECM or multicolour in TX_VIEW: the layer is not drawn
        twr(5, 0x028, 0x0243 | 1 << 14);
        pix_frame(nullptr);
        uint32_t st_bm = txrd(0x0C4);
        int d4 = fb_diff(s_code, ram_glyph, s_col, -1, true);
        CHECK(d4 == 0 && !(st_bm & 1), "TX_VIEW bitmap: layer still shown (status %08X, %d pixels)", st_bm, d4);
        twr(5, 0x028, 0x0243);
    }
    // the menu is down (trx_video_menu_off: the text off)
    txwr(0x0C0, 0);
    pix_frame(nullptr);
    {
        int d = fb_diff(s_code, ram_glyph, s_col, -1, true);
        uint32_t st = txrd(0x0C4);
        printf("   text layer after TX_CTRL b0 = 0: %d pixels not the picture below, status %08X\n", d, st);
        CHECK(d == 0 && !(st & 1), "text layer still shown after TX_CTRL b0 = 0");
    }
    pix_on = false;
    // restore: memory back
    for (int i = 0; i < 2048; i++) { if (bus_color) wr(0x0800 + i, ram_backup[i]); else pwr(0x0800 + i, ram_backup[i]); }
    for (int i = 0; i < 1024; i++) cwr(i, color_backup[i]);
    for (int i = 0; i < 1024; i++) { if (bus_color) wr(0x0400 + i, screen_backup[i]); else pwr(0x0400 + i, screen_backup[i]); }
    top->app_ultimax = 0;
    run_ticks(4);
    // the freeze and menu cost the machine nothing: no clock edge, no bus cycle, every word of every chip as it was
    {
        uint32_t edges = top->gclk_edges - gf;
        printf("4. freeze: %u window accesses as bus cycles, C64 clock edges +%u\n", bus_calls, edges);
        CHECK(bus_calls == 0, "the freeze and menu made %u bus cycles", bus_calls);
        CHECK(edges == 0, "the freeze and menu cost %u C64 clock edges", edges);
    }
    Snap after = snap(true);
    printf("   CRC before %08X, after %08X\n", before.crc, after.crc);
    CHECK(ram_diff(before.ram, after.ram, "RAM") == 0, "RAM not restored");
    CHECK(ram_diff(before.color, after.color, "colour") == 0, "colour RAM not restored");
    CHECK(sb_diff(before.sb, after.sb, "state") == 0, "chip state changed");
    CHECK(before.crc == after.crc, "CRC differs");
    uint16_t cnt4 = counter();
    top->app_stop = 0;                          // C64_STOP = 0: the machine goes on
    wait_halted(false);
    stop_at_gclk(gs + 5 * FRAME);
    Snap got = snap(true);
    printf("   5 frames on: C64 clock edges %u (reference %u), CRC %08X (reference %08X)\n",
           top->gclk_edges - gs, g_ref - gs, got.crc, ref.crc);
    CHECK(top->gclk_edges == g_ref, "not the reference's clock count");
    CHECK(ram_diff(ref.ram, got.ram, "RAM vs reference") == 0, "RAM differs from the reference");
    CHECK(ram_diff(ref.color, got.color, "colour vs reference") == 0, "colour RAM differs from the reference");
    CHECK(sb_diff(ref.sb, got.sb, "state vs reference", 1) == 0, "chip state differs from the reference");
    CHECK(got.crc == ref.crc, "CRC differs from the reference");
    top->jtag_halt = 0;
    (void)cnt4;

    // ---- 4b. the text layer goes off when the machine runs, whatever ended the freeze ----
    restore(mid);
    twr(0, 0x100, 1);
    top->app_stop = 1;
    wait_halted(true);
    for (int i = 0; i < 1000; i++) { pwr(0x0400 + i, scr_code(i)); cwr(i, scr_col(i)); }
    for (int i = 0; i < 2048; i++) pwr(0x0800 + i, glyph(i / 8, i & 7));
    txwr(0x0C0, 1);                             // the app's show bit stays set: it is not what ends the text
    pix_on = true;
    pix_frame(nullptr);
    uint32_t st_on = txrd(0x0C4);
    top->app_stop = 0;
    wait_halted(false);
    pix_frame(nullptr);
    pix_frame(nullptr);
    uint32_t st_run = txrd(0x0C4);
    int d_run = fb_diff(s_code, ram_glyph, s_col, -1, true);
    printf("4b. text layer: status %08X paused, %08X running, %d pixels not the picture below\n", st_on, st_run, d_run);
    CHECK((st_on & 1) && !(st_run & 1) && d_run == 0, "the text layer stays over a running machine");
    txwr(0x0C0, 0);
    pix_on = false;

    // ---- 4c. explicit reads of ROM through the window (REST readmem, a monitor peek without a lens): bus
    // cycles with the PLA of the frozen $01, as a freezer cartridge's reads would be ----
    restore(mid);
    twr(0, 0x100, 1);
    top->app_stop = 1;
    wait_halted(true);
    top->app_ultimax = 0;
    run_ticks(4);
    uint8_t rb0 = rd(0xA000), rb1 = rd(0xA001), rk0 = rd(0xFFFC), rk1 = rd(0xFFFD), rc = rd(0xC000);
    wr(0xA000, 0x5A);
    uint8_t ram_a000 = peek(0xA000), rom_a000 = rd(0xA000);
    poke(0xA000, ram0[0xA000]);
    printf("   frozen PLA: $A000 %02X %02X, $FFFC %02X %02X, $C000 %02X, write under BASIC: RAM %02X read %02X\n",
           rb0, rb1, rk0, rk1, rc, ram_a000, rom_a000);
    CHECK(rb0 == 0x94 && rb1 == 0xE3 && rk0 == 0xE2 && rk1 == 0xFC, "ROM reads in the freeze");
    CHECK(rc == ram0[0xC000] && ram_a000 == 0x5A && rom_a000 == 0x94, "RAM under ROM in the freeze");
    top->app_stop = 0;
    wait_halted(false);
    top->jtag_halt = 0;

    // ---- 5. a stop without Ultimax is real ----
    restore(mid);
    twr(0, 0x100, 1);
    top->app_stop = 1;
    wait_halted(true);
    uint32_t g5 = top->gclk_edges;
    wr(0xD020, 0x05);
    uint8_t b5 = sb_vic(0x20);
    printf("5. plain stop: bus write $D020 = 5, state bus reads %02X, C64 clock edges +%u\n",
           b5, top->gclk_edges - g5);
    CHECK((b5 & 0x0F) == 5, "the write did not reach the VIC");
    CHECK(top->gclk_edges - g5 >= 32, "no bus cycle");
    top->app_stop = 0;
    wait_halted(false);

    // ---- 7. GATE_CTRL b0 = 0: CPU-only pause, real bus cycles ----
    restore(mid);
    CHECK((treg(0, 0x100) & 1) == 0, "GATE_CTRL FRZ set");
    top->app_stop = 1;
    run_ticks(2000);
    uint32_t g7 = top->gclk_edges;
    run_ticks(1000);
    uint16_t k7 = counter();
    top->app_ultimax = 1;
    wr(0xD020, 0x07);
    uint8_t b7 = rd(0xD020);
    run_ticks(1000);
    printf("7. CPU-only pause: halted %d, C64 clock edges +%u in 1000 clocks, $D020 %02X\n",
           top->halted, top->gclk_edges - g7, b7);
    CHECK(!top->halted && top->gclk_edges - g7 >= 1000, "the machine stopped");
    CHECK(b7 == 0xF7, "bus cycle did not reach the VIC");
    CHECK(counter() == k7, "the CPU ran while paused");
    top->app_ultimax = 0;
    top->app_stop = 0;
    run_ms(5);
    CHECK(counter() != k7, "the CPU did not go on");

    printf(fails ? "%d FAILED\n" : "ok\n", fails);
    return fails ? 1 : 0;
}

// ---- U13: the look at the I/O page --------------------------------------------------------------
// LK_REQ {REAL b18, class b17:16, address}, LK_RES {busy b31, open b13, refused b12, real b11, ext b10,
// hit b9, done b8, data}
static uint32_t lk(uint16_t a, int cls, bool real)
{
    bwr32(0x5020, uint32_t(a) | uint32_t(cls) << 16 | (real ? 1u << 18 : 0));
    uint32_t r = 0;
    for (int n = 0; n < 20000; n++) { r = brd32(0x5024); if (!(r >> 31)) return r; }
    printf("FAIL: lens unit busy at %04X\n", a);
    exit(4);
}
// program L at $C200: SEI, CIA 1 interrupts masked (pending flags read away: timer A keeps setting its
// ICR flag), sprites 0 and 1 on top of each other (data block 13, the bench fills it), $D030 = 0 then
// $D031 = $70 (speed index 0: 1 MHz), the CPU's own reads of $D030 $D031 $D03C $D071 $D131 into
// $C310-$C314, a REU transfer C64 $0400 -> REU 0, 1 byte, then INC $C300 forever
static const uint8_t PROG_L[] = {
    0x78, 0xA9, 0x7F, 0x8D, 0x0D, 0xDC, 0xAD, 0x0D, 0xDC,
    0xA9, 0x80, 0x8D, 0x00, 0xD0, 0x8D, 0x01, 0xD0, 0x8D, 0x02, 0xD0, 0x8D, 0x03, 0xD0,
    0xA9, 0x03, 0x8D, 0x15, 0xD0,
    0xA9, 0x00, 0x8D, 0x30, 0xD0,
    0xA9, 0x70, 0x8D, 0x31, 0xD0,
    0xAD, 0x30, 0xD0, 0x8D, 0x10, 0xC3, 0xAD, 0x31, 0xD0, 0x8D, 0x11, 0xC3, 0xAD, 0x3C, 0xD0, 0x8D, 0x12, 0xC3,
    0xAD, 0x71, 0xD0, 0x8D, 0x13, 0xC3, 0xAD, 0x31, 0xD1, 0x8D, 0x14, 0xC3,
    0xA9, 0x00, 0x8D, 0x02, 0xDF, 0x8D, 0x04, 0xDF, 0x8D, 0x05, 0xDF, 0x8D, 0x06, 0xDF, 0x8D, 0x08, 0xDF,
    0xA9, 0x04, 0x8D, 0x03, 0xDF,
    0xA9, 0x01, 0x8D, 0x07, 0xDF,
    0xA9, 0x90, 0x8D, 0x01, 0xDF,
    0xEE, 0x00, 0xC3, 0x4C, 0x64, 0xC2,                             // $C264
};
static int look(const char *state)
{
    restore(state);
    // the app's turbo setting: all three registers on (TurboEnable Bit mode), menu speed 1 MHz
    top->turbo_en = 0x07;
    top->turbo_prefer = 0x80;
    top->turbo_upd_tgl ^= 1;
    run_ticks(100);
    for (int i = 0; i < 64; i++) poke(0x0340 + i, 0xFF);
    poke(0x07F8, 0x0D);
    poke(0x07F9, 0x0D);
    for (unsigned i = 0; i < sizeof PROG_L; i++) poke(0xC200 + i, PROG_L[i]);
    type("SYS49664\r");
    run_ms(80);
    jhalt(true);

    // looked at twice, all of it: the same bytes, no C64 clock edge, no state-bus word moved
    uint32_t g0 = top->gclk_edges;
    std::vector<uint32_t> sb0 = sb_dump();
    std::vector<uint32_t> r1(4096), r2(4096);
    for (int a = 0; a < 4096; a++) r1[a] = lk(0xD000 + a, 2, false);
    for (int a = 0; a < 4096; a++) r2[a] = lk(0xD000 + a, 2, false);
    std::vector<uint32_t> sb1 = sb_dump();
    int notdone = 0, diff12 = 0, ext = 0, open = 0;
    for (int a = 0; a < 4096; a++) {
        if ((r1[a] & 0x1300) != 0x0300 && notdone++ < 4) printf("   look $%04X: LK_RES %08X\n", 0xD000 + a, r1[a]);
        if (r1[a] != r2[a] && diff12++ < 4) printf("   look $%04X twice: %08X %08X\n", 0xD000 + a, r1[a], r2[a]);
        ext += (r1[a] >> 10) & 1;
        open += (r1[a] >> 13) & 1;
    }
    printf("U13 look: 2 x 4096 looks, C64 clock edges +%u, %d ext, %d with open-bus bits\n",
           top->gclk_edges - g0, ext, open);
    printf("   $D030 %02X $D031 %02X $D071 %02X $D01E %02X $D019 %02X $DC0D %02X $DF00 %02X $D41B %02X $D800 %02X\n",
           r1[0x030] & 0xFF, r1[0x031] & 0xFF, r1[0x071] & 0xFF, r1[0x01E] & 0xFF, r1[0x019] & 0xFF,
           r1[0xC0D] & 0xFF, r1[0xF00] & 0xFF, r1[0x41B] & 0xFF, r1[0x800] & 0xFF);
    CHECK(notdone == 0, "%d looks not done, refused or without hit", notdone);
    CHECK(diff12 == 0, "%d looks differ the second time", diff12);
    CHECK(top->gclk_edges == g0, "the looks cost C64 clock edges");
    CHECK(sb_diff(sb0, sb1, "before vs after the looks") == 0, "state-bus words moved over the looks");
    CHECK(ext == 0, "%d looks answer ext (no socket, no port here)", ext);
    // the turbo registers against the CPU's own reads (a DMA read does not see them: the arbiter answers them to the
    // CPU only, U13 §4, so the REAL reads below give the VIC's $FF there)
    static const uint16_t TA[5] = {0x030, 0x031, 0x03C, 0x071, 0x131};
    for (int i = 0; i < 5; i++)
        CHECK((r1[TA[i]] & 0xFF) == peek(0xC310 + i), "turbo register $%04X: look %02X, the CPU read %02X",
              0xD000 + TA[i], r1[TA[i]] & 0xFF, peek(0xC310 + i));
    CHECK((r1[0x031] & 0xFF) == 0x70 && (r1[0x131] & 0xFF) == 0xFF, "the turbo register $D031, not at $D131");
    CHECK((r1[0x01E] & 0x03) == 0x03, "no sprite-sprite collision to look at");
    CHECK(r1[0xC0D] & 0x01, "no CIA 1 timer A flag to look at");
    CHECK(r1[0xF00] & 0x40, "no REU end of block to look at");

    // each address: look, REAL read, look; the REAL byte is one of the two looks (time moves one cycle with the
    // bus cycle), the open-bus bits aside
    int bad = 0, seen = 0, real_bad = 0;
    for (int a = 0; a < 4096; a++) {
        uint16_t ad = 0xD000 + a;
        uint32_t pre = lk(ad, 2, false);
        uint32_t b = lk(ad, 2, true);
        uint32_t post = lk(ad, 2, false);
        if ((b & 0x1F00) != 0x0B00) { if (real_bad++ < 4) printf("   REAL $%04X: LK_RES %08X\n", ad, b); continue; }
        uint8_t m = !(pre & 0x2000) ? 0xFF : (ad >= 0xD800 && ad < 0xDC00) ? 0x0F : 0x00;
        bool treg = ad < 0xD100 && ((ad & 0x3F) == 0x30 || (ad & 0x3F) == 0x31 || (ad & 0x3F) == 0x3C);
        if (treg) {                             // checked against the CPU above; a DMA read gets the VIC's $FF
            if ((b & 0xFF) != 0xFF && bad++ < 8) printf("   $%04X: a DMA read of a turbo register %02X\n", ad, b & 0xFF);
            continue;
        }
        bool ok = ((b ^ pre) & m) == 0 || ((b ^ post) & m) == 0;
        if (!ok && bad++ < 8)
            printf("   $%04X: look %02X, bus %02X, look %02X (mask %02X)\n", ad, pre & 0xFF, b & 0xFF, post & 0xFF, m);
        if (((pre ^ post) & m) != 0) seen++;
    }
    printf("   4096 x look/REAL/look: %d differ from both looks, %d where the bus read changed the look\n", bad, seen);
    CHECK(real_bad == 0, "%d REAL reads failed", real_bad);
    CHECK(bad == 0, "%d bytes: the look is not what the bus read", bad);
    CHECK(seen >= 3, "the side effects (ICR, collision, REU status) were not exercised");

    // running: the look still answers, now from the live registers
    jhalt(false);
    run_ms(2);
    uint32_t rr = lk(0xD031, 2, false);
    printf("   running: $D031 %08X\n", rr);
    CHECK((rr & 0x13FF) == 0x0370, "the look of $D031 while running");

    printf(fails ? "%d FAILED\n" : "ok\n", fails);
    return fails ? 1 : 0;
}

int main(int argc, char **argv)
{
    Verilated::commandArgs(argc, argv);
    top = new Vtb_sb_top;
    int rc = 3;
    if (argc >= 3 && !strcmp(argv[1], "boot")) rc = boot(argv[2]);
    else if (argc >= 3 && !strcmp(argv[1], "run")) rc = run(argv[2]);
    else if (argc >= 3 && !strcmp(argv[1], "look")) rc = look(argv[2]);
    else fprintf(stderr, "usage: %s boot|run|look <state>\n", argv[0]);
    top->final();
    delete top;
    return rc;
}
