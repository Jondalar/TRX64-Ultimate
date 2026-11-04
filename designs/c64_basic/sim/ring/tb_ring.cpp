// SPDX-License-Identifier: GPL-3.0-or-later
// The frame ring under Verilator (T20 §13.1, ARCH A3): trx_engine.v on the
// machine of sim/statebus, mig_arb and the MIG UI model.
//
//   tb_ring boot <state>   cold start to READY, save the model
//   tb_ring run <state>    restore, then:
//     1. registers: page 2 through JTAG and through the app's bytes
//     (The areas are fixed, SLIM D4: the area registers read 0 and take no writes; no wrap test is left.)
//     2. ring: CAP_CTRL 1 and LOG_CTRL on while a program runs, 24 frames:
//        one marker per frame (consecutive frame numbers, snap on line 0 from
//        x 96, cycle count steps of one frame +- an instruction), a chip block
//        per frame, never torn, its words equal the shadows read over the
//        state bus after it was written; the C64 clock never stops
//     3. LOG_CTRL off: no more records
//     4. mover: COPY of the stream 2 area, FILL; the job-done event
//   tb_ring bootef <state>  an EasyFlash with a program of ours instead of
//                           BASIC (Ultimax, ROMH at $E000), running; save
//   tb_ring log <state>     restore the EasyFlash state, then (write log, M-b):
//     5. the CPU paused (CPU only: no writes, the cartridge's queue empties),
//        C64 RAM, colour RAM and cartridge RAM read; log on with every tap;
//        8 frames of a loop writing all three (INC, so the CPU's dummy
//        writes too, and JSR's stack writes), a JTAG poke from outside and a
//        (LOG_PUT is gone, SLIM D6: it makes no record); paused again and read again. Streams 0 and 1
//        replayed forward from the first reading: every record's old byte is
//        the byte there, the result is the second reading; backward (undo)
//        from the second gives the first. Each marker's stream counts equal
//        the bench's own count of the writes made up to its boundary edge;
//        the poke sets the marker's outside flag; nothing lost.
//   tb_ring snap <state>    restore the BASIC state (snap condition, M-c):
//     6. program B with an X- and Y-expanded sprite that is still being drawn
//        on line 0 (Y = $2C: lines 300-311 and 0-29): every frame's snap comes
//        after the sprite's last pixel; without the sprite before it; how
//        many snaps are interrupt entries (marker b4)
//     7. torn-block sweep (ARCH §12 A3): a capture every frame; for every
//        clock offset from -40 to +320 around one capture edge a separate run
//        from the same state asks for a stop there; per offset the block is
//        torn (flagged), absent (the stop came first) or whole; a whole block
//        must equal the block of the run without a stop. Prints the torn
//        window in clocks.
//   tb_ring restore <state> restore the BASIC state (M-d, T20 §13.1-3):
//     8. round trip: a program without the SID (its own IRQ handler on CIA 1's
//        timer A, CIA 2's timer B running, TOD read, $D020 written, RAM
//        written all over) runs with the log and a capture every frame; frame
//        N is captured, the machine runs on 8 frames and stops anywhere; then
//        frame N is restored: RAM by undoing stream 0 back to N's marker, the
//        VIC registers by bus replay (DMA, the CPU held), a stop on line 0
//        before the phi2 fall that ended N's snap cycle, the CPU's and both
//        CIAs' every flip-flop staged from N's chip block and loaded on that
//        edge. The 6 frames after it must equal the 6 frames after N: every
//        chip block word (all 58), every C64 RAM write record (address, old,
//        new), the cycle count of each frame, the snap positions, the flags.
//        The CIA registers the subset load leaves out (port and timer
//        latches, ICR mask, TOD alarm) are replayed over the bus first;
//        TB_RING_NOREPLAY=1 skips that (it must then differ unless the build
//        has SB_FULL), TB_RING_NOLOAD=1 skips the load.
//   tb_ring trace <state>   restore the BASIC state (T21 §4.11.1):
//     9. the trace former: a loop of known instructions with the interrupts off, log and
//        trace on for 5 frames; stream 3's records (the model's DDR2) are the instruction
//        sequence: pc, opcode, operand bytes, the registers before (a software model),
//        the cycle step of each, the write counts and stream 0's records at write_start
//   tb_ring scrub <state>   restore the BASIC state, no program (idle in the KERNAL wait loop):
//    10. the Timewarp scrub step as t20_apply.py does it: land on frame T (undo the last frame run,
//        base -> T over the ring, VIC/CIA registers by bus cycles, the CPU held to the snap
//        position, RAM, the staged block loaded), one frame with the log on, stop on line 0 cycle 4.
//        Per pair: T by the engine's APPLY, an other frame, T again by the CPU walk (marks, every
//        byte once, RAM-only): 64 KB and the chip block (but the glue word's frame counters) must be
//        equal. TB_SCRUB_PAIRS=n (default 4, at most 12), TB_SCRUB_DELAY=ticks held before the CPU
//        walk. TB_SCRUB_WALKBUS=1 makes the walk write $0000/$0001 as a bus cycle (what the app
//        did with C64_STOP set, ultimate_sys.vhd T16 Q8): the pairs then differ at $01F2, the
//        board finding of 2026-10-08.
//   tb_ring restore with TB_RING_IDLE=1 runs the round trip without a program, TB_RING_APPLY=1 puts
//   RAM back with the engine's APPLY, TB_RING_DELAY=ticks waits held before the load.
//   Exit code 0 = ok.
//
//   build.sh && cd build/run && ../tb_ring boot ../tb.state && ../tb_ring run ../tb.state
//   ../tb_ring bootef ../ef.state && ../tb_ring log ../ef.state
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include "Vtb_ring_top.h"
#include "Vtb_ring_top___024root.h"
#include "verilated.h"
#include "verilated_save.h"

static Vtb_ring_top *top;
static uint64_t ticks;
static int fails;
static bool ui_on = false;

#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

// sys_clk 31.5278 MHz, ui_clk 75 MHz: half periods in ps
static const uint64_t HS = 15859, HU = 6667;
static uint64_t t_now, t_sys, t_ui, t_c2 = ~0ull;
static void tick()
{
    uint64_t end = t_now + 2 * HS;
    for (;;) {
        uint64_t nx = t_sys;
        if (t_c2 < nx) nx = t_c2;
        if (ui_on && t_ui < nx) nx = t_ui;
        if (nx > end || (nx == end && t_sys != end)) break;
        t_now = nx;
        // clk2x (the SID engine's, T29's CPU and RAM port): rises with every clk edge, falls halfway
        if (t_c2 == nx) { top->clk2x = 0; t_c2 = ~0ull; }
        if (t_sys == nx) { top->clk = !top->clk; t_c2 = nx + HS / 2; top->clk2x = 1; t_sys += HS; }
        if (ui_on && t_ui == nx) { top->ui_clk = !top->ui_clk; t_ui += HU; }
        top->eval();
        if (t_now == end) break;
    }
    if (!ui_on) t_ui = t_now;
    ticks++;
}
static void run_ticks(uint64_t n) { for (uint64_t i = 0; i < n; i++) tick(); }
static void run_ms(double ms) { run_ticks(uint64_t(ms * 31527.778)); }
static const uint32_t FRAME = 628992;

// ---- DDR2 (the model's memory, 64 MB, wrapping) ----------------------------------
static uint32_t d32(uint32_t b)
{
    auto &m = top->rootp->tb_ring_top__DOT__mig__DOT__mem;
    return m[(b >> 4) & ((1 << 22) - 1)][(b >> 2) & 3];
}
static uint64_t d64(uint32_t b) { return d32(b) | uint64_t(d32(b + 4)) << 32; }
static void dw32(uint32_t b, uint32_t v)
{
    auto &m = top->rootp->tb_ring_top__DOT__mig__DOT__mem;
    m[(b >> 4) & ((1 << 22) - 1)][(b >> 2) & 3] = v;
}

// ---- the C64 RAM's second port ----------------------------------------------------
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

// the capture table's words in table order (the reset table: glue 1, CPU 3,
// VIC 12, CIA 5 + 5, cartridge 1)
static const int NTAB = 9;
static const int TAB_CHIP[NTAB] = {0, 1, 2, 3, 4, 7, 1, 3, 4};
static const int TAB_W0[NTAB] = {0, 0, 0, 0, 0, 0, 8, 8, 8};
static const int TAB_N[NTAB] = {1, 3, 12, 5, 5, 1, 7, 12, 12};
static const int NW = 58;
static std::vector<uint32_t> sb_dump()
{
    std::vector<uint32_t> w;
    for (int c = 0; c < NTAB; c++) {
        twr(1, 0x000, TAB_CHIP[c] << 7 | TAB_W0[c]);
        for (int i = 0; i < TAB_N[c]; i++) w.push_back(treg(1, 0x004));
    }
    return w;
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
    os << ticks << page_now << t_now << t_sys << t_ui << t_c2 << ui_on << *top;
}
static void restore(const char *name)
{
    VerilatedRestore is;
    is.open(name);
    is >> ticks >> page_now >> t_now >> t_sys >> t_ui >> t_c2 >> ui_on >> *top;
}

static int boot(const char *state)
{
    init();
    for (int n = 0; n < 400; n++) {
        run_ticks(320000);
        for (int r = 0; r < 25; r++)
            if (row(r).rfind("READY.", 0) == 0) {
                run_ticks(640000);
                ui_on = true;                       // the MIG model calibrates
                run_ticks(2000);
                save(state);
                printf("READY after %llu clocks\n", (unsigned long long)ticks);
                return 0;
            }
    }
    printf("no READY\n");
    return 3;
}

// program B of sim/statebus: SID voice 3, TOD, a loop writing $C100-$C105
// and $D020 (KERNAL IRQ on)
static const uint8_t PROG_B[] = {
    0xA9, 0xFF, 0x8D, 0x0E, 0xD4, 0x8D, 0x0F, 0xD4,
    0xA9, 0x21, 0x8D, 0x12, 0xD4,
    0xA9, 0x01, 0x8D, 0x0B, 0xDC,
    0xA9, 0x00, 0x8D, 0x0A, 0xDC, 0x8D, 0x09, 0xDC, 0x8D, 0x08, 0xDC,
    0xEE, 0x00, 0xC1, 0xD0, 0x03, 0xEE, 0x01, 0xC1,
    0xAD, 0x04, 0xDC, 0x8D, 0x02, 0xC1,
    0xAD, 0x1B, 0xD4, 0x8D, 0x03, 0xC1,
    0xAD, 0x08, 0xDC, 0x8D, 0x04, 0xC1,
    0xAD, 0x12, 0xD0, 0x8D, 0x05, 0xC1, 0x8D, 0x20, 0xD0,
    0x4C, 0x1D, 0xC0,
};

// ---- the app's DMA window (ultimate_sys's handshake): real bus cycles here ----
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

// ---- page 2 -----------------------------------------------------------------------
enum { LOG_CTRL = 0x000, LOG_STAT = 0x004, FRAMEN = 0x008, DROPPED = 0x00C, ENG_CMD = 0x080, ENG_SRC = 0x084, ENG_DST = 0x088,
       ENG_LEN = 0x08C, ENG_ARG = 0x090 };
static uint32_t LOG_BASE(int n) { return 0x020 + 16 * n; }
static uint32_t LOG_SIZE(int n) { return 0x024 + 16 * n; }   // LOG_MASK: size - 1
static uint32_t LOG_CNT(int n) { return 0x028 + 16 * n; }

// The ring areas are fixed in the engine (SLIM D4): stream 0 16 MB at 0x7000000, stream 1 8 MB at 0x6000000,
// stream 2 4 MB at 0x5C00000. The model's memory is 64 MB and wraps, so they sit there at 0x3000000,
// 0x2000000 and 0x1C00000. LOG_BASE / LOG_MASK (0x020-0x044) are gone: writes change nothing.
static const uint32_t B0 = 0x7000000, S0 = 0x1000000, B1 = 0x6000000, S1 = 0x800000, B2 = 0x5C00000, S2 = 0x400000;
static uint32_t s2_size = S2;               // the stream 2 area the parser reads
static uint64_t rec2(uint32_t i) { return d64(B2 + (i * 8) % s2_size); }

struct Frame {
    uint32_t frame, flags, gen, line, x, n0, n1;
    uint64_t cycles;
    bool block;
    std::vector<uint32_t> w;
    uint32_t bflags, bwords, gen0, gen1;
};
// parse stream 2 from record i on; returns the index after the frame's records
static bool parse_frame(uint32_t &i, uint32_t end, Frame &f)
{
    if (end - i < 4) return false;
    uint64_t m0 = rec2(i), m1 = rec2(i + 1), m2 = rec2(i + 2), m3 = rec2(i + 3);
    if (m0 >> 56 != 0xF1 || m1 >> 56 != 0xF2 || m2 >> 56 != 0xF3 || m3 >> 56 != 0xF4) {
        printf("   stream 2 record %u: %016llX %016llX %016llX %016llX\n", i, (unsigned long long)m0,
               (unsigned long long)m1, (unsigned long long)m2, (unsigned long long)m3);
        return false;
    }
    f.frame = uint32_t(m0); f.gen = uint32_t(m0 >> 32) & 0xFFFF; f.flags = uint32_t(m0 >> 48) & 0xFF;
    f.cycles = m1 & 0xFFFFFF;
    f.line = uint32_t(m2 >> 40) & 0x1FF; f.x = uint32_t(m2 >> 24) & 0x3FF;
    f.n0 = uint32_t(m3) & 0xFFFFFFF; f.n1 = uint32_t(m3 >> 28) & 0xFFFFFFF;
    i += 4;
    f.block = false;
    if (!(f.flags & 2)) return true;
    uint32_t nrec = (NW + 1) / 2;
    if (end - i < nrec + 2) return false;
    uint64_t bh = rec2(i);
    if (bh >> 56 != 0xB1) { printf("   no block header at %u: %016llX\n", i, (unsigned long long)bh); return false; }
    f.w.clear();
    for (uint32_t k = 0; k < nrec; k++) {
        uint64_t r = rec2(i + 1 + k);
        f.w.push_back(uint32_t(r));
        if (f.w.size() < NW) f.w.push_back(uint32_t(r >> 32));
    }
    uint64_t bt = rec2(i + 1 + nrec);
    if (bt >> 56 != 0xB2) { printf("   no block tail at %u: %016llX\n", i + 1 + nrec, (unsigned long long)bt); return false; }
    f.bflags = uint32_t(bt >> 48) & 0xFF; f.bwords = uint32_t(bt >> 32) & 0xFFFF;
    f.gen1 = uint32_t(bt >> 16) & 0xFFFF; f.gen0 = uint32_t(bt) & 0xFFFF;
    f.block = true;
    i += nrec + 2;
    return true;
}

static int run(const char *state)
{
    restore(state);

    // ---- 1. registers ----
    // the old area registers: writes change nothing (the records below land in the fixed areas)
    twr(2, LOG_BASE(0), 0x0400000); twr(2, LOG_SIZE(0), 0xFFFF);
    bwr32(0x2000 + LOG_BASE(1), 0x0500000); bwr32(0x2000 + LOG_SIZE(1), 0xFFF);   // the app's bytes
    twr(2, LOG_BASE(2), 0x0600000); twr(2, LOG_SIZE(2), 0x7FF);
    CHECK(treg(2, LOG_BASE(0)) == 0 && treg(2, LOG_SIZE(2)) == 0 && treg(2, 0x050) == 0, "the area registers read 0");
    twr(2, 0x050, 0x30000123); twr(2, 0x054, 0x0000AB00 | 0x00000011);          // LOG_PUT: gone, no record (stream 1 counts below)
    twr(2, 0x060, 2);
    twr(2, 0x064, (12u << 16) | (2 << 7));         // the reset table's entry 2 again
    uint32_t ls = brd32(0x2000 + LOG_STAT);
    printf("1. LOG_STAT %08X (app's bytes)\n", ls);
    CHECK(ls == 0, "registers");

    // the program, then the ring
    for (unsigned i = 0; i < sizeof PROG_B; i++) poke(0xC000 + i, PROG_B[i]);
    type("SYS49152\r");
    run_ms(60);
    twr(0, 0x104, 1);                               // CAP_CTRL: every frame
    twr(2, LOG_CTRL, 0x101);                        // restart, on

    // ---- 2. ring ----
    const int NF = 24;
    uint32_t i2 = 0, cnt2 = 0;
    std::vector<Frame> fr;
    uint64_t k0 = ticks; uint32_t g0 = top->gclk_edges;
    int compared = 0, mism = 0;
    for (int k = 0; k < NF; k++) {
        uint8_t t0 = top->cap_tgl;
        uint64_t s = ticks;
        while (top->cap_tgl == t0 && ticks - s < 2 * FRAME) tick();
        CHECK(top->cap_tgl != t0, "no capture in frame %d", k);
        // wait for the block in DDR2 (20 records per frame), then the shadows
        uint64_t w0 = ticks;
        do { run_ticks(200); cnt2 = treg(2, LOG_CNT(2)); } while (cnt2 < uint32_t(35 * (k + 1)) && ticks - w0 < 100000);
        std::vector<uint32_t> sb = sb_dump();
        Frame f;
        if (!parse_frame(i2, cnt2, f)) {
            CHECK(false, "frame %d not parsed (count %u at %u)", k, cnt2, i2);
            for (uint32_t j = 0; j < cnt2 + 2; j++) printf("   s2[%u] %016llX\n", j, (unsigned long long)rec2(j));
            for (uint32_t j = 0; j < 4; j++) printf("   s0[%u] %08X\n", j, d32(B0 + 4 * j));
            break;
        }
        fr.push_back(f);
        if (f.block) {
            compared++;
            for (int j = 0; j < NW; j++)
                if (sb[j] != f.w[j] && !(j == 0)) {       // glue w0 holds the live halted bit and CAP_GEN
                    if (mism++ < 8) printf("   frame %u word %d: ring %08X, bus %08X\n", f.frame, j, f.w[j], sb[j]);
                }
            CHECK((f.w[0] >> 16) == f.gen0, "glue word CAP_GEN %u vs block %u", f.w[0] >> 16, f.gen0);
        }
    }
    uint64_t dt = ticks - k0; uint32_t dg = top->gclk_edges - g0;
    printf("2. %zu frames, %d blocks compared with the shadows, %d words differ; clocks %llu, C64 clock edges %u\n",
           fr.size(), compared, mism, (unsigned long long)dt, dg);
    CHECK(fr.size() == NF && compared == NF && mism == 0, "ring contents");
    CHECK(dg == dt, "the C64 clock stopped");
    for (size_t k = 0; k < fr.size(); k++) {
        const Frame &f = fr[k];
        if (k < 3 || k + 1 == fr.size())
            printf("   frame %u flags %02X CAP_GEN %u cycles %llu at line %u x %u, counts %u/%u, block %u words flags %02X gen %u..%u\n",
                   f.frame, f.flags, f.gen, (unsigned long long)f.cycles, f.line, f.x, f.n0, f.n1,
                   f.bwords, f.bflags, f.gen0, f.gen1);
        CHECK(f.frame == k, "frame number %u at %zu", f.frame, k);
        CHECK(f.flags == 0x03, "flags %02X", f.flags);
        CHECK(f.line == 0 && f.x >= 96 && f.x < 96 + 8 * 16, "snap position line %u x %u", f.line, f.x);
        CHECK(f.block && f.bwords == NW && f.bflags == 0 && f.gen0 == f.gen && f.gen1 == f.gen, "block of frame %u", f.frame);
        if (k) {
            int64_t d = int64_t((f.cycles - fr[k - 1].cycles) & 0xFFFFFF) - 19656;
            CHECK(d > -16 && d < 16, "frame %u: %lld cycles after the previous", f.frame, (long long)(d + 19656));
        }
    }
    CHECK(cnt2 * 8 <= S2 && treg(2, LOG_CNT(1)) == 0, "stream 2 stays in its fixed area (%u records), the LOG_PUT made no stream 1 record (%u)", cnt2, treg(2, LOG_CNT(1)));
    uint32_t frames_reg = treg(2, FRAMEN);
    printf("   FRAME %u, DROPPED %u, LOG_CNT %u/%u/%u, LOG_STAT %X\n", frames_reg,
           treg(2, DROPPED), treg(2, LOG_CNT(0)), treg(2, LOG_CNT(1)), treg(2, LOG_CNT(2)), treg(2, LOG_STAT));
    CHECK(treg(2, DROPPED) == 0, "lost records");

    // ---- 3. off ----
    twr(2, LOG_CTRL, 0);
    twr(0, 0x104, 0);
    run_ticks(200);
    uint32_t n_off = treg(2, LOG_CNT(2));
    run_ticks(3 * FRAME);
    printf("3. log off: stream 2 count %u -> %u over 3 frames\n", n_off, treg(2, LOG_CNT(2)));
    CHECK(treg(2, LOG_CNT(2)) == n_off, "records with the log off");

    // ---- 4. mover ----
    for (uint32_t a = 0; a < 0x800; a += 4) dw32(0x0700000 + a, 0xDEADBEEF);
    uint8_t e0 = top->eng_tgl;
    twr(2, ENG_SRC, B2); twr(2, ENG_DST, 0x0700000); twr(2, ENG_LEN, 0x800); twr(2, ENG_CMD, 1);
    CHECK(treg(2, ENG_CMD) == 1, "mover not busy after COPY");
    uint64_t c0 = ticks;
    while (treg(2, ENG_CMD) & 1) run_ticks(100);
    int cd = 0;
    for (uint32_t a = 0; a < 0x800; a += 4) if (d32(B2 + a) != d32(0x0700000 + a)) cd++;
    printf("4. COPY %u bytes in %llu clocks, %d words differ; job-done event %s\n", 0x800,
           (unsigned long long)(ticks - c0), cd, top->eng_tgl != e0 ? "yes" : "no");
    CHECK(cd == 0 && top->eng_tgl != e0, "COPY");
    twr(2, ENG_DST, 0x0710000); twr(2, ENG_LEN, 0x100); twr(2, ENG_ARG, 0x5AA51234); twr(2, ENG_CMD, 2);
    while (treg(2, ENG_CMD) & 1) run_ticks(100);
    int fd = 0;
    for (uint32_t a = 0; a < 0x100; a += 4) if (d32(0x0710000 + a) != 0x5AA51234) fd++;
    CHECK(fd == 0 && d32(0x0710100) != 0x5AA51234, "FILL (%d words wrong)", fd);

    printf(fails ? "FAILED (%d)\n" : "PASS\n", fails);
    return fails ? 1 : 0;
}

// ---- M-b: the write log -----------------------------------------------------------
// the EasyFlash program (ROMH at $E000, Ultimax): a loop over X = 0..63 doing
// INC $DF00,X (cartridge RAM), LDA $DF00,X, STA $0400,X, STA $D800,X, JSR to
// an RTS, then INC $0800
static const uint8_t EF_PROG[] = {
    0x78, 0xD8, 0xA2, 0xFF, 0x9A, 0xA9, 0x00, 0x8D, 0x20, 0xD0,     // $E000
    0xA2, 0x00,                                                     // $E00A
    0xFE, 0x00, 0xDF, 0xBD, 0x00, 0xDF, 0x9D, 0x00, 0x04, 0x9D, 0x00, 0xD8,
    0x20, 0x30, 0xE0, 0xE8, 0xE0, 0x40, 0xD0, 0xEC,
    0xEE, 0x00, 0x08, 0x4C, 0x0A, 0xE0,                             // $E020
};
static const uint32_t CART_IMG = 0x3C00000, CART_RAM = 0x0EF0000;
static void db(uint32_t b, uint8_t v)
{
    uint32_t w = d32(b & ~3u);
    w = (w & ~(0xFFu << (8 * (b & 3)))) | uint32_t(v) << (8 * (b & 3));
    dw32(b & ~3u, w);
}
static uint8_t dbr(uint32_t b) { return uint8_t(d32(b & ~3u) >> (8 * (b & 3))); }
static uint8_t cram(int i) { return top->rootp->tb_ring_top__DOT__c64__DOT__c64__DOT__color_mem[i] & 0xF; }

static int bootef(const char *state)
{
    ui_on = true;
    top->cart_type = 0x11;
    for (uint32_t a = 0; a < 0x4000; a++) db(CART_IMG + a, 0xEA);
    for (unsigned i = 0; i < sizeof EF_PROG; i++) db(CART_IMG + 0x2000 + i, EF_PROG[i]);
    db(CART_IMG + 0x2030, 0x60);                                    // RTS
    db(CART_IMG + 0x2031, 0x40);                                    // RTI
    db(CART_IMG + 0x3FFA, 0x31); db(CART_IMG + 0x3FFB, 0xE0);
    db(CART_IMG + 0x3FFC, 0x00); db(CART_IMG + 0x3FFD, 0xE0);
    db(CART_IMG + 0x3FFE, 0x31); db(CART_IMG + 0x3FFF, 0xE0);
    for (uint32_t a = 0; a < 0x100; a++) db(CART_RAM + a, uint8_t(a * 3));
    init();
    run_ticks(3 * FRAME);
    uint8_t c0 = peek(0x0800);
    run_ticks(FRAME / 2);
    uint8_t c1 = peek(0x0800);
    printf("EasyFlash program: $0800 %02X -> %02X, $0400 %02X, cartridge RAM $DF00 %02X\n", c0, c1,
           peek(0x0400), dbr(CART_RAM));
    if (c0 == c1) { printf("the program does not run\n"); return 3; }
    save(state);
    return 0;
}

struct Mem { std::vector<uint8_t> ram, col, cart; };
static Mem read_all()
{
    Mem m;
    m.ram.resize(65536); for (int a = 0; a < 65536; a++) m.ram[a] = peek(uint16_t(a));
    m.col.resize(1024); for (int a = 0; a < 1024; a++) m.col[a] = cram(a);
    m.cart.resize(256); for (int a = 0; a < 256; a++) m.cart[a] = dbr(CART_RAM + a);
    return m;
}
static void cpu_pause(bool on)
{
    top->app_stop = on;
    run_ticks(4000);                                // the CPU's read cycle, the cartridge queue, the drain
}

static int logtest(const char *state)
{
    restore(state);
    const uint32_t LB0 = B0, LS0 = S0, LB1 = B1, LS1 = S1, LS2 = S2;
    s2_size = LS2;
    twr(0, 0x104, 1);
    cpu_pause(true);
    Mem m0 = read_all();
    uint32_t b0 = top->n_s0, b1 = top->n_s1;
    twr(2, LOG_CTRL, 0x10F);                        // restart; on, outside, cartridge, colour
    cpu_pause(false);

    // 8 frames; the markers' counts as the bench sees them; a poke
    std::vector<std::pair<uint32_t, uint32_t>> bmk;
    uint32_t nmk = top->n_mk;
    int poke_frame = -1;
    uint64_t t0 = ticks;
    while (bmk.size() < 8 && ticks - t0 < 10 * FRAME) {
        tick();
        if (top->n_mk != nmk) {
            nmk = top->n_mk;
            bmk.push_back({top->n_s0_mk - b0, top->n_s1_mk - b1});
            if (bmk.size() == 3) {
                run_ticks(5000);
                poke(0x0F00, 0xA5);                 // from outside, while it runs
                dma(0x6000 + 0x3A, true, 0x0B, false, true);   // the app's colour RAM aperture: an outside colour RAM write
                poke_frame = 3;                     // in the frame that ends at marker 3
            }
        }
    }
    cpu_pause(true);
    run_ticks(20000);                               // the drain
    Mem m1 = read_all();
    uint32_t c0 = treg(2, LOG_CNT(0)), c1 = treg(2, LOG_CNT(1)), c2 = treg(2, LOG_CNT(2));
    uint32_t dropped = treg(2, DROPPED);
    twr(2, LOG_CTRL, 0);
    cpu_pause(false);
    printf("5. 8 frames: stream 0 %u, stream 1 %u, stream 2 %u records; bench %u / %u writes; DROPPED %08X\n",
           c0, c1, c2, top->n_s0 - b0, top->n_s1 - b1, dropped);
    CHECK(dropped == 0, "records lost");
    CHECK(c0 * 4 <= LS0 && c1 * 8 <= LS1, "areas too small for the test");
    CHECK(c0 == top->n_s0 - b0 && c1 == top->n_s1 - b1, "record counts against the bench's");

    // forward replay
    Mem m = m0;
    int bad0 = 0, bad1 = 0, put = 0, ext_ram = 0, ext_col = 0;
    std::vector<uint32_t> r0(c0);
    std::vector<uint64_t> r1(c1);
    for (uint32_t i = 0; i < c0; i++) {
        uint32_t r = r0[i] = d32(LB0 + 4 * i);
        uint16_t a = r & 0xFFFF; uint8_t o = r >> 16, n = r >> 24;
        if (m.ram[a] != o && bad0++ < 5) printf("   stream 0 #%u $%04X old %02X, memory %02X\n", i, a, o, m.ram[a]);
        m.ram[a] = n;
        if (a == 0x0F00 && n == 0xA5) ext_ram++;
    }
    for (uint32_t i = 0; i < c1; i++) {
        uint64_t r = r1[i] = d64(LB1 + 8 * i);
        uint32_t a = r & 0xFFFFFFF, sp = (r >> 28) & 0xF;
        uint8_t o = r >> 32, n = r >> 40;
        if (sp == 1) {
            if ((r >> 63) && (a & 0x3FF) == 0x3A && (n & 0xF) == 0xB) ext_col++;
            if (m.col[a & 0x3FF] != o && bad1++ < 5) printf("   stream 1 #%u colour %03X old %X, memory %X\n", i, a, o, m.col[a & 0x3FF]);
            m.col[a & 0x3FF] = n & 0xF;
        } else if (sp == 2) {
            if (m.cart[a & 0xFF] != o && bad1++ < 5) printf("   stream 1 #%u cart %02X old %02X, memory %02X\n", i, a, o, m.cart[a & 0xFF]);
            m.cart[a & 0xFF] = n;
        } else if (sp == 3) put++;
        else if (bad1++ < 5) printf("   stream 1 #%u: %016llX\n", i, (unsigned long long)r);
    }
    int e_ram = 0, e_col = 0, e_cart = 0;
    for (int a = 0; a < 65536; a++) e_ram += m.ram[a] != m1.ram[a];
    for (int a = 0; a < 1024; a++) e_col += m.col[a] != m1.col[a];
    for (int a = 0; a < 256; a++) e_cart += m.cart[a] != m1.cart[a];
    printf("   forward: old bytes wrong %d / %d; after the replay RAM %d, colour %d, cartridge RAM %d bytes differ\n",
           bad0, bad1, e_ram, e_col, e_cart);
    CHECK(bad0 == 0 && bad1 == 0 && e_ram == 0 && e_col == 0 && e_cart == 0, "forward replay");
    CHECK(put == 0 && ext_ram == 1, "no LOG_PUT record %d, the poke %d", put, ext_ram);
    CHECK(ext_col == 1, "the app's colour RAM aperture write is one stream 1 record with the outside bit (%d)", ext_col);
    // undo
    m = m1;
    for (uint32_t i = c0; i-- > 0;) m.ram[r0[i] & 0xFFFF] = r0[i] >> 16;
    for (uint32_t i = c1; i-- > 0;) {
        uint32_t sp = (r1[i] >> 28) & 0xF, a = r1[i] & 0xFFFFFFF;
        if (sp == 1) m.col[a & 0x3FF] = (r1[i] >> 32) & 0xF;
        if (sp == 2) m.cart[a & 0xFF] = r1[i] >> 32;
    }
    CHECK(m.ram == m0.ram && m.col == m0.col && m.cart == m0.cart, "undo does not give the first reading");

    // the markers
    uint32_t i2 = 0;
    int mk_bad = 0;
    CHECK(poke_frame == 3, "no poke");
    for (size_t k = 0; k < bmk.size(); k++) {
        Frame f;
        if (!parse_frame(i2, c2, f)) { CHECK(false, "marker %zu not parsed", k); break; }
        bool ext = f.flags & 8;
        uint32_t put_n = 0;
        if ((f.n0 != bmk[k].first || f.n1 != bmk[k].second + put_n || ext != (int(k) == poke_frame)) && mk_bad++ < 5)
            printf("   marker %u: counts %u/%u, bench %u/%u, flags %02X\n", f.frame, f.n0, f.n1,
                   bmk[k].first, bmk[k].second, f.flags);
        if (k < 2) printf("   marker %u: counts %u/%u (bench %u/%u), flags %02X\n", f.frame, f.n0, f.n1,
                          bmk[k].first, bmk[k].second, f.flags);
    }
    CHECK(mk_bad == 0, "marker counts or outside flag");
    printf(fails ? "FAILED (%d)\n" : "PASS\n", fails);
    return fails ? 1 : 0;
}

// ---- M-c: the snap condition, the torn-block sweep ------------------------------------
static const uint8_t SPRITE_SETUP[] = {
    0xA9, 0x2C, 0x8D, 0x01, 0xD0,             // Y = $2C
    0xA9, 0xE0, 0x8D, 0x00, 0xD0,             // X = $1E0
    0xA9, 0x01, 0x8D, 0x10, 0xD0,
    0x8D, 0x15, 0xD0, 0x8D, 0x1D, 0xD0, 0x8D, 0x17, 0xD0,   // on, X and Y expanded
    0x4C, 0x00, 0xC0,                         // program B
};
// the markers of n frames from now on (stream 2 at B2, restarted here)
static std::vector<Frame> frames(int n)
{
    s2_size = S2;
    twr(0, 0x104, 1);
    twr(2, LOG_CTRL, 0x101);
    std::vector<Frame> fr;
    uint32_t i2 = 0;
    run_ticks(uint64_t(n) * FRAME + 20000);
    uint32_t c2 = treg(2, LOG_CNT(2));
    while ((int)fr.size() < n) {
        Frame f;
        if (!parse_frame(i2, c2, f)) {
            if (i2 + 4 < c2) {
                printf("   stream 2: %u records, parsed up to %u\n", c2, i2);
                for (uint32_t j = i2; j < c2 && j < i2 + 24; j++) printf("   s2[%u] %016llX\n", j, (unsigned long long)rec2(j));
            }
            break;
        }
        fr.push_back(f);
    }
    twr(2, LOG_CTRL, 0);
    twr(0, 0x104, 0);
    return fr;
}
static void xs(const std::vector<Frame> &fr, unsigned &lo, unsigned &hi, int &intg, int &nosnap)
{
    lo = 999; hi = 0; intg = 0; nosnap = 0;
    for (const Frame &f : fr) {
        if (!(f.flags & 1)) { nosnap++; continue; }
        if (f.x < lo) lo = f.x;
        if (f.x > hi) hi = f.x;
        if (f.flags & 0x10) intg++;
    }
}

static int snaptest(const char *state)
{
    // ---- 6. a sprite on line 0 ----
    restore(state);
    for (unsigned i = 0; i < sizeof PROG_B; i++) poke(0xC000 + i, PROG_B[i]);
    type("SYS49152\r");
    run_ms(60);
    std::vector<Frame> f0 = frames(16);
    restore(state);
    for (unsigned i = 0; i < sizeof PROG_B; i++) poke(0xC000 + i, PROG_B[i]);
    for (unsigned i = 0; i < sizeof SPRITE_SETUP; i++) poke(0xC300 + i, SPRITE_SETUP[i]);
    for (int i = 0; i < 64; i++) poke(0x0340 + i, 0xFF);
    poke(0x07F8, 0x0D);
    type("SYS49920\r");
    run_ms(60);
    std::vector<Frame> f1 = frames(16);
    unsigned lo0, hi0, lo1, hi1; int in0, in1, ns0, ns1;
    xs(f0, lo0, hi0, in0, ns0);
    xs(f1, lo1, hi1, in1, ns1);
    printf("6. snap x without the sprite %u..%u (%d interrupt entries, %d without a snap), "
           "with it %u..%u (%d, %d), %zu/%zu frames\n", lo0, hi0, in0, ns0, lo1, hi1, in1, ns1, f0.size(), f1.size());
    CHECK(f0.size() >= 15 && f1.size() >= 15 && ns0 == 0 && ns1 == 0, "markers");   // the first frame arms
    CHECK(lo1 > hi0, "the snap did not wait for the sprite");

    // ---- 7. torn-block sweep ----
    restore(state);
    for (unsigned i = 0; i < sizeof PROG_B; i++) poke(0xC000 + i, PROG_B[i]);
    type("SYS49152\r");
    run_ms(60);
    s2_size = S2;
    twr(0, 0x104, 1);
    twr(2, LOG_CTRL, 0x101);
    // up to the first capture, then save shortly before the second one
    uint8_t t0 = top->cap_tgl;
    while (top->cap_tgl == t0) tick();
    run_ticks(FRAME - 3000);
    const char *pre = "sweep_pre.state";
    save(pre);
    uint64_t base_tick = ticks;
    t0 = top->cap_tgl;
    while (top->cap_tgl == t0) tick();
    uint64_t cap_tick = ticks - 1;                  // the clock that loaded the shadows
    run_ticks(5000);
    uint32_t c2 = treg(2, LOG_CNT(2));
    uint32_t i2 = 0;
    Frame ref, f;
    while (parse_frame(i2, c2, f)) ref = f;
    CHECK(ref.block && ref.bflags == 0, "no reference block");
    printf("7. reference: frame %u, capture at clock +%llu after the saved state\n", ref.frame,
           (unsigned long long)(cap_tick - base_tick));
    int whole = 0, torn = 0, none = 0, mixed = 0;
    int torn_lo = 9999, torn_hi = -9999;
    for (int off = -40; off <= 320; off++) {
        restore(pre);
        while (ticks < cap_tick + off) tick();
        top->jtag_halt = 1;
        run_ticks(6000);                            // stopped; the copy and the drain finish
        uint32_t n2 = treg(2, LOG_CNT(2));
        uint32_t j = 0;
        Frame g, last; bool got = false;
        while (parse_frame(j, n2, g)) if (g.frame == ref.frame) { last = g; got = true; }
        if (!got || !last.block) none++;
        else if (last.bflags & 1) {
            torn++;
            if (off < torn_lo) torn_lo = off;
            if (off > torn_hi) torn_hi = off;
        } else {
            whole++;
            if (last.w != ref.w) {
                mixed++;
                printf("   offset %d: a whole block differs from the reference\n", off);
            }
        }
        top->jtag_halt = 0;
    }
    printf("   offsets -40..320: %d whole (equal to the reference), %d torn (offsets %d..%d), "
           "%d without a block, %d mixed\n", whole, torn, torn_lo, torn_hi, none, mixed);
    CHECK(mixed == 0, "a block that is not flagged torn differs");
    CHECK(torn > 0 && whole > 0, "the sweep did not cross the copy");
    printf(fails ? "FAILED (%d)\n" : "PASS\n", fails);
    return fails ? 1 : 0;
}

// ---- M-d: the round trip --------------------------------------------------------------
static const uint8_t PROG_R[] = {
    0x78, 0xA9, 0x91, 0x8D, 0x14, 0x03, 0xA9, 0xC0, 0x8D, 0x15, 0x03,   // SEI, IRQ vector $C046
    0xA9, 0xFF, 0x8D, 0x06, 0xDD, 0x8D, 0x07, 0xDD, 0xA9, 0x11, 0x8D, 0x0F, 0xDD,  // CIA 2 timer B
    0x58,                                                               // CLI
    0xEE, 0x00, 0xC1, 0xD0, 0x03, 0xEE, 0x01, 0xC1,                     // $C019
    0xAD, 0x04, 0xDC, 0x8D, 0x02, 0xC1,
    0xAD, 0x06, 0xDD, 0x8D, 0x03, 0xC1,
    0xAD, 0x08, 0xDC, 0x8D, 0x04, 0xC1,
    0xAD, 0x12, 0xD0, 0x8D, 0x05, 0xC1, 0x8D, 0x20, 0xD0,
    0xAE, 0x00, 0xC1, 0x8A, 0x9D, 0x00, 0xC2,
    // $C043: CIA 2 registers the subset leaves to the bus replay: read back
    // every loop (PRB, DDRB, timer A, stopped: its latch), written about every
    // two frames (PRB, DDRB, timer A latch, ICR mask bits 3-4, TOD alarm)
    0xAD, 0x01, 0xDD, 0x8D, 0x08, 0xC1, 0xAD, 0x03, 0xDD, 0x8D, 0x09, 0xC1,
    0xAD, 0x04, 0xDD, 0x8D, 0x0A, 0xC1,
    0xAD, 0x00, 0xC1, 0xD0, 0x34, 0xAD, 0x01, 0xC1, 0x29, 0x03, 0xD0, 0x2D,
    0xAD, 0x01, 0xC1, 0x8D, 0x01, 0xDD, 0x8D, 0x03, 0xDD, 0x8D, 0x04, 0xDD, 0x8D, 0x05, 0xDD,
    0x29, 0x18, 0x09, 0x80, 0x8D, 0x0D, 0xDD, 0x49, 0x98, 0x8D, 0x0D, 0xDD,
    0xA9, 0x81, 0x8D, 0x0F, 0xDD, 0xAD, 0x01, 0xC1, 0x29, 0x0F, 0x8D, 0x08, 0xDD,
    0xA9, 0x01, 0x8D, 0x0F, 0xDD,
    0x4C, 0x19, 0xC0,                                                   // $C08E
    0xEE, 0x06, 0xC1, 0xAD, 0x0D, 0xDC, 0x4C, 0x81, 0xEA,             // $C091: the IRQ handler
};
struct RFrame { Frame f; std::vector<uint32_t> w0; };   // a frame and the stream 0 records before its marker
// parse every frame from stream 2 record i on; s0 records of frame k = [prev F4, this F4)
static std::vector<Frame> all_frames(uint32_t c2)
{
    std::vector<Frame> v;
    uint32_t i = 0;
    Frame f;
    while (parse_frame(i, c2, f)) v.push_back(f);
    return v;
}

static int restoretest(const char *state)
{
    const uint32_t LB0 = B0, LS0 = S0, LB1 = B1, LS1 = S1, LS2 = S2;
    restore(state);
    if (!getenv("TB_RING_IDLE")) {      // TB_RING_IDLE=1: no program, BASIC idles in its KERNAL wait loop
        for (unsigned i = 0; i < sizeof PROG_R; i++) poke(0xC000 + i, PROG_R[i]);
        type("SYS49152\r");
    }
    run_ms(60);
    s2_size = LS2;
    twr(0, 0x104, 1);
    twr(2, LOG_CTRL, 0x10F);
    const int N = 2, NR = 6;
    run_ticks(uint64_t(N + NR + 4) * FRAME);
    top->jtag_halt = 1;
    for (int n = 0; n < 3 * FRAME && !top->halted; n++) tick();
    run_ticks(20000);                               // the drain
    uint32_t c0 = treg(2, LOG_CNT(0)), c1 = treg(2, LOG_CNT(1)), c2 = treg(2, LOG_CNT(2));
    twr(2, LOG_CTRL, 0);
    twr(0, 0x104, 0);
    std::vector<Frame> ref = all_frames(c2);
    printf("8. reference: %zu frames, stream 0 %u, stream 1 %u records\n", ref.size(), c0, c1);
    if ((int)ref.size() < N + NR + 1) { CHECK(false, "too few frames"); return 1; }
    const Frame fN = ref[N];
    CHECK(fN.block && fN.bflags == 0 && (fN.flags & 3) == 3 && c1 == 0, "frame N block, no stream 1 records");
    std::vector<uint32_t> s0(c0);
    for (uint32_t i = 0; i < c0; i++) s0[i] = d32(LB0 + 4 * i);
    printf("   frame N = %u: snap line %u x %u, flags %02X, stream 0 at %u; PC %04X\n", fN.frame, fN.line, fN.x,
           fN.flags, fN.n0, fN.w[2] & 0xFFFF);

    // RAM: undo every write after N's boundary
    if (getenv("TB_RING_APPLY")) {      // by the engine's APPLY (undo, newest record first)
        twr(2, 0x094, (c0 - 1) & 0xFFFFFFF);
        twr(2, 0x098, c0 - fN.n0);
        twr(2, 0x09C, 1);
        uint64_t ta = ticks;
        while ((treg(2, 0x09C) & 1) && ticks - ta < 4000000) run_ticks(100);
        CHECK(!(treg(2, 0x09C) & 1), "APPLY did not finish");
    } else
        for (uint32_t i = c0; i-- > fN.n0;) poke(s0[i] & 0xFFFF, s0[i] >> 16);
    // VIC registers by replay (real bus cycles; the first one holds the CPU)
    auto vreg = [&](int r) { return uint8_t(fN.w[4 + r / 4] >> (8 * (r & 3))); };
    for (int r = 0; r <= 0x2E; r++) {
        if (r == 0x12 || r == 0x13 || r == 0x14 || r == 0x19 || r == 0x1E || r == 0x1F) continue;
        dma(uint16_t(0xD000 + r), true, vreg(r));
    }
    // the CIA registers the subset does not load: port latches, timer
    // latches, ICR mask, TOD alarm, by replay from N's block (its CIA words)
    auto bits = [&](int w0, int lo, int n) {
        uint32_t v = 0;
        for (int b = 0; b < n; b++) v |= ((fN.w[w0 + (lo + b) / 32] >> ((lo + b) % 32)) & 1u) << b;
        return v;
    };
    if (!getenv("TB_RING_NOREPLAY"))
        for (int c = 0; c < 2; c++) {
            int w0 = c ? 46 : 34;
            uint16_t io = c ? 0xDD00 : 0xDC00;
            dma(io + 2, true, bits(w0, 16, 8));     // DDRA
            dma(io + 3, true, bits(w0, 8, 8));      // DDRB
            dma(io + 0, true, bits(w0, 32, 8));     // PRA
            dma(io + 1, true, bits(w0, 24, 8));     // PRB
            uint32_t ta = bits(w0, 43, 16), tb = bits(w0, 83, 16);
            dma(io + 4, true, ta & 0xFF); dma(io + 5, true, ta >> 8);
            dma(io + 6, true, tb & 0xFF); dma(io + 7, true, tb >> 8);
            dma(io + 0xD, true, 0x7F);
            dma(io + 0xD, true, 0x80 | bits(w0, 311, 5));
            dma(io + 0xF, true, 0x80);              // CRB: alarm writes (CRB itself is loaded)
            dma(io + 0xB, true, bits(w0, 123, 8));
            dma(io + 0xA, true, bits(w0, 131, 8));
            dma(io + 0x9, true, bits(w0, 139, 8));
            dma(io + 0x8, true, bits(w0, 147, 8));
        }
    // hold the CPU, run to the stop position, stage, load
    top->app_stop = 1;                              // the CPU-only pause (GATE_CTRL b0 = 0)
    twr(0, 0x114, (1u << 8) | (fN.x >> 3));
    top->jtag_halt = 0;
    uint64_t t0 = ticks;
    while (!(treg(0, 0x114) & (1u << 16)) && ticks - t0 < 4 * FRAME) run_ticks(500);
    CHECK(treg(0, 0x114) & (1u << 16), "no stop at the position");
    CHECK(top->halted, "not halted at the position");
    if (getenv("TB_RING_DELAY")) run_ticks(strtoull(getenv("TB_RING_DELAY"), 0, 0));   // wall time while held
    const int W1 = 27, W3 = 34, W4 = 46;            // the block's CPU, CIA 1, CIA 2 words
    twr(1, 0x000, 1 << 7 | 8);  for (int k = 0; k < 7; k++)  twr(1, 0x004, fN.w[W1 + k]);
    twr(1, 0x000, 3 << 7 | 8);  for (int k = 0; k < 12; k++) twr(1, 0x004, fN.w[W3 + k]);
    twr(1, 0x000, 4 << 7 | 8);  for (int k = 0; k < 12; k++) twr(1, 0x004, fN.w[W4 + k]);
    CHECK((treg(1, 0x008) & 2) == 0, "staged writes refused");
    // TB_RING_NOLOAD=1: the same without the load (the negative check)
    twr(1, 0x00C, getenv("TB_RING_NOLOAD") ? 0 : 0x1A);
    top->app_stop = 0;
    run_ticks(10);
    twr(0, 0x104, 1);
    twr(2, LOG_CTRL, 0x10F | 0x100);
    twr(0, 0x114, 1u << 9);                         // go on: the load is the first edge
    run_ticks(uint64_t(NR + 1) * FRAME + 20000);
    top->jtag_halt = 1;
    for (int n = 0; n < 3 * FRAME && !top->halted; n++) tick();
    run_ticks(20000);
    uint32_t d0 = treg(2, LOG_CNT(0)), d2 = treg(2, LOG_CNT(2));
    std::vector<Frame> got = all_frames(d2);
    std::vector<uint32_t> t0s(d0);
    for (uint32_t i = 0; i < d0; i++) t0s[i] = d32(LB0 + 4 * i);
    printf("   restored: %zu frames, stream 0 %u records\n", got.size(), d0);
    int bad = 0;
    uint32_t base0 = fN.n0;
    for (int k = 0; k < NR && k < (int)got.size(); k++) {
        const Frame &a = ref[N + 1 + k], &b = got[k];
        const Frame &ap = ref[N + k];
        uint32_t ra = (a.cycles - ap.cycles) & 0xFFFFFF;
        uint32_t rb = k ? ((b.cycles - got[k - 1].cycles) & 0xFFFFFF) : 0;
        int wd = 0;
        for (int j = 1; j < NW; j++) if (a.w[j] != b.w[j]) { if (wd++ < 4) printf("   frame +%d word %d: %08X vs %08X\n", k + 1, j, a.w[j], b.w[j]); }
        uint32_t na = a.n0 - ap.n0, nb = b.n0 - (k ? got[k - 1].n0 : 0);
        int rd = 0;
        for (uint32_t i = 0; i < na && i < nb; i++) {
            uint32_t x = s0[ap.n0 + i], y = t0s[(k ? got[k - 1].n0 : 0) + i];
            if (x != y && rd++ < 3) printf("   frame +%d write %u: %08X vs %08X\n", k + 1, i, x, y);
        }
        bool ok = wd == 0 && rd == 0 && na == nb && a.x == b.x && a.line == b.line && a.flags == b.flags &&
                  (k == 0 || ra == rb) && b.bflags == 0;
        printf("   frame +%d: snap x %u/%u, writes %u/%u, %s%d words and %d writes differ%s\n", k + 1, a.x, b.x,
               na, nb, k ? "" : "", wd, rd, ok ? "" : "  <--");
        if (!ok) bad++;
    }
    (void)base0;
    CHECK((int)got.size() >= NR && bad == 0, "the restored run differs from the reference");

    // ---- 9. RESUME_PHASE: a stop ends that many clocks after whole frames ----
    top->jtag_halt = 0;
    run_ticks(2 * FRAME);
    for (uint32_t ph : {0u, 1000u, 123457u, 0u}) {
        twr(0, 0x110, ph);
        uint64_t t1 = ticks; uint32_t g1 = top->gclk_edges;
        top->jtag_halt = 1;
        for (int n = 0; n < 3 * FRAME && !top->halted; n++) tick();
        top->jtag_halt = 0;
        for (int n = 0; n < 4 * FRAME && top->halted; n++) tick();
        run_ticks(1000);
        uint64_t off = (ticks - t1) - (top->gclk_edges - g1);
        printf("9. RESUME_PHASE %u: the clock was off %llu clocks = %llu frames + %llu\n", ph,
               (unsigned long long)off, (unsigned long long)(off / FRAME), (unsigned long long)(off % FRAME));
        CHECK(off % FRAME == ph, "RESUME_PHASE %u", ph);
    }
    twr(0, 0x110, 0);
    printf(fails ? "FAILED (%d)\n" : "PASS\n", fails);
    return fails ? 1 : 0;
}

// ---- the trace former (T21 §4.11.1, tb_ring "trace") --------------------------------
// A loop of known instructions with the interrupts off, run with the log and the trace
// on; stream 3's records, read from the model's DDR2, must be the instruction sequence:
// pc, opcode and operand bytes, the registers before each instruction (a software
// model started from a record), the cycle step of each instruction (a VIC stall may
// add cycles, never remove them), the writes (count, and stream 0's records at
// write_start: address and new value).
static const uint8_t PROG_T[] = {
    0x78,                                   // C000 SEI
    0xA2, 0x00,                             // C001 LDX #$00
    0xA9, 0x40,                             // C003 LDA #$40     (loop)
    0x8D, 0x00, 0xC1,                       // C005 STA $C100
    0xE8,                                   // C008 INX
    0x9D, 0x10, 0xC1,                       // C009 STA $C110,X
    0xEE, 0x01, 0xC1,                       // C00C INC $C101
    0x4C, 0x03, 0xC0,                       // C00F JMP $C003
};
static int tracetest(const char *state)
{
    const uint32_t TB0 = B0, TS0 = S0, T3 = 0x6800000, CAP3 = 349525;
    restore(state);
    for (unsigned i = 0; i < sizeof PROG_T; i++) poke(0xC000 + i, PROG_T[i]);
    type("SYS49152\r");
    run_ms(60);
    twr(0, 0x104, 1);
    twr(2, LOG_CTRL, 0x111);                        // restart, trace, on
    run_ticks(uint64_t(5) * FRAME);
    top->jtag_halt = 1;
    for (int n = 0; n < 3 * FRAME && !top->halted; n++) tick();
    run_ticks(20000);                               // the drain
    uint32_t c0 = treg(2, LOG_CNT(0)), c3 = treg(2, LOG_CNT(3)), st = treg(2, LOG_STAT);
    uint32_t drop = treg(2, DROPPED);
    printf("trace: %u records, %u stream 0 records, LOG_STAT %02X, dropped %u\n", c3, c0, st, drop);
    CHECK(c3 > 1000 && c3 < CAP3, "record count %u", c3);
    CHECK(!(st & 0x20), "trace FIFO not empty");
    CHECK(drop == 0, "records dropped");
    twr(2, LOG_CTRL, 0);

    struct Rec { uint64_t cyc; uint32_t ws; uint16_t pc, wc; uint8_t a, x, y, s, p, op, b1, b2; };
    auto rd = [&](uint32_t n) {
        uint32_t b = T3 + 24 * (n % CAP3);
        uint64_t w0 = d64(b), w1 = d64(b + 8), w2 = d64(b + 16);
        Rec r;
        r.cyc = w0 & 0xFFFFFF; r.ws = uint32_t(w1) & 0xFFFFFFF; r.pc = uint16_t(w1 >> 32); r.wc = uint16_t(w1 >> 48);
        r.a = uint8_t(w2); r.x = uint8_t(w2 >> 8); r.y = uint8_t(w2 >> 16); r.s = uint8_t(w2 >> 24);
        r.p = uint8_t(w2 >> 32); r.op = uint8_t(w2 >> 40); r.b1 = uint8_t(w2 >> 48); r.b2 = uint8_t(w2 >> 56);
        return r;
    };
    auto len_of = [](uint8_t op) { return op == 0x78 || op == 0xE8 ? 1 : op == 0xA2 || op == 0xA9 ? 2 : 3; };
    auto cyc_of = [](uint8_t op) { return op == 0x78 || op == 0xE8 || op == 0xA2 || op == 0xA9 ? 2 : op == 0x8D ? 4 : op == 0x9D ? 5 : op == 0xEE ? 6 : 3; };
    auto wr_of = [](uint8_t op) { return op == 0x8D || op == 0x9D ? 1 : op == 0xEE ? 2 : 0; };
    auto nz = [](uint8_t p, uint8_t v) { return uint8_t((p & ~0x82) | (v & 0x80) | (v == 0 ? 2 : 0)); };

    const uint32_t first = 6;
    if (c3 < first + 4) { CHECK(false, "too few records"); return 1; }
    Rec r0 = rd(first);
    uint8_t a = r0.a, x = r0.x, p = r0.p, m101 = 0;
    bool m101_known = false;
    uint16_t pc = r0.pc;
    int bad = 0, checked = 0, wchecked = 0, stalled = 0, exact = 0;
    uint64_t wsum = 0, last_cyc = 0;
    for (uint32_t n = first; n + 2 < c3; n++) {
        Rec r = rd(n), nx = rd(n + 1);
        if (pc < 0xC000 || pc >= 0xC000 + sizeof PROG_T) { printf("   record %u: pc %04X outside the program\n", n, pc); bad++; break; }
        uint8_t op = PROG_T[pc - 0xC000];
        uint8_t o1 = len_of(op) > 1 ? PROG_T[pc - 0xC000 + 1] : 0, o2 = len_of(op) > 2 ? PROG_T[pc - 0xC000 + 2] : 0;
        bool ok = r.pc == pc && r.op == op && (len_of(op) < 2 || r.b1 == o1) && (len_of(op) < 3 || r.b2 == o2) &&
                  r.a == a && r.x == x && (r.p & 0xCF) == (p & 0xCF) && r.wc == wr_of(op);
        uint32_t dc = uint32_t((nx.cyc - r.cyc) & 0xFFFFFF);
        if (dc < uint32_t(cyc_of(op))) ok = false;
        else if (dc == uint32_t(cyc_of(op))) exact++;
        else stalled++;
        if (n > first && r.ws != uint32_t(wsum)) ok = false;       // the writes of the records before
        if (n == first) wsum = r.ws;
        // the writes in stream 0
        for (int k = 0; k < r.wc; k++) {
            uint32_t w = d32(TB0 + 4 * ((r.ws + k) % (TS0 / 4)));
            uint16_t wa = uint16_t(w), wn = uint8_t(w >> 24); uint8_t wo = uint8_t(w >> 16);
            uint16_t ea = op == 0x8D ? 0xC100 : op == 0x9D ? uint16_t(0xC110 + x) : 0xC101;
            uint8_t en = op == 0xEE ? (k == 0 ? wo : uint8_t(wo + 1)) : 0x40;
            if (op == 0xEE && k == 0 && wn != wo) ok = false;
            if (op == 0xEE && k == 1) { if (m101_known && wo != m101) ok = false; }
            if (wa != ea || wn != en) { printf("   record %u write %d: %04X<-%02X, expected %04X<-%02X\n", n, k, wa, wn, ea, en); ok = false; }
            if (op == 0xEE && k == 1) { m101 = wn; m101_known = true; }
            wchecked++;
        }
        wsum += r.wc;
        if (!ok) {
            if (bad < 5)
                printf("   record %u: pc %04X op %02X b %02X %02X A %02X X %02X P %02X wc %u dc %u; expected pc %04X op %02X A %02X X %02X P %02X wc %d dc %d\n",
                       n, r.pc, r.op, r.b1, r.b2, r.a, r.x, r.p, r.wc, dc, pc, op, a, x, p, wr_of(op), cyc_of(op));
            bad++;
        }
        checked++;
        // the model
        switch (op) {
        case 0x78: p |= 4; pc += 1; break;
        case 0xA2: x = o1; p = nz(p, x); pc += 2; break;
        case 0xA9: a = o1; p = nz(p, a); pc += 2; break;
        case 0x8D: case 0x9D: pc += 3; break;
        case 0xE8: x++; p = nz(p, x); pc += 1; break;
        case 0xEE: pc += 3; if (m101_known) p = nz(p, m101); break;
        case 0x4C: pc = uint16_t(o1 | o2 << 8); break;
        }
        if (op == 0xEE && !m101_known) { printf("   no INC value\n"); bad++; break; }
        last_cyc = r.cyc;
    }
    (void)last_cyc;
    printf("trace: %d records checked (%d writes), %d bad, %d exact cycle steps, %d with a stall\n", checked, wchecked, bad,
           exact, stalled);
    CHECK(bad == 0, "%d records differ from the program", bad);
    CHECK(checked > 1000, "only %d records checked", checked);
    CHECK(wsum - 0 <= c0, "write counts %llu beyond stream 0's %u", (unsigned long long)wsum, c0);
    printf(fails ? "FAILED (%d)\n" : "PASS\n", fails);
    return fails ? 1 : 0;
}

// ---- T20 scrub: the app's scrub step on an idle BASIC, engine APPLY against the walk ----------
// What tests/t20_apply.py does on the board: land on frame T (undo the last frame run, walk the
// ring from the base to T, put T's block in as put_back does), run one frame with the log on and
// stop on line 0 cycle 4, read the machine. Once with the RAM walk done by the engine (APPLY), once
// pokewise, each from different origins; the machines after the frame run must be equal (64 KB and
// the whole chip block).
struct Scrub {
    std::vector<Frame> ring;
    uint32_t c0 = 0;        // ring records (stream 0)
    uint32_t base = 0;      // the stream 0 index the RAM stands at (state of the landed frame)
    uint32_t head = 0;      // records written so far (frame runs append)
    uint32_t rs = 0;        // where the last frame run's records start
    bool held = false;      // the machine stands on a position hold (else jtag_halt)
};
static uint32_t rec0(uint32_t i) { return d32(B0 + 4 * i); }
static std::map<uint16_t, uint8_t> walk_pend;    // the CPU walk's marks: the last store per address wins
static void apply_range(bool engine, uint32_t from, uint32_t to, bool undo)    // records [from, to)
{
    if (to == from) return;
    if (engine) {
        twr(2, 0x094, undo ? (to - 1) & 0xFFFFFFF : from);
        twr(2, 0x098, to - from);
        twr(2, 0x09C, undo ? 1 : 0);
        uint64_t ta = ticks;
        while ((treg(2, 0x09C) & 1) && ticks - ta < 40000000) run_ticks(100);
        CHECK(!(treg(2, 0x09C) & 1), "APPLY did not finish: %u..%u %s", from, to, undo ? "undo" : "redo");
    } else if (undo) {
        for (uint32_t i = to; i-- > from;) { uint32_t r = rec0(i); walk_pend[r & 0xFFFF] = (r >> 16) & 0xFF; }
    } else {
        for (uint32_t i = from; i < to; i++) { uint32_t r = rec0(i); walk_pend[r & 0xFFFF] = (r >> 24) & 0xFF; }
    }
}
// the marked bytes, once each, as each_pg writes them. TB_SCRUB_WALKBUS=1: a write to $0000/$0001 is a bus
// cycle (ultimate_sys.vhd, T16 Q8: dma_bus <= c64_stop, even in C64_DMA_MEMONLY), as the app's walk makes it
static void walk_flush()
{
    for (auto &kv : walk_pend) {
        if (kv.first < 2 && getenv("TB_SCRUB_WALKBUS")) dma(kv.first, true, kv.second, true);
        else poke(kv.first, kv.second);
    }
    walk_pend.clear();
}
struct Shot { std::vector<uint8_t> ram; std::vector<uint32_t> blk; uint32_t pc; };
static Shot land(Scrub &S, int k, bool engine)
{
    const Frame &fT = S.ring[k];
    const uint32_t nT = fT.n0;
    // 1. the machine stands (jtag_halt or a position hold); VIC and CIA registers by bus cycles
    auto vreg = [&](int r) { return uint8_t(fT.w[4 + r / 4] >> (8 * (r & 3))); };
    for (int r = 0; r <= 0x2E; r++) {
        if (r == 0x12 || r == 0x13 || r == 0x14 || r == 0x19 || r == 0x1E || r == 0x1F) continue;
        dma(uint16_t(0xD000 + r), true, vreg(r));
    }
    auto bits = [&](int w0, int lo, int n) {
        uint32_t v = 0;
        for (int b = 0; b < n; b++) v |= ((fT.w[w0 + (lo + b) / 32] >> ((lo + b) % 32)) & 1u) << b;
        return v;
    };
    for (int c = 0; c < 2; c++) {
        int w0 = c ? 46 : 34;
        uint16_t io = c ? 0xDD00 : 0xDC00;
        dma(io + 2, true, bits(w0, 16, 8)); dma(io + 3, true, bits(w0, 8, 8));
        dma(io + 0, true, bits(w0, 32, 8)); dma(io + 1, true, bits(w0, 24, 8));
        uint32_t ta = bits(w0, 43, 16), tb = bits(w0, 83, 16);
        dma(io + 4, true, ta & 0xFF); dma(io + 5, true, ta >> 8);
        dma(io + 6, true, tb & 0xFF); dma(io + 7, true, tb >> 8);
        dma(io + 0xD, true, 0x7F); dma(io + 0xD, true, 0x80 | bits(w0, 311, 5));
        dma(io + 0xF, true, 0x80);
        dma(io + 0xB, true, bits(w0, 123, 8)); dma(io + 0xA, true, bits(w0, 131, 8));
        dma(io + 0x9, true, bits(w0, 139, 8)); dma(io + 0x8, true, bits(w0, 147, 8));
    }
    // 2. the CPU held, run to the snap position
    top->app_stop = 1;
    twr(0, 0x114, (1u << 8) | (S.held ? 1u << 9 : 0) | (fT.x >> 3));
    top->jtag_halt = 0;
    uint64_t t0 = ticks;
    while (!(treg(0, 0x114) & (1u << 16)) && ticks - t0 < 4 * FRAME) run_ticks(500);
    CHECK(treg(0, 0x114) & (1u << 16), "no stop at the position");
    if (!engine && getenv("TB_SCRUB_DELAY")) run_ticks(strtoull(getenv("TB_SCRUB_DELAY"), 0, 0));   // the CPU walk takes seconds
    // 3. RAM through the second port: the last frame run's records, then base -> T
    apply_range(engine, S.rs, S.head, true);       // the last frame run took back: the RAM stands at base again
    if (nT < S.base) apply_range(engine, nT, S.base, true);
    else if (nT > S.base) apply_range(engine, S.base, nT, false);
    S.base = nT;
    if (!engine) walk_flush();
    // 4. staged words, load
    const int W1 = 27, W3 = 34, W4 = 46;
    twr(1, 0x000, 1 << 7 | 8);  for (int k2 = 0; k2 < 7; k2++)  twr(1, 0x004, fT.w[W1 + k2]);
    twr(1, 0x000, 3 << 7 | 8);  for (int k2 = 0; k2 < 12; k2++) twr(1, 0x004, fT.w[W3 + k2]);
    twr(1, 0x000, 4 << 7 | 8);  for (int k2 = 0; k2 < 12; k2++) twr(1, 0x004, fT.w[W4 + k2]);
    twr(1, 0x00C, 0x1A);
    top->app_stop = 0;
    run_ticks(10);
    // 5. one frame with the log on (no captures), stop on line 0 cycle 4
    twr(0, 0x104, 0);
    S.rs = treg(2, LOG_CNT(0));
    twr(2, LOG_CTRL, 0x00F | 0x200);
    twr(0, 0x114, (1u << 8) | (1u << 9) | 4);
    t0 = ticks;
    while (!(treg(0, 0x114) & (1u << 16)) && ticks - t0 < 4 * FRAME) run_ticks(500);
    CHECK(treg(0, 0x114) & (1u << 16), "no stop at line 0 cycle 4");
    S.held = true;
    run_ticks(20000);                                   // the drain
    S.head = treg(2, LOG_CNT(0));
    Shot sh;
    sh.ram.resize(65536);
    for (int a = 0; a < 65536; a++) sh.ram[a] = peek(uint16_t(a));
    sh.blk = sb_dump();
    sh.pc = sh.blk.size() > 29 ? sh.blk[29] : 0;
    return sh;
}
static int scrubtest(const char *state)
{
    restore(state);
    run_ms(60);
    s2_size = S2;
    twr(0, 0x104, 1);
    twr(2, LOG_CTRL, 0x10F);
    run_ticks(uint64_t(16) * FRAME);
    top->jtag_halt = 1;
    for (int n = 0; n < 3 * FRAME && !top->halted; n++) tick();
    run_ticks(20000);
    Scrub S;
    S.c0 = treg(2, LOG_CNT(0));
    uint32_t c2 = treg(2, LOG_CNT(2));
    twr(2, LOG_CTRL, 0);
    twr(0, 0x104, 0);
    S.ring = all_frames(c2);
    S.base = S.head = S.rs = S.c0;
    printf("10. scrub: ring of %zu frames, %u stream 0 records\n", S.ring.size(), S.c0);
    if (S.ring.size() < 12) { CHECK(false, "too few frames"); return 1; }
    int ntarget = getenv("TB_SCRUB_PAIRS") ? atoi(getenv("TB_SCRUB_PAIRS")) : 4;
    const int T[] = {3, 5, 7, 4, 6, 8, 9, 2, 10, 11, 12, 13}, O[] = {9, 10, 4, 8, 2, 5, 3, 7, 3, 4, 5, 6};
    int bad = 0;
    for (int i = 0; i < ntarget && i < 12; i++) {
        Shot a = land(S, T[i], true);
        land(S, O[i], true);
        Shot b = land(S, T[i], false);
        int dr = 0, dw = 0;
        for (int x = 0; x < 65536; x++) if (a.ram[x] != b.ram[x]) { if (dr++ < 6) printf("   RAM $%04X engine %02X walk %02X\n", x, a.ram[x], b.ram[x]); }
        for (size_t x = 1; x < a.blk.size(); x++) if (a.blk[x] != b.blk[x]) { if (dw++ < 6) printf("   block word %zu engine %08X walk %08X\n", x, a.blk[x], b.blk[x]); }
        printf("   pair %d: target %d via %d: PC %04X/%04X, $01F2 %02X/%02X, %d RAM bytes, %d block words differ\n", i, T[i], O[i], a.pc & 0xFFFF, b.pc & 0xFFFF, a.ram[0x1F2], b.ram[0x1F2], dr, dw);
        bad += dr + dw != 0;
    }
    CHECK(bad == 0, "%d pairs differ", bad);
    printf(fails ? "FAILED (%d)\n" : "PASS\n", fails);
    return fails ? 1 : 0;
}

int main(int argc, char **argv)
{
    Verilated::commandArgs(argc, argv);
    top = new Vtb_ring_top;
    t_now = 0; t_sys = HS; t_ui = HU;
    int rc = 3;
    if (argc >= 3 && !strcmp(argv[1], "boot")) rc = boot(argv[2]);
    else if (argc >= 3 && !strcmp(argv[1], "run")) rc = run(argv[2]);
    else if (argc >= 3 && !strcmp(argv[1], "bootef")) rc = bootef(argv[2]);
    else if (argc >= 3 && !strcmp(argv[1], "log")) rc = logtest(argv[2]);
    else if (argc >= 3 && !strcmp(argv[1], "snap")) rc = snaptest(argv[2]);
    else if (argc >= 3 && !strcmp(argv[1], "restore")) rc = restoretest(argv[2]);
    else if (argc >= 3 && !strcmp(argv[1], "trace")) rc = tracetest(argv[2]);
    else if (argc >= 3 && !strcmp(argv[1], "scrub")) rc = scrubtest(argv[2]);
    else fprintf(stderr, "usage: %s boot|run|bootef|log|snap|restore|trace|scrub <state>\n", argv[0]);
    top->final();
    delete top;
    return rc;
}
