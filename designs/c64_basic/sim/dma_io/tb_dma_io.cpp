// SPDX-License-Identifier: GPL-3.0-or-later
// c64_core under Verilator: the Ultimate app's DMA as C64 bus cycles (T12).
//
//   tb_dma_io boot <state>      cold start to READY, save the model
//   tb_dma_io freeze <state>    restore, then the app's freeze sequence over
//                               the DMA port (c64.cc stop/backup_io/init_io,
//                               the menu on the C64 screen, restore_io/resume)
//
// Checks: the PLA view of a DMA cycle (ROMs on read, RAM under them on write,
// Ultimax), the register values read back after init_io, the VIC's picture
// of the written screen (frame grabbed from the pixel stream, also saved as
// build/run/freeze.ppm), everything back after restore_io, and BASIC running
// again (PRINT 6*7 typed through the keyboard buffer); the ROM block's and
// the colour RAM's second ports equal bus reads. Exit code 0 = ok.
//
//   build.sh && cd build/run && ../tb_dma_io boot ../tb.state && ../tb_dma_io freeze ../tb.state
// With build.sh --trace (tb_dma_io_trace, its own state) and TRACE_RESTORE set,
// the start of restore_io goes to trace.vcd.
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
static VerilatedVcdC *tfp;
#endif
static int tracing;

static Vc64_core *top;
static uint64_t clocks;
static int fails;

// the last frame from the pixel stream, raster line x raster x
enum { FW = 512, FH = 320 };
static uint8_t frame[FH][FW];
static bool grab;

static void tick()
{
    top->clk = 1; top->gclk = 1; top->snd_clk = 1; top->cart_wclk = 1;
    top->eval();
#ifdef TRACE
    if (tracing) tfp->dump(clocks * 2);
#endif
    top->clk = 0; top->gclk = 0; top->snd_clk = 0; top->cart_wclk = 0;
    top->eval();
#ifdef TRACE
    if (tracing) { tfp->dump(clocks * 2 + 1); tracing--; }
#endif
    if (grab && top->pix_y < FH && top->pix_x < FW) frame[top->pix_y][top->pix_x] = top->pix_color;
    clocks++;
}

static void run_cycles(uint64_t n) { for (uint64_t i = 0; i < n * 32; i++) tick(); }

#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

// c64_mem's debug port: the RAM itself, no bus cycle; with dbg_rom the ROM
// block (byte offset: char 0x0000, KERNAL 0x2000, BASIC 0x4000)
static uint8_t peek(uint16_t a) { top->dbg_addr = a; tick(); tick(); return top->dbg_rdata; }
static uint8_t peek_rom(uint16_t a)
{
    top->dbg_rom = 1; top->dbg_addr = a; tick();
    uint8_t v = top->dbg_rdata;                 // the lane select holds for this clock
    top->dbg_rom = 0; tick();
    return v;
}
// the colour RAM's second port (c64_core cram_*): low nibble
static uint8_t peek_color(uint16_t i) { top->cram_addr = i; tick(); tick(); return top->cram_rdata; }
static void poke(uint16_t a, uint8_t v)
{
    top->dbg_addr = a; top->dbg_wdata = v; top->dbg_we = 1;
    tick();
    top->dbg_we = 0;
}

// the app's DMA window: one C64 bus cycle
static uint64_t dma_count, dma_clocks;
static uint8_t dma(uint16_t a, bool we, uint8_t v)
{
    top->dma_addr = a; top->dma_we = we; top->dma_wdata = v; top->dma_req = 1;
    tick();
    top->dma_req = 0;
    for (int n = 0; n < 32 * 200000; n++) {        // a bad line holds BA low for 40+ cycles
        tick();
        if (top->dma_done) {
            if (n > 32 * 200) printf("DMA %04X took %d cycles (line %d)\n", a, n / 32, top->pix_y);
            dma_count++; dma_clocks += n + 2;
            return top->dma_rdata;
        }
    }
    printf("FAIL: DMA %04X timeout\n", a);
    exit(4);
}
static uint8_t rd(uint16_t a) { return dma(a, false, 0); }
static void wr(uint16_t a, uint8_t v) { dma(a, true, v); }

static std::string row(int r, bool viadma = false)
{
    std::string s;
    for (int i = 0; i < 40; i++) {
        uint8_t c = (viadma ? rd(0x0400 + r * 40 + i) : peek(0x0400 + r * 40 + i)) & 0x7F;
        if (c == 0) s += '@';
        else if (c < 27) s += char('A' + c - 1);
        else if (c < 64) s += char(c);
        else s += '.';
    }
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

static void show(int from = 0)
{
    for (int r = from; r < 25; r++) {
        std::string s = row(r);
        if (!s.empty()) printf("  | %s\n", s.c_str());
    }
}

static void init()
{
    top->rst = 1; top->snd_rst = 1; top->audio_sample_en = 0;
    top->keys = 0; top->joy1 = 0; top->joy2 = 0; top->restore = 0;
    top->dbg_addr = 0; top->dbg_we = 0; top->dbg_wdata = 0;
    top->reu_en = 1; top->reu_size = 5; top->reu_cmd_ready = 1; top->reu_rdata = 0; top->reu_rdata_valid = 0;
    top->cpu_reset_req = 0; top->cpu_pause_req = 0;
    top->cart_type = 0; top->cart_kill_tgl = 0; top->cart_force_tgl = 0;
    top->cart_ack_tgl = 0; top->cart_fill_be = 0;
    top->dma_req = 0; top->dma_addr = 0; top->dma_we = 0; top->dma_wdata = 0; top->ultimax = 0;
    top->dbg_rom = 0; top->sb_addr = 0; top->cap_every = 0;
    top->cram_addr = 0; top->cram_we = 0; top->cram_wdata = 0;
    for (int i = 0; i < 64; i++) tick();
    top->rst = 0; top->snd_rst = 0;
}

static int boot(const char *state)
{
    init();
    for (int n = 0; n < 400; n++) {
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
    show();
    return 3;
}

// the menu's charset and screen: a pattern per character and row
static uint8_t glyph(int c, int r) { return uint8_t(c * 0x1D + r * 0x47 + 0x11); }
static uint8_t scr_code(int i) { return uint8_t(i * 7 + i / 40); }
static uint8_t scr_col(int i) { return uint8_t(1 + i % 15); }

// grab one whole frame from the pixel stream
static void grab_frame()
{
    while (top->pix_y != 0) tick();
    while (top->pix_y == 0) tick();
    grab = true;
    while (top->pix_y != 0) tick();
    grab = false;
}

static void save_ppm(const char *name)
{
    static const uint8_t pal[16][3] = {
        {0,0,0},{255,255,255},{136,57,50},{103,182,189},{139,63,150},{85,160,73},{64,49,141},{191,206,114},
        {139,84,41},{87,66,0},{184,105,98},{80,80,80},{120,120,120},{148,224,137},{120,105,196},{159,159,159}};
    FILE *f = fopen(name, "wb");
    if (!f) return;
    fprintf(f, "P6 %d %d 255\n", FW, FH);
    for (int y = 0; y < FH; y++)
        for (int x = 0; x < FW; x++) fwrite(pal[frame[y][x] & 15], 1, 3, f);
    fclose(f);
}

// where does the 40x25 text area start in the frame? (first char row only)
static int match_text(int y0, int x0, int rows)
{
    int ok = 0;
    for (int y = 0; y < rows * 8; y++)
        for (int x = 0; x < 320; x++) {
            int i = (y / 8) * 40 + x / 8;
            bool fg = glyph(scr_code(i), y & 7) & (0x80 >> (x & 7));
            int want = fg ? scr_col(i) : 0;          // background black (init: $D021 = 0)
            if (frame[y0 + y][x0 + x] == want) ok++;
        }
    return ok;
}

static int freeze(const char *state)
{
    VerilatedRestore is;
    is.open(state);
    is >> clocks >> *top;
    run_cycles(1000);

    // what the C64 had before: RAM through the debug port
    std::vector<uint8_t> ram0(65536);
    for (int a = 0; a < 65536; a++) ram0[a] = peek(uint16_t(a));
    uint8_t jiffy0 = ram0[0xA2];

    // C64::stop: C64_STOP, the CPU halts at its next read cycle
    top->cpu_pause_req = 1;
    run_cycles(10);

    // ---- the PLA's view of a DMA cycle, $01 = $37 (BASIC, KERNAL, I/O) ----
    CHECK(rd(0x0001) == 0x37, "$01 = %02X", rd(0x0001));
    CHECK(rd(0xA000) == 0x94 && rd(0xA001) == 0xE3, "BASIC ROM at $A000: %02X %02X", rd(0xA000), rd(0xA001));
    CHECK(rd(0xFFFC) == 0xE2 && rd(0xFFFD) == 0xFC, "KERNAL reset vector %02X%02X", rd(0xFFFD), rd(0xFFFC));
    wr(0xA000, 0x5A);
    CHECK(peek(0xA000) == 0x5A && rd(0xA000) == 0x94, "write under BASIC: RAM %02X, read %02X", peek(0xA000), rd(0xA000));
    poke(0xA000, ram0[0xA000]);
    CHECK(rd(0xC000) == ram0[0xC000], "RAM at $C000");
    // the ROM block's second port (the freeze responder's ROM reads) against bus reads
    int rbad = 0;
    for (int i = 0; i < 64; i++) {
        uint16_t b = 0xA000 + (i * 131 & 0x1FFF), k = 0xE000 + (i * 127 & 0x1FFF);
        uint8_t pb = peek_rom(0x4000 + (b & 0x1FFF)), db = rd(b);
        uint8_t pk = peek_rom(0x2000 + (k & 0x1FFF)), dk = rd(k);
        if (pb != db && rbad++ < 4) printf("   $%04X: port %02X, bus %02X\n", b, pb, db);
        if (pk != dk && rbad++ < 4) printf("   $%04X: port %02X, bus %02X\n", k, pk, dk);
    }
    CHECK(rbad == 0, "%d ROM bytes on the second port differ from bus reads", rbad);
    // $D000-$DFFF is I/O: $D020 through the VIC, not the RAM under it
    CHECK((rd(0xD020) & 0x0F) == 14 && (rd(0xD020) & 0xF0) == 0xF0, "$D020 = %02X", rd(0xD020));

    // ---- C64::stop (do_raster) then goUltimax and the VIC registers ----
    top->ultimax = 1;
    run_cycles(2);
    uint8_t raster = rd(0xD012), raster_hi = rd(0xD011), vic_irq_en = rd(0xD01A), vic_irq = rd(0xD019);
    printf("stopped: raster %02X %02X, VIC irq en %02X irq %02X\n", raster, raster_hi, vic_irq_en, vic_irq);
    // Ultimax: $1000-$7FFF and $A000-$CFFF are not mapped (writes go nowhere)
    wr(0x1000, uint8_t(~ram0[0x1000]));
    CHECK(peek(0x1000) == ram0[0x1000], "Ultimax write reached $1000");
    CHECK(rd(0x0400) == ram0[0x0400], "Ultimax: RAM at $0400");

    // ---- backup_io ----
    uint8_t vic_backup[49];
    for (int i = 0; i < 49; i++) vic_backup[i] = rd(0xD000 + i);
    wr(0xD011, 0); wr(0xD020, 0); wr(0xD021, 0);
    wr(0xD418, 0);                                   // SID_VOLUME (Gideon's mute)
    for (int i = 0; i < 13; i++) if (i != 8 && i != 11) rd(0xDC00 + i);
    std::vector<uint8_t> ram_backup(2048), color_backup(1024), screen_backup(1024);
    for (int i = 0; i < 2048; i++) ram_backup[i] = rd(0x0800 + i);
    for (int i = 0; i < 1024; i++) color_backup[i] = rd(0xD800 + i) & 0x0F;
    for (int i = 0; i < 1024; i++) screen_backup[i] = rd(0x0400 + i);
    for (int i = 0; i < 2048; i++) wr(0x0800 + i, glyph(i / 8, i & 7));
    uint8_t cia[8];
    cia[0] = rd(0xDD02); cia[1] = rd(0xDD00);
    cia[2] = rd(0xDC02); cia[3] = rd(0xDC03);
    wr(0xDC02, 0x00); wr(0xDC03, 0xFF); cia[5] = rd(0xDC01);
    wr(0xDC03, 0x00); wr(0xDC02, 0xFF); cia[4] = rd(0xDC00);
    cia[6] = rd(0xDC0E); cia[7] = rd(0xDC0F);
    printf("CIA backup %02X %02X %02X %02X %02X %02X %02X %02X\n",
           cia[0], cia[1], cia[2], cia[3], cia[4], cia[5], cia[6], cia[7]);

    // the backups are the C64's own values
    bool same = true;
    for (int i = 0; i < 2048; i++) same &= ram_backup[i] == ram0[0x0800 + i];
    for (int i = 0; i < 1024; i++) same &= screen_backup[i] == ram0[0x0400 + i];
    CHECK(same, "RAM backup over DMA differs from the RAM");
    CHECK(cia[2] == 0xFF && cia[3] == 0x00, "CIA1 DDR %02X %02X", cia[2], cia[3]);
    CHECK((cia[0] & 0x3F) == 0x3F, "CIA2 DDRA %02X", cia[0]);
    CHECK(cia[6] & 0x01, "CIA1 timer A not running (CRA %02X)", cia[6]);

    // ---- init_io ----
    wr(0xDD02, rd(0xDD02) & 0xFC);
    wr(0xDC03, 0x00); wr(0xDC02, 0xFF);
    wr(0xDC0E, rd(0xDC0E) & 0xFD); wr(0xDC0F, rd(0xDC0F) & 0xFD);
    wr(0xD018, 0x12);
    wr(0xD011, 0x1B); wr(0xD012, 0xF8); wr(0xD015, 0x00); wr(0xD016, 0xC8); wr(0xD020, 0x00);

    // ---- the menu: screen and colour RAM ----
    for (int i = 0; i < 1000; i++) { wr(0x0400 + i, scr_code(i)); wr(0xD800 + i, scr_col(i)); }
    printf("%llu DMA cycles, %.1f C64 cycles each\n", (unsigned long long)dma_count,
           double(dma_clocks) / dma_count / 32);

    CHECK(rd(0xD018) == 0x13, "$D018 = %02X", rd(0xD018));
    CHECK((rd(0xD011) & 0x7F) == 0x1B, "$D011 = %02X", rd(0xD011));
    CHECK(rd(0xD016) == 0xC8, "$D016 = %02X", rd(0xD016));
    CHECK(rd(0xD020) == 0xF0 && rd(0xD021) == 0xF0, "$D020/1 = %02X %02X", rd(0xD020), rd(0xD021));
    CHECK(rd(0xDC02) == 0xFF && rd(0xDC03) == 0x00, "CIA1 DDR %02X %02X", rd(0xDC02), rd(0xDC03));
    CHECK((rd(0xDD02) & 0x03) == 0, "CIA2 DDRA %02X", rd(0xDD02));
    CHECK((rd(0xDC0E) & 0x02) == 0 && (rd(0xDC0F) & 0x02) == 0, "CRA/CRB PB on");
    int cbad = 0, sbad = 0;
    for (int i = 0; i < 1000; i++) {
        if ((rd(0xD800 + i) & 0x0F) != scr_col(i)) cbad++;
        if (peek(0x0400 + i) != scr_code(i)) sbad++;
    }
    CHECK(cbad == 0, "%d colour RAM bytes read back wrong", cbad);
    int pbad = 0;
    for (int i = 0; i < 1024; i++) if (peek_color(i) != (rd(0xD800 + i) & 0x0F)) pbad++;
    CHECK(pbad == 0, "%d colour RAM bytes on the second port differ from bus reads", pbad);
    CHECK(sbad == 0, "%d screen bytes wrong in RAM", sbad);
    // the keyboard scan the menu does: column 0 low, no key
    wr(0xDC00, 0xFE);
    CHECK(rd(0xDC01) == 0xFF, "keyboard row %02X", rd(0xDC01));

    // ---- what the VIC shows ----
    run_cycles(63 * 312);
    grab_frame();
    save_ppm("freeze.ppm");
    int best = 0, by = 0, bx = 0;
    for (int y0 = 30; y0 < 80; y0++)
        for (int x0 = 0; x0 + 320 <= FW; x0++) {
            int m = match_text(y0, x0, 1);
            if (m > best) { best = m; by = y0; bx = x0; }
        }
    int all = match_text(by, bx, 25);
    printf("text area at line %d, x %d: %d of %d pixels as written\n", by, bx, all, 320 * 200);
    CHECK(all == 320 * 200, "the VIC does not show the written screen");

    // ---- restore_io ----
    wr(0xD011, 0);
    for (int i = 0; i < 2048; i++) wr(0x0800 + i, ram_backup[i]);
    if (getenv("TRACE_RESTORE")) tracing = 32 * 400;
    for (int i = 0; i < 1024; i++) wr(0xD800 + i, color_backup[i]);
    for (int i = 0; i < 1024; i++) wr(0x0400 + i, screen_backup[i]);
    wr(0xDD02, cia[0]);
    wr(0xDC02, cia[2]); wr(0xDC03, cia[3]); wr(0xDC00, cia[4]); wr(0xDC01, cia[5]);
    wr(0xDC0E, cia[6]); wr(0xDC0F, cia[7]);
    for (int i = 0; i < 49; i++) wr(0xD000 + i, vic_backup[i]);
    wr(0xD418, 15); wr(0xD41F, 0);

    // ---- resume (stop mode 1, no raster IRQ) ----
    wr(0xD012, raster); wr(0xD011, raster_hi & 0x7F);
    wr(0xD019, 0x81);
    wr(0xD01A, vic_irq_en);
    top->ultimax = 0;
    run_cycles(2);
    top->cpu_pause_req = 0;

    run_cycles(10000);
    same = true;
    for (int a = 0x0400; a < 0x1000; a++) same &= peek(uint16_t(a)) == ram0[a];
    CHECK(same, "RAM $0400-$0FFF not restored");
    // peek() stops the C64 for its DMA cycles too
    top->cpu_pause_req = 1;
    int vbad = 0;
    for (int i = 0; i < 1000; i++) if ((rd(0xD800 + i) & 0x0F) != color_backup[i]) vbad++;
    CHECK(vbad == 0, "%d colour RAM bytes not restored", vbad);
    CHECK(rd(0xD018) == vic_backup[0x18] && rd(0xD020) == vic_backup[0x20], "VIC not restored");
    top->cpu_pause_req = 0;
    CHECK(peek(0xA2) != jiffy0, "jiffy clock stands");

    // BASIC: PRINT 6*7
    const char *cmd = "PRINT 6*7\r";
    for (int i = 0; cmd[i]; i++) poke(0x0277 + i, uint8_t(cmd[i]));
    poke(0x00C6, uint8_t(strlen(cmd)));
    run_cycles(200000);
    bool found = false;
    for (int r = 0; r < 25; r++) found |= row(r) == " 42";
    CHECK(found, "BASIC did not answer PRINT 6*7");
    show();
    printf(fails ? "%d FAILED\n" : "ok\n", fails);
    return fails ? 1 : 0;
}

int main(int argc, char **argv)
{
    Verilated::commandArgs(argc, argv);
    top = new Vc64_core;
#ifdef TRACE
    Verilated::traceEverOn(true);
    tfp = new VerilatedVcdC;
    top->trace(tfp, 99);
    tfp->open("trace.vcd");
#endif
    int rc = 3;
    if (argc >= 3 && !strcmp(argv[1], "boot")) rc = boot(argv[2]);
    else if (argc >= 3 && !strcmp(argv[1], "freeze")) rc = freeze(argv[2]);
    else fprintf(stderr, "usage: %s boot|freeze <state>\n", argv[0]);
#ifdef TRACE
    tfp->close();
#endif
    top->final();
    delete top;
    return rc;
}
