// SPDX-License-Identifier: GPL-3.0-or-later
// The machine pause (T12 B) under Verilator, on the T05 bench (sim/drive:
// c64_core, drive A, serial bus, DDR2 model).
//
//   tb_pause boot <state> <rom>   power both up as the app does (no disk), run to
//                                 READY, check the drive's DOS came up, save
//   tb_pause run <state>          restore, start a test loop at $C000, then:
//     1. reference: run R C64 clocks, pause, note RAM, raster and clocks
//     2. same start, pause P frames in the middle, resume, run to R clocks
//        and pause: nothing (C64 clocks, raster, the loop's counter, the
//        drive's CPU cycles) moved while paused, the resume came after whole
//        frames, and RAM, raster and clock count equal the reference
//     3. DMA while paused: the same I/O reads before and after P frames move
//        only by the cycles the reads took (CIA 1 timer A, TOD, SID voice 3,
//        VIC raster); each read after the first costs one C64 cycle; bus and
//        RAM-port writes land; the machine goes on after the resume
//     4. (inside 2 and 3) the state bus while paused: the 6510, VIC and CIA
//        words do not move over P frames and cost no C64 clock; after bus DMA
//        cycles they follow the machine (CIA 1 timer A 5 below what the
//        first of five DMA reads saw)
//
// The test loop at $C000 blanks the screen (no bad lines: every bus DMA gets
// its cycle at once), sets SID voice 3 to sawtooth at $FFFF, starts the TOD,
// then counts in $C100/$C101 and copies $DC04, $D41B, $DC08 and $D012 to
// $C102-$C105, with the KERNAL's IRQ running. Exit code 0 = ok.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "Vtb_drive_top.h"
#include "Vtb_drive_top___024root.h"
#include "verilated.h"
#include "verilated_save.h"

static Vtb_drive_top *top;
static uint64_t now_ps, next_clk, next_sys, next_ui;
static uint64_t sys_cycles, clk_edges;
static int fails;
static const uint64_t CLK_HALF = 15859;     // 31.5278 MHz
static const uint64_t SYS_HALF = 5000;      // 100 MHz
static const uint64_t UI_HALF = 6667;       // 75 MHz
static const uint32_t FRAME = 628992;       // dot4x clocks per PAL frame

#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

// ---- the MIG model's memory (16 MB, 128-bit words) ----------------------------
#define MEM top->rootp->tb_drive_top__DOT__mig__DOT__mem
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

static const uint8_t LOOP[] = {
    0xA9, 0x0B, 0x8D, 0x11, 0xD0,       // LDA #$0B, STA $D011: screen off
    0xA9, 0xFF, 0x8D, 0x0E, 0xD4,       // voice 3 frequency $FFFF
    0x8D, 0x0F, 0xD4,
    0xA9, 0x21, 0x8D, 0x12, 0xD4,       // sawtooth, gate
    0xA9, 0x01, 0x8D, 0x0B, 0xDC,       // TOD 01:00:00.0, runs from the tenths write
    0xA9, 0x00, 0x8D, 0x0A, 0xDC,
    0x8D, 0x09, 0xDC, 0x8D, 0x08, 0xDC,
    0xEE, 0x00, 0xC1,                   // loop ($C022): INC $C100
    0xD0, 0x03, 0xEE, 0x01, 0xC1,       // BNE +3, INC $C101
    0xAD, 0x04, 0xDC, 0x8D, 0x02, 0xC1, // $DC04 -> $C102
    0xAD, 0x1B, 0xD4, 0x8D, 0x03, 0xC1, // $D41B -> $C103
    0xAD, 0x08, 0xDC, 0x8D, 0x04, 0xC1, // $DC08 -> $C104
    0xAD, 0x12, 0xD0, 0x8D, 0x05, 0xC1, // $D012 -> $C105
    0x4C, 0x22, 0xC0,                   // JMP loop
};

struct Io { uint16_t timer; uint8_t osc3, tenths, d012; };
static Io io_dma(uint32_t *edges)
{
    Io v;
    uint32_t e[5];
    v.timer = dma(0xDC04, false, 0, &e[0]);
    v.timer |= dma(0xDC05, false, 0, &e[1]) << 8;
    v.osc3 = dma(0xD41B, false, 0, &e[2]);
    v.tenths = dma(0xDC08, false, 0, &e[3]);
    v.d012 = dma(0xD012, false, 0, &e[4]);
    for (int i = 0; i < 5; i++) edges[i] = e[i];
    return v;
}

static int run(const char *state)
{
    restore(state);
    for (unsigned i = 0; i < sizeof LOOP; i++) poke(0xC000 + i, LOOP[i]);
    const char *sys = "SYS49152\r";
    for (int i = 0; sys[i]; i++) poke(0x0277 + i, sys[i]);
    poke(0x00C6, strlen(sys));
    run_us(100000);
    uint16_t c0 = counter();
    run_us(20000);
    printf("loop counter %04X -> %04X, TOD tenths %X\n", c0, counter(), peek(0xC104));
    CHECK(counter() != c0, "test loop does not run");
    const char *mid = "pause_mid.state";
    save(mid);
    uint32_t g0 = top->gclk_edges;
    const uint32_t R = 8 * FRAME, P = 7;   // P frames: 140 ms, a running TOD would move

    // 1. reference
    run_clks(R);
    halt(true);
    uint32_t ref_edges = top->gclk_edges - g0, ref_raster = raster();
    std::vector<uint8_t> ref = ram();
    printf("1. reference: paused after %u C64 clocks, raster %u/%u, counter %04X\n",
           ref_edges, ref_raster >> 10, ref_raster & 0x3FF, ref[0xC100] | ref[0xC101] << 8);

    // 2. a pause in the middle
    restore(mid);
    uint32_t sk0 = skew();
    run_clks(2 * FRAME + 12345);
    halt(true);
    run_us(5);                          // the drive's synchronizer and its last tick
    uint32_t g1 = top->gclk_edges, r1 = raster(), d1 = drive_cycles();
    uint16_t k1 = counter();
    std::vector<uint32_t> sb1 = sb_all();
    for (unsigned f = 0; f < P; f++) run_clks(FRAME);
    std::vector<uint32_t> sb2 = sb_all();
    printf("4. state bus while paused: PC %04X A %02X, CIA 1 timer A %04X; %s after %u frames, C64 clocks +%u\n",
           sb1[1] & 0xFFFF, sb1[0] & 0xFF, sb1[3 + 12 + 1] & 0xFFFF, sb1 == sb2 ? "the same" : "MOVED", P,
           top->gclk_edges - g1);
    CHECK(sb1 == sb2, "state bus words moved while paused");
    printf("2. paused %u frames at raster %u/%u: C64 clocks +%u, raster %u/%u, counter %04X -> %04X,"
           " drive cycles %u -> %u\n", P, r1 >> 10, r1 & 0x3FF, top->gclk_edges - g1, raster() >> 10,
           raster() & 0x3FF, k1, counter(), d1, drive_cycles());
    CHECK(top->gclk_edges == g1, "C64 clock ran while paused");
    CHECK(raster() == r1, "raster moved while paused");
    CHECK(counter() == k1, "CPU ran while paused");
    CHECK(drive_cycles() == d1, "drive CPU ran while paused");
    halt(false);
    printf("   resumed: clocks left out modulo a frame %u (before %u)\n", skew(), sk0);
    CHECK(skew() == sk0, "resume not after whole frames");
    run_us(1000);
    CHECK(drive_cycles() != d1, "drive did not go on");
    uint32_t left = R - (top->gclk_edges - g0);
    run_clks(left);
    while (top->gclk_edges - g0 < R) clk_tick();
    halt(true);
    std::vector<uint8_t> got = ram();
    int diff = 0;
    for (int a = 0; a < 65536; a++)
        if (got[a] != ref[a] && diff++ < 8) printf("   $%04X: %02X, reference %02X\n", a, got[a], ref[a]);
    printf("   paused again after %u C64 clocks, raster %u/%u, counter %04X, %d RAM bytes differ\n",
           top->gclk_edges - g0, raster() >> 10, raster() & 0x3FF, got[0xC100] | got[0xC101] << 8, diff);
    CHECK(top->gclk_edges - g0 == ref_edges, "not the reference's clock count");
    CHECK(raster() == ref_raster, "not the reference's raster position");
    CHECK(diff == 0, "RAM differs from the reference");

    // 3. DMA while paused
    restore(mid);
    run_clks(FRAME + 777);
    halt(true);
    uint32_t e0, e[5];
    uint16_t kd = counter();
    dma(0xC100, false, 0, &e0);         // the first one also brings the CPU to a read
    Io a = io_dma(e);
    printf("3. first DMA %u clocks, then %u %u %u %u %u; timer %04X osc3 %02X tenths %X raster %u\n",
           e0, e[0], e[1], e[2], e[3], e[4], a.timer, a.osc3, a.tenths, a.d012);
    for (int i = 0; i < 5; i++) CHECK(e[i] == 32, "DMA read %d cost %u clocks, not one cycle", i, e[i]);
    uint32_t g2 = top->gclk_edges;
    for (unsigned f = 0; f < P; f++) run_clks(FRAME);
    CHECK(top->gclk_edges == g2, "C64 clock ran while paused between DMAs");
    Io b = io_dma(e);
    uint16_t sb_ta = sb(3, 1) & 0xFFFF;
    // $DC04 was the first of the five reads: the timer has counted 5 cycles since
    printf("   state bus after the five DMA reads: CIA 1 timer A %04X ($DC04 read %04X)\n", sb_ta, b.timer);
    CHECK((uint16_t)(b.timer - sb_ta) == 5, "state bus does not follow the DMA cycles");
    printf("   after %u frames: timer %04X osc3 %02X tenths %X raster %u\n", P, b.timer, b.osc3, b.tenths, b.d012);
    // five reads, one cycle each, between a value and the same value again
    CHECK((uint16_t)(a.timer - b.timer) == 5, "CIA 1 timer A moved by %u, not 5", (uint16_t)(a.timer - b.timer));
    CHECK(b.tenths == a.tenths, "TOD moved");
    CHECK((uint8_t)(b.osc3 - a.osc3) >= 4 && (uint8_t)(b.osc3 - a.osc3) <= 6, "SID voice 3 moved by %u",
          (uint8_t)(b.osc3 - a.osc3));
    CHECK(b.d012 == a.d012 || b.d012 == (uint8_t)(a.d012 + 1), "raster line moved from %u to %u", a.d012, b.d012);
    uint32_t ew;
    for (int i = 0; i < 8; i++) dma(0xC200 + i, true, 0xA0 + i, &ew);
    CHECK(ew == 32, "DMA write cost %u clocks", ew);
    dma(0xD020, true, 0x02);
    uint8_t border = dma(0xD020, false, 0);
    poke(0xC300, 0x5A);                 // the RAM port, no C64 cycle
    uint32_t g3 = top->gclk_edges;
    bool ok = true;
    for (int i = 0; i < 8; i++) ok &= peek(0xC200 + i) == 0xA0 + i;
    printf("   $D020 reads %02X; $C200-$C207 %s; $C300 %02X; counter %04X -> %04X\n", border,
           ok ? "written" : "wrong", peek(0xC300), kd, counter());
    CHECK((border & 0x0F) == 2, "border write did not land");
    CHECK(ok, "bus DMA writes did not land");
    CHECK(peek(0xC300) == 0x5A, "RAM port write did not land");
    CHECK(top->gclk_edges == g3, "RAM port access cost C64 clocks");
    CHECK(counter() - kd <= 1, "CPU ran during the DMAs");
    halt(false);
    CHECK(skew() == sk0, "resume after DMAs not after whole frames");
    run_us(20000);
    printf("   resumed: counter %04X, $C200 %02X, $C300 %02X, raster %u/%u\n", counter(), peek(0xC200),
           peek(0xC300), raster() >> 10, raster() & 0x3FF);
    CHECK(counter() - kd > 100, "CPU did not go on");
    CHECK(peek(0xC200) == 0xA0 && peek(0xC300) == 0x5A, "DMA bytes lost after the resume");

    printf("%s, %d failures\n", fails ? "PAUSE FAILED" : "pause ok", fails);
    return fails ? 1 : 0;
}

int main(int argc, char **argv)
{
    Verilated::commandArgs(argc, argv);
    top = new Vtb_drive_top;
    if (argc >= 4 && !strcmp(argv[1], "boot")) return boot(argv[2], argv[3]);
    if (argc >= 3 && !strcmp(argv[1], "run")) return run(argv[2]);
    printf("usage: tb_pause boot <state> <rom> | run <state>\n");
    return 2;
}
