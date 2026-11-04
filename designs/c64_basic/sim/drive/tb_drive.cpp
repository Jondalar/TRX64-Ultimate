// SPDX-License-Identifier: GPL-3.0-or-later
// Drive A (drive_unit: Gideon's c1541_drive + drive_mem) and the C64 core on
// one serial bus under Verilator (T05).
//
//   tb_drive boot <state> <rom> <gcr|->  power both up as the app does (with
//                                        the disk in, or none), run to READY,
//                                        check the drive's DOS came up, save
//   tb_drive load <state> [gcr]          restore, [mount the disk into the
//                                        running drive,] LOAD"$",8 and LIST
//                                        through the keyboard buffer, check
//                                        the listing; then device 9 as the app
//                                        sets it (DDR2 $77/$78) and LOAD"$",9
//
// The testbench plays the RISC-V: the drive ROM is written over the system
// bus (cpu_mem_bridge, mig_arb, into the MIG model's memory), the GCR tracks
// go straight into that memory, the drive registers and the parameter RAM are
// written over the IO bus (c1541.cc: init, insert_disk, drive_power,
// drive_reset). LOAD=1 in the environment keeps mig_arb's client 1 reading
// back to back (tb_drive_top.v). The board's IEC port is modelled as lines
// with a pull-up (PORT_RISE sys clocks to come back up, default 300; -1 =
// idle). Exit code 0 = ok.
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
static uint64_t sys_cycles;
static int fails;
static const uint64_t CLK_HALF = 15859;     // 31.5278 MHz
static const uint64_t SYS_HALF = 5000;      // 100 MHz
static const uint64_t UI_HALF = 6667;       // 75 MHz

#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

// ---- the MIG model's memory (16 MB, 128-bit words) ----------------------------
#define MEM top->rootp->tb_drive_top__DOT__mig__DOT__mem
static uint8_t ddr_rd(uint32_t a) { return MEM[(a >> 4) & 0xFFFFF][(a >> 2) & 3] >> (8 * (a & 3)); }
static void ddr_wr(uint32_t a, uint8_t v)
{
    uint32_t &w = MEM[(a >> 4) & 0xFFFFF][(a >> 2) & 3];
    w = (w & ~(0xFFu << (8 * (a & 3)))) | (uint32_t)v << (8 * (a & 3));
}

// ---- the board's IEC port with nothing plugged in: a line goes low with our
// pull at once and comes back up PORT_RISE sys clocks after the release (pull-up
// and cable); PORT_RISE < 0 leaves the port idle ------------------------------
static int port_rise = 300;
static int port_hold[3];
static void port_model()
{
    if (port_rise < 0) { top->port_atn_i = top->port_clk_i = top->port_data_i = 0; return; }
    uint8_t o[3] = {top->port_atn_o, top->port_clk_o, top->port_data_o};
    for (int i = 0; i < 3; i++) {
        if (o[i]) port_hold[i] = port_rise;
        else if (port_hold[i]) port_hold[i]--;
    }
    top->port_atn_i = port_hold[0] != 0; top->port_clk_i = port_hold[1] != 0; top->port_data_i = port_hold[2] != 0;
}

// drive A's head: print the half track and VIA 2 port B writes as they change
static bool head_log;
static uint32_t head_last;
static void head_watch()
{
    uint32_t w = top->drv_dbg[4] & 0xFFFEFF00u;      // status (not byte ready), last PB write
    if (head_log && w != head_last)
        printf("[%8.3f ms] head: half track %u, step %u, motor %u | PB written %02X | GCR at %06X\n",
               now_ps / 1e9, w >> 25, (w >> 23) & 3, (w >> 22) & 1, (w >> 8) & 0xFF,
               top->drv_dbg[0] & 0x3FFFFFF);
    head_last = w;
}

// IEC_LOG=n: print the first n changes of the serial bus after the LOAD is typed, as the C64 and as the drive see it
static int iec_log_left;
static uint32_t iec_last = ~0u;
static uint64_t iec_t0;
static void iec_watch()
{
    if (iec_log_left <= 0) return;
    uint32_t w = top->c64_atn << 5 | top->c64_clk << 4 | top->c64_data << 3 | top->drv_atn << 2 | top->drv_clk << 1 | top->drv_data;
    if (w == iec_last) return;
    iec_last = w;
    iec_log_left--;
    printf("IEC %10.3f us  c64 a%d c%d d%d  drive a%d c%d d%d\n", (now_ps - iec_t0) / 1e6, top->c64_atn, top->c64_clk,
           top->c64_data, top->drv_atn, top->drv_clk, top->drv_data);
}

// ---- clocks: the next edge of any; the port model acts on sys rising edges ----
static uint64_t next_c2 = ~0ull;
static void step()
{
    uint64_t t = next_clk < next_sys ? next_clk : next_sys;
    if (next_ui < t) t = next_ui;
    if (next_c2 < t) t = next_c2;
    now_ps = t;
    // clk2x (the T25.1 SID engine's clock): rises with every clk edge, falls halfway
    if (next_c2 == t) { top->clk2x = 0; next_c2 = ~0ull; }
    bool sys_rise = next_sys == t && !top->sys_clk;
    if (next_sys == t) {
        if (sys_rise) sys_cycles++;
        top->sys_clk = !top->sys_clk; next_sys += SYS_HALF;
    }
    if (next_clk == t) { top->clk = !top->clk; next_c2 = next_clk + CLK_HALF / 2; next_clk += CLK_HALF; top->clk2x = 1; }
    if (next_ui == t) { top->ui_clk = !top->ui_clk; next_ui += UI_HALF; }
    top->eval();
    if (sys_rise) { port_model(); top->eval(); head_watch(); }
    iec_watch();
}

static void sys_tick() { uint64_t c = sys_cycles; while (sys_cycles == c) step(); }
static void clk_tick() { bool c = top->clk; while (top->clk == c) step(); while (top->clk != c) step(); }
static void run_us(double us) { uint64_t end = now_ps + uint64_t(us * 1e6); while (now_ps < end) step(); }

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

// a store from the CPU onto the system bus (drive_mem snoops it)
static void host_write(uint32_t a, uint32_t v, uint8_t be = 0xF)
{
    top->host_address = a; top->host_wdata = v; top->host_byte_en = be;
    top->host_rwn = 0; top->host_request = 1;
    for (int n = 0; n < 100000; n++) {
        sys_tick();
        if (top->mem_rack && top->mem_rack_tag == 0x21) { top->host_request = 0; return; }
    }
    printf("FAIL: host write %08X not taken\n", a);
    exit(4);
}

// ---- the C64's RAM (c64_mem's debug port) ----------------------------------------
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
static void show()
{
    for (int r = 0; r < 25; r++) {
        std::string s = row(r);
        if (!s.empty()) printf("  | %s\n", s.c_str());
    }
}
static int find_row(const char *prefix, int from = 0)
{
    for (int r = from; r < 25; r++)
        if (row(r).rfind(prefix, 0) == 0) return r;
    return -1;
}

static void type(const char *text)
{
    int n = strlen(text);
    for (int i = 0; i < n; i++) poke(0x0277 + i, text[i] == '\n' ? 13 : text[i]);
    poke(0x00C6, n);
}

static void status(const char *what)
{
    uint32_t n = top->dp_reads + top->dp_writes;
    uint32_t r = top->dp_reads;
    printf("[%8.3f ms] %s  bus c64 a%d c%d d%d drive a%d c%d d%d  leds_n %d  drive port r%u w%u avg %.0f max %u ns,"
           " reads avg %.0f max %u ns, >500ns %u  load r%u\n",
           now_ps / 1e9, what, top->c64_atn, top->c64_clk, top->c64_data, top->drv_atn, top->drv_clk,
           top->drv_data, top->leds_n, r, top->dp_writes, n ? 10.0 * top->dp_cycles / n : 0.0,
           10 * top->dp_max, r ? 10.0 * top->dp_rcycles / r : 0.0, 10 * top->dp_rmax, top->dp_over50,
           top->load_reads);
}

// ---- save / restore (the model, the clocks, the memory) ------------------------
static void save(const char *name)
{
    VerilatedSave os;
    os.open(name);
    os << now_ps << next_clk << next_sys << next_ui << sys_cycles << *top;
}
static void restore(const char *name)
{
    VerilatedRestore is;
    is.open(name);
    is >> now_ps >> next_clk >> next_sys >> next_ui >> sys_cycles >> *top;
}

static const uint32_t DRIVE_RAM = 0x0EE0000;   // ultimate_logic_32 g_ram_base
static const uint32_t GCR_BASE  = 0x0100000;   // where the app's GcrImage would be

static std::string drive_ram_text(uint32_t from, int len)
{
    std::string s;
    for (int i = 0; i < len; i++) { uint8_t c = ddr_rd(DRIVE_RAM + from + i); s += (c >= 32 && c < 127) ? char(c) : '.'; }
    return s;
}

// C1541::insert_disk: the tracks (half track 2*(t-1)) and their parameters
static const uint32_t DUMMY = GCR_BASE + 0x80000, DUMMY_LEN = 0x1E0C, ROT = 100000000 / 20;
static int insert(const char *gcr_name)
{
    const uint32_t dummy = DUMMY, rot = ROT;
    FILE *f = fopen(gcr_name, "rb");
    if (!f) { printf("no %s\n", gcr_name); return 2; }
    io_write(3, 0);                     // SENSOR dark, 150 ms
    run_us(150000);
    uint32_t len = 0, bit_time = 0;
    // packed one after the other from an odd address, as in the app's GcrImage
    // (e.g. track 33 at 0x244656)
    std::vector<uint32_t> start(84, 0);
    uint32_t at = GCR_BASE + 0x40001;
    for (int t = 1; t <= 35; t++) {
        uint32_t l;
        if (fread(&l, 4, 1, f) != 1) break;
        start[2 * (t - 1)] = at;
        at += l;
        std::vector<uint8_t> trk(l);
        if (fread(trk.data(), 1, l, f) != l) break;
        for (uint32_t i = 0; i < l; i++) ddr_wr(start[2 * (t - 1)] + i, trk[i]);
        start[2 * (t - 1) + 1] = l;     // length, kept in the odd slot for the loop below
    }
    fclose(f);
    for (int i = 0; i < 84; i++) {
        if (i % 2 == 0 && start[i]) {
            len = start[i + 1]; bit_time = rot / len;
            io_write32(0x1000 + 8 * i, start[i]);
        } else {
            io_write32(0x1000 + 8 * i, dummy);
        }
        io_write32(0x1000 + 8 * i + 4, (len - 1) | (bit_time << 16));
    }
    io_write(0x800, 0x80);
    io_write(3, 1);                     // SENSOR light: not write protected
    io_write(4, 1);                     // INSERTED
    io_write(12, 1);                    // DISKCHANGE
    status("disk inserted");

    return 0;
}

static int boot(const char *state, const char *rom_name, const char *gcr_name)
{
    std::vector<uint8_t> rom(32768);
    FILE *f = fopen(rom_name, "rb");
    if (!f) { printf("no %s\n", rom_name); return 2; }
    size_t n = fread(rom.data(), 1, rom.size(), f);
    fclose(f);
    if (n == 16384) memcpy(&rom[16384], &rom[0], 16384);   // c1541.cc: a 16 K ROM twice
    else if (n != 32768) { printf("ROM %s: %zu bytes\n", rom_name, n); return 2; }

    top->rst = 1; top->sys_rst = 1; top->cpu_reset_req = 0; top->c64_reset_n = 0;
    top->io_read = 0; top->io_write = 0; top->host_request = 0;
    top->load = getenv("LOAD") && atoi(getenv("LOAD"));
    top->dbg_we = 0; top->dbg_addr = 0;
    next_clk = CLK_HALF; next_sys = SYS_HALF; next_ui = UI_HALF;
    for (int i = 0; i < 100; i++) sys_tick();
    top->rst = 0; top->sys_rst = 0;
    for (int i = 0; i < 100; i++) sys_tick();

    // C1541::init: parameter RAM to the dummy track; ROM into the drive image
    // (set_drive_type, as CPU stores); settings: RAMMAP 0, HW_ADDR 0 (drive 8)
    const uint32_t dummy = GCR_BASE + 0x80000, dummy_len = 0x1E0C, rot = 100000000 / 20;
    for (int i = 0; i < 256; i++) {
        io_write32(0x1000 + 8 * i, dummy);
        io_write32(0x1000 + 8 * i + 4, ((rot / dummy_len) << 16) | 0x100);
    }
    for (int a = 0; a < 32768; a += 4)
        host_write(DRIVE_RAM + 0x8000 + a, rom[a] | rom[a + 1] << 8 | rom[a + 2] << 16 | (uint32_t)rom[a + 3] << 24);
    status("ROM written");
    io_write(5, 0x00);
    io_write(2, 0x00);
    // drive_power(true): POWER, drive_reset (RESET 1, 1 ms, then follow the C64 reset)
    io_write(0, 1);
    io_write(1, 1);
    run_us(1000);
    io_write(1, 0x02);
    io_write(0x800, 0x80);

    if (strcmp(gcr_name, "-") && insert(gcr_name)) return 2;

    // both come out of reset together
    top->c64_reset_n = 1;
    for (int n = 0; n < 60; n++) {
        run_us(50000);
        status("boot");
        if (find_row("READY.") >= 0) break;
    }
    run_us(300000);
    show();
    CHECK(find_row("READY.") >= 0, "C64: no READY");
    // the DOS puts its power-on message (73) into its error buffer at $02D5
    std::string msg = drive_ram_text(0x2D5, 24);
    printf("drive $02D5 (written back to DDR): %s\n", msg.c_str());
    CHECK(msg.find("CBM DOS V2.6 1541") != std::string::npos, "drive: no CBM DOS V2.6 1541 at $02D5");
    printf("drive $77/$78 = %02X %02X (listen/talk address)\n", ddr_rd(DRIVE_RAM + 0x77), ddr_rd(DRIVE_RAM + 0x78));
    CHECK(ddr_rd(DRIVE_RAM + 0x77) == 0x28 && ddr_rd(DRIVE_RAM + 0x78) == 0x48, "drive: not device 8");
    CHECK(io_read(0) == 1, "drive: POWER reads back wrong");
    save(state);
    printf("%s, %d failures\n", fails ? "BOOT FAILED" : "boot ok", fails);
    return fails ? 1 : 0;
}

static int load(const char *state, const char *gcr_name)
{
    restore(state);
    head_log = getenv("HEAD_LOG") != nullptr;
    if (getenv("TURBO")) {              // T29: Turbo Control Manual, CPU Speed = menu index $TURBO (3 = 4 MHz, 9 = 16, 15 = 63)
        top->turbo_en = 0; top->turbo_prefer = 0x80 | atoi(getenv("TURBO"));
        run_us(2); top->turbo_upd_tgl = !top->turbo_upd_tgl; run_us(2);
        for (int i = 0; i < 4; i++) clk_tick();
        printf("turbo_stat %08X\n", top->turbo_stat);
    }
    top->load = getenv("LOAD") && atoi(getenv("LOAD"));
    if (gcr_name) {                     // a disk mounted into the running drive (REST mount)
        if (insert(gcr_name)) return 2;
        run_us(500000);
    }
    if (getenv("IEC_LOG")) { iec_log_left = atoi(getenv("IEC_LOG")); iec_t0 = now_ps; }
    type("LOAD\"$\",8\n");
    bool done = false;
    for (int n = 0; n < 200 && !done; n++) {
        run_us(50000);
        status("load");
        if (n % 4 == 0)
            printf("    drive: param ht0 %08X ht34 %08X, GCR fetches/100 ms %u, half track %u (max %u)\n",
                   top->drv_dbg[5], top->drv_dbg[6], top->drv_dbg[7] >> 16, top->drv_dbg[4] >> 25,
                   (top->drv_dbg[4] >> 1) & 0x7F);
        int r = find_row("LOAD\"$\",8");
        if (r >= 0 && find_row("READY.", r + 1) >= 0) done = true;
        if (r >= 0 && find_row("?", r + 1) >= 0) break;
    }
    show();
    if (getenv("LOAD_ONLY")) { printf("%s\n", done ? "LOAD done" : "LOAD NOT done"); return done ? 0 : 1; }   // T29 bench: one LOAD
    CHECK(done, "no READY after LOAD");
    type("LIST\n");
    run_us(300000);
    show();
    CHECK(find_row("0 \"T05 TEST DISK") >= 0, "listing: no disk name");
    CHECK(find_row("1    \"HELLO\"") >= 0, "listing: no HELLO");
    CHECK(find_row("1    \"SECOND\"") >= 0, "listing: no SECOND");
    CHECK(find_row("662 BLOCKS FREE.") >= 0, "listing: not 662 BLOCKS FREE.");

    // C1541::set_sw_address(9) while the drive runs: the app writes $78 and
    // $77 in DDR2, the drive's cache must not keep the old ones
    host_write(DRIVE_RAM + 0x78, 0x49, 0x1);
    host_write(DRIVE_RAM + 0x74, 0x29u << 24, 0x8);
    run_us(1000);
    printf("drive $77/$78 = %02X %02X after set_sw_address(9)\n", ddr_rd(DRIVE_RAM + 0x77), ddr_rd(DRIVE_RAM + 0x78));
    type("LOAD\"$\",9\n");
    done = false;
    for (int n = 0; n < 200 && !done; n++) {
        run_us(50000);
        status("load 9");
        int r = find_row("LOAD\"$\",9");
        if (r >= 0 && find_row("READY.", r + 1) >= 0) done = true;
        if (r >= 0 && find_row("?", r + 1) >= 0) break;
    }
    show();
    CHECK(done && find_row("?") < 0, "no READY after LOAD\"$\",9");
    printf("%s, %d failures\n", fails ? "LOAD FAILED" : "load ok", fails);
    return fails ? 1 : 0;
}

int main(int argc, char **argv)
{
    Verilated::commandArgs(argc, argv);
    if (getenv("PORT_RISE")) port_rise = atoi(getenv("PORT_RISE"));
    top = new Vtb_drive_top;
    if (argc >= 5 && !strcmp(argv[1], "boot")) return boot(argv[2], argv[3], argv[4]);
    if (argc >= 3 && !strcmp(argv[1], "load")) return load(argv[2], argc >= 4 ? argv[3] : nullptr);
    printf("usage: tb_drive boot <state> <rom> <gcr|-> | load <state> [gcr]\n");
    return 2;
}
