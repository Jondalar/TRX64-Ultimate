// SPDX-License-Identifier: GPL-3.0-or-later
// T16 M1 under Verilator (tb_slot_top.v: c64_core with its slot_port pins on a
// transceiver and cartridge model). One test per run:
//   tb_slot idle    an 8K cart in the port, C64_BUS_EXTERNAL 0: the port never
//                   moves, the C64 boots to READY from its own ROMs
//   tb_slot 8k      8K cart, CBM80 autostart by the KERNAL, ROML reads
//   tb_slot 16k     16K cart, ROML and ROMH reads
//   tb_slot ef      EasyFlash-like, ultimax at reset: IO2 RAM, IO1 read back,
//                   flash command sequence reaches the cart (ROM_WR_EN 1), VIC
//                   fetches from ROMH, bank switch, 16K
//   tb_slot ef_ro   the same with ROM_WR_EN 0: no flash write reaches the cart
//   tb_slot pause   ef, then the machine pause: park state, app DMA reads of the
//                   cart (one PHI2 pulse per byte), resume
//   tb_slot lens    ef, then the machine pause and the app's lens unit (T21 §3.4.1): the look says
//                   "the real port serves it", REAL reads of ROML, ROMH and IO1/IO2 with the select
//                   forced come from the cart (also with the app's Ultimax on, and ROMH at $E000 with
//                   the PLA mapping KERNAL), one PHI2 pulse per byte, none for a look, refused while
//                   the machine runs
//   tb_slot btn     ef, then the cart's reset button resets the C64 (the port
//                   RESET is not driven by us for it), the cart boots again
//   tb_slot sock_dec     T37 (spec §6 items 1, 3): no cart, the SID sockets enabled; the app's DMA reads and
//                   writes hit socket 1/2 per base/mask (detection mask $F0, $FE, split A5/A8, unmapped,
//                   disabled, $DE00/$DF00, overlap of both, I/O banked out): which CS goes low
//   tb_slot sock_rw      items 2, 3: a raster-IRQ program writes and reads back 25 registers of each chip with
//                   CPU cycles, then the app's DMA round trip; chip model contention/setup/hold checks
//   tb_slot sock_split   item 4: the split A5/A7/A9: SLOT_SID_ADDR picks the chip's personality (A/B), both sockets
//   tb_slot sock_detect  item 6: the byte sequences of detectFPGASID / detectRemakes against FPGASID, ARMSID,
//                   SwinSID, a plain chip and no chip (no hang)
//   tb_slot sock_de00    item 1/5: an EasyFlash-like cart armed and a socket at $DE00: the cart keeps IO1
//                   (Q4, Alex 2026-10-16), the socket sees nothing, the cart still serves ROML; disabled: IO1 works
//   tb_slot sock_pause   item 7: the IRQ program while the machine is paused and resumed 40 times: no CS in the
//                   park state, no repeated cycle, results intact
//   tb_slot sock_idle    the sockets enabled and idle: PHI2 runs, nothing else moves, K17 idle level; sockets off
//                   with a split: PHI2 stays low
// Every run also checks contention, turnaround, idle, write setup/hold,
// address stability and the read samples' age (tb_slot_top.v). Exit 0 = PASS.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "Vtb_slot_top.h"
#include "verilated.h"

static Vtb_slot_top *top;
static uint64_t now_ps, next_clk, clk_edges, next_c2 = ~0ull;
static int fails;
static const uint64_t CLK_HALF = 15859;     // 31.5278 MHz
static const char *bin_dir = ".";
static int acc = 8;                         // the cart's access time in ticks (31.7 ns)
static uint32_t allow_cont = 0;             // T37: two chips selected on purpose (overlap test)

#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

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
static void run_us(double us) { uint64_t end = now_ps + uint64_t(us * 1e6); while (now_ps < end) step(); }

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
static bool ready_shown()
{
    for (int r = 0; r < 25; r++)
        if (row(r).rfind("READY.", 0) == 0) return true;
    return false;
}

// every ROM byte has a known value
static uint8_t P(int bank, int half, int off)
{
    return uint8_t(off * 7 + (off >> 8) * 13 + bank * 31 + half * 101);
}
static void rom_w(int bank, int half, int off, uint8_t v)
{
    top->rom_addr = (bank << 14) | (half << 13) | off; top->rom_wdata = v; top->rom_we = 1;
    clk_tick();
    top->rom_we = 0;
}
static std::vector<uint8_t> load(const char *name)
{
    std::string p = std::string(bin_dir) + "/" + name;
    FILE *f = fopen(p.c_str(), "rb");
    if (!f) { printf("FAIL: cannot open %s\n", p.c_str()); exit(2); }
    std::vector<uint8_t> v(16384);
    v.resize(fread(v.data(), 1, v.size(), f));
    fclose(f);
    return v;
}
static void fill_rom()
{
    for (int b = 0; b < 8; b++)
        for (int h = 0; h < 2; h++)
            for (int o = 0; o < 8192; o++) rom_w(b, h, o, P(b, h, o));
}
static void put(int bank, int half, const std::vector<uint8_t> &v)
{
    for (size_t i = 0; i < v.size() && i < 8192; i++) rom_w(bank, half, i, v[i]);
}

static uint8_t dma(uint16_t a, bool we = false, uint8_t v = 0)
{
    top->dma_addr = a; top->dma_we = we; top->dma_wdata = v; top->dma_req = 1;
    clk_tick();
    top->dma_req = 0;
    for (int n = 0; n < 200000 && !top->dma_done; n++) clk_tick();
    if (!top->dma_done) { printf("FAIL: DMA %04X not done\n", a); exit(3); }
    return top->dma_rdata;
}

static bool wait_for(uint16_t a, uint8_t v, double ms)
{
    uint64_t end = now_ps + uint64_t(ms * 1e9);
    while (now_ps < end) {
        run_us(200);
        if (peek(a) == v) return true;
    }
    return false;
}

// power on with the cart's program at (bank, half); the app's reset as the
// firmware does it (C64_MODE reset with C64_BUS_* set, then released)
static void power_on(int mode, int bus_int, int bus_ext, bool wr_en, const char *bin, int bank, int half)
{
    top->rst = 1; top->cpu_reset_req = 1;
    top->cart_mode = 0; top->acc_ticks = acc;
    top->bus_int = bus_int; top->bus_ext = bus_ext; top->rom_wr_en = wr_en;
    next_clk = CLK_HALF;
    for (int i = 0; i < 50; i++) clk_tick();
    fill_rom();
    put(bank, half, load(bin));
    top->cart_mode = mode;
    top->rst = 0;
    run_us(200);                                    // the app holds the C64 in reset
    top->cpu_reset_req = 0;
}

static void report()
{
    printf("port: %u PHI2 pulses, %u park clocks; status %08X cnt0 %08X cnt1 %08X; flash log %u\n",
           top->n_phi2, top->n_park, top->slot_status, top->slot_cnt0, top->slot_cnt1, top->log_n);
    printf("reads used: CPU %u (min age %u ticks), VIC %u (min age %u ticks); writes %u\n",
           top->n_cpu_rd, top->min_cpu_age, top->n_vic_rd, top->min_vic_age, top->n_wr);
    CHECK(top->e_cont_port == 0, "port D contention %u", top->e_cont_port);
    CHECK(top->e_cont_fpga == 0, "FPGA D contention %u", top->e_cont_fpga);
    CHECK(top->e_turn == 0, "DIR turned with OE on %u", top->e_turn);
    CHECK(top->e_adir == 0, "address transceiver turned %u", top->e_adir);
    CHECK(top->e_idle == 0, "output active while idle %u", top->e_idle);
    CHECK(top->e_wr_setup == 0, "write data setup %u", top->e_wr_setup);
    CHECK(top->e_wr_hold == 0, "write hold %u", top->e_wr_hold);
    CHECK(top->e_addr == 0, "address moved under a strobe %u", top->e_addr);
    CHECK(top->e_cpu_late == 0 && top->e_cpu_data == 0, "CPU samples late %u, wrong %u",
          top->e_cpu_late, top->e_cpu_data);
    CHECK(top->e_vic_late == 0 && top->e_vic_data == 0, "VIC samples late %u, wrong %u",
          top->e_vic_late, top->e_vic_data);
    CHECK(!(top->slot_status & (1u << 17)), "guard fault, cause %u", (top->slot_status >> 20) & 15);
    CHECK(top->e_sid_cs_phi == 0, "SID CS low with PHI2 low %u", top->e_sid_cs_phi);
    CHECK(top->e_sid_pre == 0, "SID address/R/W moved as CS fell %u", top->e_sid_pre);
    CHECK(top->e_sid_hold == 0, "SID address/R/W/data moved at PHI2's fall %u", top->e_sid_hold);
    CHECK(top->e_sid_park == 0, "SID CS or PHI2 active in the park state %u", top->e_sid_park);
    CHECK(top->e_sid_late == 0 && top->e_sid_data == 0, "SID reads late %u, wrong %u", top->e_sid_late, top->e_sid_data);
    CHECK(top->e_sid_idle == 0, "bus line active without a socket cycle %u", top->e_sid_idle);
    CHECK(top->e_sid_a == 0, "SLOT_SID_ADDR wrong %u", top->e_sid_a);
    CHECK(top->e_sid_cont == allow_cont, "two SIDs drove D %u (allowed %u)", top->e_sid_cont, allow_cont);
    if (top->n_sid_wr || top->n_sid_rd)
        printf("sockets: CS1 %u, CS2 %u, both %u; chip writes %u, reads %u, personality B %u\n",
               top->n_cs1, top->n_cs2, top->n_cs_both, top->n_sid_wr, top->n_sid_rd, top->n_sid_a1);
}

static void test_cart(bool big)
{
    power_on(big ? 2 : 1, 0x0, 0xF, false, big ? "cart16k.bin" : "cart8k.bin", 0, 0);
    CHECK(wait_for(0xC100, 0xAA, 100), "cart program did not finish");
    int bad = 0;
    for (int i = 0; i < 256; i++) {
        if (peek(0xC000 + i) != P(0, 0, 0x1F00 + i)) bad++;
        if (big && peek(0xC200 + i) != P(0, 1, 0x1F00 + i)) bad++;
    }
    CHECK(bad == 0, "%d bytes read from the cart differ", bad);
    CHECK(peek(0xC101) == 1, "cart started %u times", peek(0xC101));
    CHECK((top->slot_status & 0x3) == (big ? 0 : 1), "GAME/EXROM pins %X", top->slot_status & 3);
    CHECK(top->slot_status & (1u << 16), "not armed");
    CHECK(top->n_cpu_rd >= 256, "only %u routed reads", top->n_cpu_rd);
}

// T74: a routed IO1/IO2 read that the cart does not answer returns the VIC's byte of the PHI1 before (the FPGA puts it
// on the port's D lines; the board's bus hold keeps it), not the $A5 the CPU wrote to the same address a cycle earlier
static void test_openbus()
{
    power_on(1, 0x0, 0xF, false, "openbus.bin", 0, 0);
    CHECK(wait_for(0xC100, 0xAA, 300), "openbus program did not finish");
    for (int io = 1; io <= 2; io++) {
        int n[256] = {0}, bad = 0;
        for (int i = 0; i < 256; i++) n[peek((io == 2 ? 0xC200 : 0xC300) + i)]++;
        for (int v = 0; v < 256; v++)
            if (n[v] && v != 0x5C && v != 0x11 && v != 0x22) bad += n[v];
        printf("IO%d reads: $5C %d, $11 %d, $22 %d, $A5 %d, other %d\n", io, n[0x5C], n[0x11], n[0x22], n[0xA5], bad);
        for (int v = 0; v < 256; v++)
            if (n[v] && v != 0x5C && v != 0x11 && v != 0x22) printf("  other: $%02X x %d\n", v, n[v]);
        CHECK(bad == 0 && n[0xA5] == 0, "IO%d: %d reads are not the VIC's byte (%d echo the write)", io, bad, n[0xA5]);
        CHECK(n[0x5C] > 128, "IO%d: only %d reads are the idle fetch", io, n[0x5C]);
    }
    CHECK(top->n_cpu_rd >= 512, "only %u routed reads", top->n_cpu_rd);
}

static void ef_boot(bool wr_en)
{
    power_on(3, 0x0, 0xF, wr_en, "ef.bin", 0, 1);
    CHECK(wait_for(0x0206, 0xAA, 100), "EF program did not finish");
    CHECK(peek(0x0200) == 0x5A && peek(0x0201) == 0xA5, "IO2 RAM %02X %02X", peek(0x0200), peek(0x0201));
    CHECK(peek(0x0202) == 0x03, "IO1 read back %02X", peek(0x0202));
    CHECK(peek(0x0203) == P(3, 0, 0x10), "ROML bank 3 in ultimax %02X", peek(0x0203));
    CHECK(peek(0x0204) == P(1, 0, 0x20), "ROML bank 1 in 16K %02X", peek(0x0204));
    CHECK(peek(0x0205) == P(1, 1, 0x30), "ROMH bank 1 in 16K %02X", peek(0x0205));
    CHECK(top->ef_mode_o == 0x07 && top->ef_bank_o == 1, "EF regs %02X %02X", top->ef_mode_o, top->ef_bank_o);
    CHECK(top->n_vic_rd > 1000, "VIC ultimax fetches %u", top->n_vic_rd);
}

static void check_flash_log(bool wr_en)
{
    static const uint32_t want[5] = {
        0x00000555u << 8 | 0xAA, 0x000002AAu << 8 | 0x55, 0x00000555u << 8 | 0xA0,
        0x00000123u << 8 | 0x42, (1u << 29) | 0x00000555u << 8 | 0xAA };
    if (!wr_en) { CHECK(top->log_n == 0, "%u flash writes reached the cart", top->log_n); return; }
    CHECK(top->log_n == 5, "%u flash writes, want 5", top->log_n);
    for (int i = 0; i < 5 && i < top->log_n; i++) {
        top->log_idx = i; clk_tick();
        CHECK(top->log_word == want[i], "flash write %d: %08X, want %08X", i, top->log_word, want[i]);
    }
}

static void test_pause()
{
    ef_boot(false);
    run_us(500);
    top->halt_req = 1;
    for (int n = 0; n < 100000 && !top->halted; n++) clk_tick();
    CHECK(top->halted, "no pause");
    run_us(50);
    uint32_t ph = top->n_phi2, pk = top->n_park;
    run_us(2000);
    CHECK(top->n_phi2 == ph, "PHI2 moved while paused (%u)", top->n_phi2 - ph);
    CHECK(!top->p_phi2 && top->n_park - pk > 60000, "not parked (%u)", top->n_park - pk);
    uint8_t c0 = peek(0x0207);
    int bad = 0, pulses_bad = 0;
    dma(0x8000);                                    // the first one lets the CPU reach a read
    for (int i = 0; i < 16; i++) {
        uint32_t p0 = top->n_phi2;
        if (dma(0x8000 + i) != P(1, 0, i)) bad++;
        run_us(5);
        if (top->n_phi2 - p0 != 1) pulses_bad++;
    }
    if (dma(0xA030) != P(1, 1, 0x30)) bad++;
    CHECK(bad == 0, "%d DMA bytes from the cart differ", bad);
    CHECK(pulses_bad == 0, "%d DMA bytes without exactly one PHI2 pulse", pulses_bad);
    run_us(2000);
    top->halt_req = 0;
    for (int n = 0; n < 2000000 && top->halted; n++) clk_tick();
    CHECK(!top->halted, "no resume");
    run_us(2000);
    CHECK(peek(0x0207) != c0, "the cart's loop does not go on after the resume");
}

// LK_RES: b7:0 data, b8 done, b9 hit, b10 ext, b11 real, b12 refused, b31 busy
static uint32_t lk(uint16_t a, int cls, int real)
{
    top->lk_req = a | (cls << 16) | (real << 18);
    top->lk_go = 1;
    clk_tick();
    top->lk_go = 0;
    clk_tick();
    for (int n = 0; n < 200000 && (top->lk_res >> 31); n++) clk_tick();
    CHECK(!(top->lk_res >> 31), "lens request $%04X cls %d real %d did not finish", a, cls, real);
    return top->lk_res;
}

static void test_lens()
{
    ef_boot(false);
    run_us(500);
    uint32_t r = lk(0x8000, 0, 1);
    CHECK((r & 0x1100) == 0x1100 && !(r & 0x800), "REAL while running must be refused: %08X", r);
    top->halt_req = 1;
    for (int n = 0; n < 100000 && !top->halted; n++) clk_tick();
    CHECK(top->halted, "no pause");
    run_us(50);
    int bad = 0, pulses_bad = 0, ext_bad = 0;
    // the look: the real port serves ROML/ROMH/IO1/IO2 here, nothing is read, no PHI2
    uint32_t p0 = top->n_phi2;
    for (int cls = 0; cls < 4; cls++) {
        r = lk(cls == 0 ? 0x8000 : cls == 1 ? 0xA000 : cls == 2 ? 0xDE00 : 0xDF00, cls, 0);
        if ((r & 0x1D00) != 0x0500 || (uint8_t)r != 0xFF) ext_bad++;
    }
    CHECK(ext_bad == 0, "%d looks did not report the real port (ext)", ext_bad);
    run_us(5);
    CHECK(top->n_phi2 == p0, "a look moved PHI2 (%u pulses)", top->n_phi2 - p0);
    lk(0x8000, 0, 1);                               // the first one lets the CPU reach a read
    // EF in 16K mode, bank 1 (ef_boot): ROML = P(1,0,i), ROMH = P(1,1,i)
    for (int app_ult = 0; app_ult < 2; app_ult++) {
        top->app_ultimax = app_ult;
        for (int i = 0; i < 16; i++) {
            p0 = top->n_phi2;
            r = lk(0x8000 + i, 0, 1);
            if (!(r & 0x800) || (r & 0x1000) || (uint8_t)r != P(1, 0, i)) bad++;
            run_us(5);
            if (top->n_phi2 - p0 != 1) pulses_bad++;
        }
        // ROMH asked at $E030 and $A030: the PLA in 16K mode maps KERNAL at $E000; the forced select does not care
        r = lk(0xE030, 1, 1);
        if ((uint8_t)r != P(1, 1, 0x30)) bad++;
        r = lk(0xA030, 1, 1);
        if ((uint8_t)r != P(1, 1, 0x30)) bad++;
        // ROML at an address outside $8000-$9FFF: only the select matters, A12-A0 pick the byte
        r = lk(0xC020, 0, 1);
        if ((uint8_t)r != P(1, 0, 0x20)) bad++;
        // IO1: bank and mode registers; IO2: the cart's RAM
        r = lk(0xDE00, 2, 1);
        if ((uint8_t)r != 1) bad++;
        r = lk(0xDE02, 2, 1);
        if ((uint8_t)r != 0x07) bad++;
        r = lk(0xDF00, 3, 1);
        if ((uint8_t)r != 0x5A) bad++;
        r = lk(0xDF01, 3, 1);
        if ((uint8_t)r != 0xA5) bad++;
    }
    top->app_ultimax = 0;
    CHECK(bad == 0, "%d REAL bytes from the cart differ", bad);
    CHECK(pulses_bad == 0, "%d REAL bytes without exactly one PHI2 pulse", pulses_bad);
    // the lens unit does not leave the machine resumed or the cart changed
    CHECK(top->halted, "the lens unit resumed the machine");
    CHECK(top->ef_mode_o == 0x07 && top->ef_bank_o == 1, "EF regs %02X %02X", top->ef_mode_o, top->ef_bank_o);
    run_us(2000);
    top->halt_req = 0;
    for (int n = 0; n < 2000000 && top->halted; n++) clk_tick();
    CHECK(!top->halted, "no resume");
    run_us(2000);
}


// Soak of the lens unit the way the firmware's dump drives it (trx_lens.c cart_byte: the look, then, when it
// says "real port", the REAL cycle), random class and address, the EF's bank changed by a DMA write to $DE00
// between reads. Every answer is compared with the model's byte; anything but the right byte is counted
// by kind (refused, no done, zero, other). N from the environment (T16_SOAK, default 20000).
static void test_lens_soak()
{
    ef_boot(false);
    run_us(500);
    top->halt_req = 1;
    for (int n = 0; n < 100000 && !top->halted; n++) clk_tick();
    CHECK(top->halted, "no pause");
    run_us(50);
    lk(0x8000, 0, 1);
    long n = getenv("T16_SOAK") ? atol(getenv("T16_SOAK")) : 20000;
    uint32_t seed = 12345;
    auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return seed >> 8; };
    int bank = 1;
    long refused = 0, nodone = 0, zero = 0, other = 0, notext = 0;
    for (long i = 0; i < n; i++) {
        if ((rnd() & 15) == 0) {
            bank = 1 + (rnd() % 7);
            dma(0xDE00, true, bank);
            run_us(1);
            CHECK(top->ef_bank_o == bank, "DMA write of bank %d: the cart has %d", bank, top->ef_bank_o);
        }
        int cls = rnd() & 1;
        uint16_t a = cls ? (0xA000 + (rnd() & 0x1FFF)) : (0x8000 + (rnd() & 0x1FFF));
        uint8_t want = P(bank, cls, a & 0x1FFF);
        uint32_t r = lk(a, cls, 0);
        if (!(r & 0x400)) { notext++; continue; }
        r = lk(a, cls, 1);
        if (!(r & 0x100)) nodone++;
        else if (r & 0x1000) refused++;
        else if ((uint8_t)r != want) { if ((uint8_t)r == 0) zero++; else other++; if (other + zero < 10) printf("bad byte $%04X bank %d: got %02X want %02X (%08X)\n", a, bank, (uint8_t)r, want, r); }
        if ((rnd() & 3) == 0) run_us((rnd() & 7) * 0.3);
    }
    printf("lens soak: %ld reads, refused %ld, not done %ld, zero %ld, other %ld, look without ext %ld\n",
           n, refused, nodone, zero, other, notext);
    CHECK(refused + nodone + zero + other + notext == 0, "lens soak: errors");
    CHECK(top->halted, "the lens unit resumed the machine");
    top->halt_req = 0;
    for (int n2 = 0; n2 < 2000000 && top->halted; n2++) clk_tick();
    CHECK(!top->halted, "no resume");
}

// DMA writes (the app's POKE while the machine is paused) reach the cart like a CPU write: IO1 registers
// (latched at PHI2's fall: the hold tick), then, in ultimax, a flash command sequence on ROML/ROMH with
// ROM_WR_EN. The cycle ends at core tick 16 (ext_dma_cyc falls with phi2_n); the port keeps address, R/W,
// strobe and data one more tick like a CPU cycle (slot_port.v q_*).
static void test_dma_wr()
{
    ef_boot(true);
    check_flash_log(true);
    run_us(500);
    top->halt_req = 1;
    for (int n = 0; n < 100000 && !top->halted; n++) clk_tick();
    CHECK(top->halted, "no pause");
    run_us(50);
    dma(0x8000);                                    // the first one lets the CPU reach a read
    dma(0xDE00, true, 5); run_us(1);
    CHECK(top->ef_bank_o == 5, "DMA write of $DE00: bank %d", top->ef_bank_o);
    dma(0xDE02, true, 0x05); run_us(1);             // ultimax
    CHECK(top->ef_mode_o == 0x05, "DMA write of $DE02: %02X", top->ef_mode_o);
    dma(0xDE00, true, 2); run_us(1);
    CHECK(top->ef_bank_o == 2, "DMA write of $DE00: bank %d", top->ef_bank_o);
    uint32_t n0 = top->log_n;
    dma(0x8555, true, 0xAA); dma(0x82AA, true, 0x55); dma(0x8555, true, 0xA0); dma(0x8123, true, 0x77);
    dma(0xE555, true, 0xAA);
    run_us(1);
    CHECK(top->log_n - n0 == 5, "%u DMA flash writes reached the cart, want 5", top->log_n - n0);
    static const uint32_t want[5] = {
        (2u << 23) | 0x555u << 8 | 0xAA, (2u << 23) | 0x2AAu << 8 | 0x55, (2u << 23) | 0x555u << 8 | 0xA0,
        (2u << 23) | 0x123u << 8 | 0x77, (1u << 29) | (2u << 23) | 0x555u << 8 | 0xAA };
    for (int i = 0; i < 5 && n0 + i < 16; i++) {
        top->log_idx = n0 + i; clk_tick();
        CHECK(top->log_word == want[i], "DMA flash write %d: %08X, want %08X", i, top->log_word, want[i]);
    }
    CHECK(top->halted, "resumed");
    top->halt_req = 0;
    for (int n = 0; n < 2000000 && top->halted; n++) clk_tick();
    CHECK(!top->halted, "no resume");
}

// T16 Q8: an app DMA write to $00/$01 while the CPU is held goes into the 6510 port (and the RAM below it),
// so the PLA of the next DMA cycles sees it; the program's port comes back when the hold ends. Run with a
// whole-machine pause (halt_req) and with the CPU pause alone (the app's C64_STOP).
static void port_case(bool whole)
{
    power_on(1, 0x0, 0xF, false, "cport.bin", 0, 0);
    CHECK(wait_for(0xC100, 0xAA, 100), "cart program did not finish");
    CHECK(top->cpu_pout == 0x35, "program's $01 %02X", top->cpu_pout);
    uint8_t pdir = top->cpu_pdir;
    uint8_t c0 = peek(0xC101);
    run_us(300);
    CHECK(peek(0xC101) != c0, "stage does not count");
    if (whole) {
        top->halt_req = 1;
        for (int n = 0; n < 100000 && !top->halted; n++) clk_tick();
        CHECK(top->halted, "no pause");
    } else {
        top->cpu_pause_req = 1;
        run_us(100);
    }
    run_us(50);
    const uint16_t A = 0x9F10;
    uint8_t want = P(0, 0, 0x1F10);
    dma(A);                                         // the first one holds the CPU
    CHECK(top->cpu_pout == 0x35, "port changed by a read %02X", top->cpu_pout);
    uint8_t ram = dma(A);
    CHECK(ram != want, "ROML shows with $01 = $35 (%02X, RAM %02X, port %02X/%02X)", ram, peek(A), top->cpu_pdir, top->cpu_pout);
    dma(0x0001, true, 0x37);
    run_us(1);
    CHECK(top->cpu_pout == 0x37, "DMA write of $01 did not reach the port (%02X)", top->cpu_pout);
    CHECK(top->cpu_pdir == pdir, "$00 changed %02X", top->cpu_pdir);
    uint32_t p0 = top->n_phi2;
    CHECK(dma(A) == want, "ROML does not show with $01 = $37");
    run_us(2);
    CHECK(!whole || top->n_phi2 - p0 == 1, "DMA read of the cart: %u PHI2 pulses", top->n_phi2 - p0);
    dma(0x0000, true, 0xFF);                        // $00 as well
    run_us(1);
    CHECK(top->cpu_pdir == 0xFF, "DMA write of $00 did not reach the port (%02X)", top->cpu_pdir);
    dma(0x0002, true, 0x55);                        // not the port
    run_us(1);
    CHECK(top->cpu_pdir == 0xFF && top->cpu_pout == 0x37, "a write of $02 changed the port");
    CHECK(peek(0x0001) == 0x37 && peek(0x0000) == 0xFF, "RAM below the port: %02X %02X", peek(0x0001), peek(0x0000));
    run_us(1000);                                   // the hold lasts: nothing else touches the port
    CHECK(top->cpu_pdir == 0xFF && top->cpu_pout == 0x37, "port moved while held");
    if (whole) {
        top->halt_req = 0;
        for (int n = 0; n < 2000000 && top->halted; n++) clk_tick();
        CHECK(!top->halted, "no resume");
    } else {
        top->cpu_pause_req = 0;
    }
    run_us(5);
    CHECK(top->cpu_pdir == pdir && top->cpu_pout == 0x35, "port not restored: $00 %02X $01 %02X", top->cpu_pdir, top->cpu_pout);
    uint8_t c1 = peek(0xC101);
    run_us(500);
    CHECK(peek(0xC101) != c1, "the program does not go on after the resume");
    CHECK(top->cpu_pout == 0x35, "port %02X after the resume", top->cpu_pout);
    // a second hold without a port write leaves the program's port alone
    if (whole) { top->halt_req = 1; for (int n = 0; n < 100000 && !top->halted; n++) clk_tick(); }
    else { top->cpu_pause_req = 1; run_us(100); }
    dma(0x0002, true, 0x11);
    if (whole) { top->halt_req = 0; for (int n = 0; n < 2000000 && top->halted; n++) clk_tick(); }
    else top->cpu_pause_req = 0;
    run_us(500);
    CHECK(top->cpu_pout == 0x35 && top->cpu_pdir == pdir, "port after a hold without a port write: %02X %02X", top->cpu_pdir, top->cpu_pout);
    CHECK(!(top->slot_status & (1u << 17)), "guard fault");
}
static void test_cpuport() { port_case(true); }
static void test_cpuport_cp() { port_case(false); }

// The firmware's lens read as the REST handler runs it, one pause per chunk: the CPU pause (C64_STOP), then the
// whole-machine pause (GATE_CTRL b0), a few look + REAL reads, the gate released, the CPU pause released, the
// program runs for a random time. N cycles (T16_PAUSES, default 300), the first request after each pause is
// the interesting one (the CPU runs to its next read, the gate opens for the DMA cycle).
static void test_lens_pause_soak()
{
    ef_boot(false);
    long n = getenv("T16_PAUSES") ? atol(getenv("T16_PAUSES")) : 300;
    uint32_t seed = 777;
    auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return seed >> 8; };
    long bad = 0, refused = 0, nodone = 0, reads = 0, first_bad = 0;
    for (long c = 0; c < n; c++) {
        run_us(100 + (rnd() % 3000) * 0.37);        // the program runs: the stop lands anywhere
        bool cpu_first = rnd() & 1;
        if (cpu_first) top->cpu_pause_req = 1;
        run_us((rnd() % 20) * 0.5);
        top->halt_req = 1;
        for (int k = 0; k < 100000 && !top->halted; k++) clk_tick();
        if (!top->halted) { CHECK(0, "cycle %ld: no pause", c); break; }
        int cnt = 1 + rnd() % 8;
        for (int i = 0; i < cnt; i++) {
            int cls = rnd() & 1;
            uint16_t a = cls ? (0xA000 + (rnd() & 0x1FFF)) : (0x8000 + (rnd() & 0x1FFF));
            uint8_t want = P(1, cls, a & 0x1FFF);
            uint32_t r = lk(a, cls, 0);
            reads++;
            bool ok = (r & 0x400) != 0;
            if (ok) {
                r = lk(a, cls, 1);
                if (!(r & 0x100)) { nodone++; ok = false; }
                else if (r & 0x1000) { refused++; ok = false; }
                else if ((uint8_t)r != want) ok = false;
            }
            if (!ok) { bad++; if (i == 0) first_bad++; if (bad < 10) printf("cycle %ld read %d $%04X: %08X want %02X\n", c, i, a, r, want); }
        }
        top->halt_req = 0;
        for (int k = 0; k < 2000000 && top->halted; k++) clk_tick();
        if (top->halted) { CHECK(0, "cycle %ld: no resume", c); break; }
        if (cpu_first) top->cpu_pause_req = 0;
        else if (rnd() & 1) { top->cpu_pause_req = 1; run_us(2); top->cpu_pause_req = 0; }
    }
    printf("lens pause soak: %ld pauses, %ld reads, bad %ld (first of a pause %ld), refused %ld, not done %ld\n",
           n, reads, bad, first_bad, refused, nodone);
    CHECK(bad == 0, "lens pause soak: errors");
}

static void test_btn()
{
    ef_boot(false);
    poke(0x0206, 0);
    uint8_t boots = peek(0x0208);
    bool drove = false;
    top->cart_btn = 1;
    for (uint64_t end = now_ps + 300000000ull; now_ps < end; ) { step(); drove |= top->p_rst_o; }
    top->cart_btn = 0;
    for (uint64_t end = now_ps + 100000000ull; now_ps < end; ) { step(); drove |= top->p_rst_o; }
    CHECK(!drove, "we drove the port RESET for the cart's own reset");
    CHECK(wait_for(0x0206, 0xAA, 100), "the cart did not boot again");
    CHECK(peek(0x0208) == boots + 1, "boots %u, want %u", peek(0x0208), boots + 1);
}


// ---- T37: the SID sockets (spec §6) -------------------------------------------------------------------------------------
// the app's registers as ultimate_sys puts them out: en bit n = socket n+1, base/mask = A11..A4 of each, split =
// C64_STEREO_ADDRSEL (0 Off, 1-5 = A5-A9; the app has cleared the bit in mask and base), chip = sid_mode of the bench
static void socks(int en, int b1, int m1, int b2, int m2, int split, int chip)
{
    top->sock_en = en; top->sock_base = (b2 << 8) | b1; top->sock_mask = (m2 << 8) | m1;
    top->stereo_sel = split; top->sid_mode = chip; top->sid_acc = 7;
    for (int i = 0; i < 8; i++) clk_tick();                 // through the synchronizers
}
static const int SPLIT_BIT[6] = {0x00, 0x02, 0x04, 0x08, 0x10, 0x20};  // in the A11..A4 byte
static void socks_split(int chip, int split)
{
    int m = 0xFE & ~SPLIT_BIT[split];
    socks(3, 0x40 & ~SPLIT_BIT[split], m, 0x50 & ~SPLIT_BIT[split], m, split, chip);
}
static void boot_nocart()
{
    power_on(0, 0xF, 0x0, false, "cart8k.bin", 0, 0);
    bool ok = false;
    for (int k = 0; k < 80 && !ok; k++) { run_us(50000); ok = ready_shown(); }
    CHECK(ok, "C64: no READY");
}
static void pause_machine()
{
    top->halt_req = 1;
    for (int n = 0; n < 100000 && !top->halted; n++) clk_tick();
    CHECK(top->halted, "no pause");
    run_us(50);
    dma(0x0400);                                            // the first one lets the CPU reach a read
}
static void resume_machine()
{
    top->halt_req = 0;
    for (int n = 0; n < 2000000 && top->halted; n++) clk_tick();
    CHECK(!top->halted, "no resume");
}
// which CS went low for this app DMA access: b0 socket 1, b1 socket 2
static int probe(uint16_t a, bool we = false, uint8_t v = 0)
{
    uint32_t c1 = top->n_cs1, c2 = top->n_cs2;
    dma(a, we, v);
    run_us(2);
    return (top->n_cs1 != c1 ? 1 : 0) | (top->n_cs2 != c2 ? 2 : 0);
}
struct Case { uint16_t a; int want; };
static void cases(const char *name, const Case *c, int n, bool also_write)
{
    for (int i = 0; i < n; i++) {
        int r = probe(c[i].a);
        CHECK(r == c[i].want, "%s: read $%04X selects %d, want %d", name, c[i].a, r, c[i].want);
        if (also_write) {
            int w = probe(c[i].a, true, 0x21);
            CHECK(w == c[i].want, "%s: write $%04X selects %d, want %d", name, c[i].a, w, c[i].want);
        }
    }
}
#define CASES(name, ...) do { static const Case c_[] = __VA_ARGS__; cases(name, c_, sizeof(c_) / sizeof(c_[0]), true); } while (0)

static void test_sock_dec()
{
    socks(3, 0x40, 0xFE, 0x42, 0xFE, 0, 1);
    boot_nocart();
    CHECK(top->n_sid_wr > 0, "the KERNAL's own SID writes did not reach socket 1");
    CHECK(top->n_phi2 > 0, "PHI2 does not run with the sockets enabled");
    pause_machine();
    // the sockets at $D400 and $D420 (mask $FE: A4 is not compared)
    CASES("D400/D420", {{0xD400, 1}, {0xD41F, 1}, {0xD420, 2}, {0xD43F, 2}, {0xD440, 0}, {0xD4FF, 0}, {0xD500, 0},
                        {0xD7E0, 0}, {0xC800, 0}, {0xD300, 0}});
    // reads of the other chips' pages: the VIC, the CIAs, ROM: no CS
    for (uint16_t a : {0xD000, 0xD011, 0xDC00, 0xDD00, 0xA000, 0xE000, 0x0400, 0xDE00, 0xDF00}) {
        int r = probe(a);
        CHECK(r == 0, "$%04X selects %d", a, r);
    }
    // the detection setup of the app: $D400-$D4FF and $D500-$D5FF (mask $F0)
    socks(3, 0x40, 0xF0, 0x50, 0xF0, 0, 1);
    CASES("detection F0", {{0xD400, 1}, {0xD4FF, 1}, {0xD500, 2}, {0xD5FF, 2}, {0xD600, 0}, {0xD3FF, 0}, {0xD7FF - 0x100, 0}});
    // socket 1 only, then socket 2 only
    socks(1, 0x40, 0xFE, 0x42, 0xFE, 0, 1);
    CASES("socket 1 only", {{0xD400, 1}, {0xD420, 0}});
    socks(2, 0x40, 0xFE, 0x42, 0xFE, 0, 1);
    CASES("socket 2 only", {{0xD400, 0}, {0xD420, 2}});
    // the split: A5 is not compared, the socket covers both halves; the A8 split with the sockets in the page
    socks_split(1, 1);
    CASES("split A5", {{0xD400, 1}, {0xD41F, 1}, {0xD420, 1}, {0xD43F, 1}, {0xD440, 0}, {0xD500, 2}, {0xD53F, 2}, {0xD540, 0}});
    socks(3, 0x40, 0xEE, 0x60, 0xEE, 4, 1);                 // the app: both sockets with the A8 bit cleared in mask and base
    CASES("split A8", {{0xD400, 1}, {0xD41F, 1}, {0xD500, 1}, {0xD51F, 1}, {0xD420, 0}, {0xD520, 0}, {0xD600, 2},
                       {0xD700, 2}, {0xD71F, 2}, {0xD620, 0}});
    socks(3, 0x01, 0xFE, 0x01, 0xFE, 0, 1);
    CASES("unmapped", {{0xD400, 0}, {0xD401, 0}, {0xD420, 0}, {0xD500, 0}, {0xD601, 0}});
    socks(0, 0x40, 0xFE, 0x42, 0xFE, 0, 1);
    CASES("disabled", {{0xD400, 0}, {0xD420, 0}});
    // $DE00 / $DF00: the socket answers there too (the u64_sid_offsets table has the bases)
    socks(3, 0xE0, 0xFE, 0xF0, 0xFE, 0, 1);
    CASES("DE00/DF00", {{0xDE00, 1}, {0xDE1F, 1}, {0xDE20, 0}, {0xDF00, 2}, {0xDF1F, 2}, {0xDF20, 0}, {0xD400, 0}});
    // both sockets on the same address: both CS low (a bus error on a real board; the app never does it)
    socks(3, 0x40, 0xFE, 0x40, 0xFE, 0, 1);
    uint32_t b0 = top->n_cs_both;
    allow_cont = 0;
    int r = probe(0xD400, true, 0x33);
    CHECK(r == 3 && top->n_cs_both == b0 + 1, "overlap of both sockets: %d, both %u", r, top->n_cs_both - b0);
    // I/O banked out: $01 = $33 shows the character ROM, no CS
    socks(3, 0x40, 0xFE, 0x42, 0xFE, 0, 1);
    dma(0x0001, true, 0x33);
    CASES("I/O off", {{0xD400, 0}, {0xD420, 0}});
    dma(0x0001, true, 0x37);
    CASES("I/O on again", {{0xD400, 1}, {0xD420, 2}});
    resume_machine();
    run_us(5000);
}

static void install_socket_irq()
{
    std::vector<uint8_t> v = load("socket.bin");
    for (size_t i = 0; i < v.size(); i++) poke(0xC000 + i, v[i]);
    poke(0xC0F0, 0); poke(0xC0F1, 0);
    poke(0x0314, 0x00); poke(0x0315, 0xC0);                 // CINV -> $C000
}
static void check_socket_ram(const char *what)
{
    int bad = 0;
    for (int r = 0; r < 25; r++) {
        if (peek(0xC100 + r) != uint8_t(r ^ 0x5A)) bad++;
        if (peek(0xC120 + r) != uint8_t(r ^ 0xA5)) bad++;
    }
    CHECK(bad == 0, "%s: %d of 50 registers read back wrong", what, bad);
}

static void test_sock_rw()
{
    socks(3, 0x40, 0xFE, 0x42, 0xFE, 0, 1);
    boot_nocart();
    pause_machine();
    install_socket_irq();
    resume_machine();
    CHECK(wait_for(0xC0F1, 4, 400), "the IRQ program does not finish its rounds");
    run_us(3000);
    pause_machine();
    check_socket_ram("CPU");
    CHECK(top->n_sid_rd > 100, "chips saw only %u reads", top->n_sid_rd);
    CHECK(top->n_sid_both > 0, "no write reached the socket and the internal SID together");
    // the app's DMA round trip on both chips
    int bad = 0;
    for (int r = 0; r < 25; r++) { dma(0xD400 + r, true, uint8_t(r * 3 + 1)); dma(0xD420 + r, true, uint8_t(r * 5 + 2)); }
    for (int r = 0; r < 25; r++) {
        if (dma(0xD400 + r) != uint8_t(r * 3 + 1)) bad++;
        if (dma(0xD420 + r) != uint8_t(r * 5 + 2)) bad++;
    }
    CHECK(bad == 0, "%d of 50 DMA round trips differ", bad);
    // the socket answers where the internal SID would also (the testbench's internal SID decodes everything)
    CHECK(dma(0xD41B) == 0x00, "read of $D41B: the chip's OSC3 is 0, got %02X", dma(0xD41B));
    resume_machine();
}

static void test_sock_split()
{
    boot_nocart();
    pause_machine();
    for (int split : {1, 3, 5}) {
        socks_split(1, split);
        int off = 0x10 << split;                            // the address bit as an offset: A5 = $20, A7 = $80, A9 = $200
        int bad = 0;
        for (int sk = 0; sk < 2; sk++) {
            int base = 0xD400 + 0x100 * sk;
            for (int r = 0; r < 25; r++) {
                dma(base + r, true, uint8_t(0x10 + r + sk));
                dma(base + off + r, true, uint8_t(0xB0 + r + sk));
            }
            for (int r = 0; r < 25; r++) {
                if (dma(base + r) != uint8_t(0x10 + r + sk)) bad++;
                if (dma(base + off + r) != uint8_t(0xB0 + r + sk)) bad++;
            }
        }
        CHECK(bad == 0, "split A%d: %d of 100 reads differ (personality A/B)", 4 + split, bad);
        CHECK(top->n_sid_a1 > 0, "split A%d: SLOT_SID_ADDR never high in a cycle", 4 + split);
    }
    resume_machine();
}

static void test_sock_detect()
{
    boot_nocart();
    pause_machine();
    socks(3, 0x40, 0xF0, 0x50, 0xF0, 0, 2);                 // the detection setup, an FPGASID in both sockets
    for (int sk = 0; sk < 2; sk++) {
        uint16_t b = 0xD400 + 0x100 * sk;
        dma(b + 25, true, 0xEE); dma(b + 26, true, 0xAB);
        uint8_t id1 = dma(b + 0), id2 = dma(b + 1);
        CHECK(id1 == 0x1D && id2 == 0xF5, "FPGASID in socket %d: %02X %02X", sk + 1, id1, id2);
    }
    socks(3, 0x40, 0xF0, 0x50, 0xF0, 0, 1);                 // a plain chip: not an FPGASID
    for (int sk = 0; sk < 2; sk++) {
        uint16_t b = 0xD400 + 0x100 * sk;
        dma(b + 25, true, 0xEE); dma(b + 26, true, 0xAB);
        uint8_t id1 = dma(b + 0), id2 = dma(b + 1);
        CHECK(!(id1 == 0x1D && id2 == 0xF5), "plain chip in socket %d looks like an FPGASID", sk + 1);
    }
    static const struct { int chip; uint8_t a, b; const char *name; } rem[] = {
        {3, 'N', 'O', "ARMSID"}, {4, 'S', 'W', "SwinSID"}};
    for (auto &c : rem) {
        socks(3, 0x40, 0xF0, 0x50, 0xF0, 0, c.chip);
        for (int sk = 0; sk < 2; sk++) {
            uint16_t b = 0xD400 + 0x100 * sk;
            dma(b + 29, true, 'S'); run_us(10); dma(b + 30, true, 'I'); run_us(10); dma(b + 31, true, 'D'); run_us(10);
            uint8_t id1 = dma(b + 27), id2 = dma(b + 28);
            CHECK(id1 == c.a && id2 == c.b, "%s in socket %d: %02X %02X", c.name, sk + 1, id1, id2);
        }
    }
    socks(3, 0x40, 0xF0, 0x50, 0xF0, 0, 1);                 // a plain chip: no id
    dma(0xD41D, true, 'S'); dma(0xD41E, true, 'I'); dma(0xD41F, true, 'D');
    CHECK(dma(0xD41B) != 'N' && dma(0xD41B) != 'S', "plain chip answers the remake handshake");
    socks(3, 0x40, 0xF0, 0x50, 0xF0, 0, 0);                 // no chip: the read completes, whatever D shows
    uint8_t open0 = dma(0xD400), open1 = dma(0xD500);
    printf("empty sockets read %02X %02X (the bench's open bus)\n", open0, open1);
    CHECK(!(open0 == 0x1D && open1 == 0xF5), "no chip looks like an FPGASID");
    resume_machine();
}

static void test_sock_de00()
{
    // the cart is armed (EasyFlash-like) and socket 1 sits at $DE00: the cart keeps IO1 (Q4), the socket sees nothing
    ef_boot(true);
    run_us(500);
    pause_machine();
    socks(1, 0xE0, 0xFE, 0x01, 0xFE, 0, 1);
    uint8_t bank0 = top->ef_bank_o;
    uint32_t w0 = top->n_sid_wr, r0 = top->n_sid_rd;
    dma(0xDE00, true, 3); run_us(1);
    CHECK(top->ef_bank_o == 3, "a socket at $DE00 took the cart's IO1 register (bank %d)", top->ef_bank_o);
    CHECK(top->n_sid_wr == w0, "the socket saw the write of $DE00 (%u) although a cart is armed", top->n_sid_wr - w0);
    dma(0xDE00); run_us(1);
    CHECK(top->n_sid_rd == r0, "the socket saw the read of $DE00 (%u) although a cart is armed", top->n_sid_rd - r0);
    dma(0xDE00, true, bank0); run_us(1);
    dma(0x8000);
    uint8_t rom_ok = dma(0x8000 + 0x10);
    CHECK(rom_ok == P(1, 0, 0x10), "ROML still served by the cart: %02X", rom_ok);
    // the socket off: IO1 is the cart's again
    socks(0, 0xE0, 0xFE, 0x01, 0xFE, 0, 1);
    dma(0xDE00, true, 5); run_us(1);
    CHECK(top->ef_bank_o == 5, "socket off: the cart's bank register is %d", top->ef_bank_o);
    resume_machine();
}

static void test_sock_pause()
{
    socks(3, 0x40, 0xFE, 0x42, 0xFE, 0, 1);
    boot_nocart();
    pause_machine();
    install_socket_irq();
    resume_machine();
    CHECK(wait_for(0xC0F1, 3, 400), "the IRQ program does not finish its rounds");
    uint32_t seed = 12345;
    for (int i = 0; i < 40; i++) {
        seed = seed * 1103515245u + 12345u;
        run_us(100 + (seed >> 16) % 3000);
        top->halt_req = 1;
        for (int n = 0; n < 100000 && !top->halted; n++) clk_tick();
        CHECK(top->halted, "no pause %d", i);
        run_us(10 + (seed >> 8) % 40);
        CHECK(top->p_phi2 == 0, "PHI2 high in the park state");
        top->halt_req = 0;
        for (int n = 0; n < 2000000 && top->halted; n++) clk_tick();
    }
    uint8_t rounds = peek(0xC0F1);
    run_us(30000);
    CHECK(peek(0xC0F1) != rounds, "the program does not go on after the pauses");
    pause_machine();
    check_socket_ram("after the pauses");
    resume_machine();
}

static void test_sock_idle()
{
    socks(3, 0x40, 0xFE, 0x42, 0xFE, 1, 1);                 // split A5 on, so K17 idles high
    boot_nocart();
    uint32_t p0 = top->n_phi2;
    run_us(2000);
    CHECK(top->n_phi2 - p0 > 1500, "PHI2 pulses %u in 2 ms with the sockets on", top->n_phi2 - p0);
    socks(0, 0x40, 0xFE, 0x42, 0xFE, 1, 1);                 // the sockets off, the split still on
    run_us(200);
    p0 = top->n_phi2;
    run_us(2000);
    CHECK(top->n_phi2 == p0, "PHI2 pulses %u with the sockets off", top->n_phi2 - p0);
}

static void test_idle()
{
    power_on(1, 0xF, 0x0, true, "cart8k.bin", 0, 0);
    bool ok = false;
    for (int k = 0; k < 80 && !ok; k++) { run_us(50000); ok = ready_shown(); }
    CHECK(ok, "C64: no READY");
    CHECK(peek(0xC100) != 0xAA, "the cart ran");
    CHECK(top->n_phi2 == 0, "PHI2 moved %u times", top->n_phi2);
    CHECK((top->slot_status & 0x3) == 1, "GAME/EXROM pins %X", top->slot_status & 3);
    CHECK(!(top->slot_status & (1u << 16)), "armed");
}

int main(int argc, char **argv)
{
    Verilated::commandArgs(argc, argv);
    if (argc < 2) { printf("usage: tb_slot idle|8k|16k|ef|ef_ro|pause|btn [bin dir] [access ticks, 8]\n"); return 2; }
    if (argc > 2) bin_dir = argv[2];
    if (argc > 3) acc = atoi(argv[3]);
    top = new Vtb_slot_top;
    std::string t = argv[1];
    if (t == "idle") test_idle();
    else if (t == "8k") test_cart(false);
    else if (t == "16k") test_cart(true);
    else if (t == "ef") { ef_boot(true); check_flash_log(true); }
    else if (t == "ef_ro") { ef_boot(false); check_flash_log(false); }
    else if (t == "pause") test_pause();
    else if (t == "lens") test_lens();
    else if (t == "lens_soak") test_lens_soak();
    else if (t == "dma_wr") test_dma_wr();
    else if (t == "lens_pause") test_lens_pause_soak();
    else if (t == "cpuport") test_cpuport();
    else if (t == "cpuport_cp") test_cpuport_cp();
    else if (t == "btn") test_btn();
    else if (t == "openbus") test_openbus();
    else if (t == "sock_dec") test_sock_dec();
    else if (t == "sock_rw") test_sock_rw();
    else if (t == "sock_split") test_sock_split();
    else if (t == "sock_detect") test_sock_detect();
    else if (t == "sock_de00") test_sock_de00();
    else if (t == "sock_pause") test_sock_pause();
    else if (t == "sock_idle") test_sock_idle();
    else { printf("unknown test %s\n", t.c_str()); return 2; }
    report();
    printf("%s %s, %d failures (%.2f ms simulated)\n", t.c_str(), fails ? "FAILED" : "PASS", fails, now_ps / 1e9);
    return fails ? 1 : 0;
}
