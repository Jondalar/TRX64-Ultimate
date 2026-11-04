// SPDX-License-Identifier: GPL-3.0-or-later
// c64_core's cartridge under Verilator (tb_cart_top.v), with the Ultimate
// app's side played here: a .crt file is laid out in the DDR2 image the way
// C64_CRT::read_crt does it (16 KB per bank, ROMH 8 KB above ROML, small
// chips mirrored to 8 KB, the image mirrored up to 4 MB), CART_TYPE comes
// from the same table as configure_cart, and the C64 is reset with it.
// The descriptor (T33) is the app's, from the same generated table
// (trx_cart_desc.h), written with CART_TYPE.
// The MIG app port is a model with random stalls and a read latency; the
// RISC-V's port (mig_arb client 0) keeps reading and writing DDR2 meanwhile.
// Flash carts: the responder is configured with the part as the app would
// (trx_io page 4), and the app's interrupt handler is played: some
// microseconds after an operation it applies it to the image (AND for a
// program, $FF over the part's sector), then invalidates the cache and ACKs.
//
//   tb_cart <crt> [options]
//     --expect HEX      $0400.. must hold these bytes once $07E7 = $AA
//     --inactive        the cartridge must have switched itself off
//     --key C@N         press the key C (one of 0-9 A-Z RETURN SPACE) after N C64 cycles, held 80000 cycles
//     --kill            then KILL (+06 b0): the cartridge must be off
//     --ddr OFF=VAL     after the run, image byte OFF (hex) must be VAL (hex)
//     --cycles N        C64 cycles to run at most (default 2000000)
//     --screen          print the text screen at the end
//     --ppm FILE        save the last frame
//     --peek            at the end, pause the machine and look at ROML, ROMH,
//                       IO1 and IO2 as the freeze session does (c64_core pk_*,
//                       cart_fill's DDR2 read: T12 §11 Q2), then read the same
//                       addresses with bus DMA cycles (Ultimax): equal where
//                       the look says the cartridge or the REU answers; the
//                       machine's clock and cartridge state do not move for it
// Exit code 0 = all checks passed.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <deque>
#include "Vtb_cart_top.h"
#include "verilated.h"
#include "trx_cart_desc.h"

static Vtb_cart_top *top;
static uint64_t now_ps, clk_next, ui_next;
static const uint64_t CLK_HALF = 15859, UI_HALF = 6667;
static uint64_t c64_clocks, wait_clocks, fills, fill_ps, fill_ps_max, fill_start;
static int fill_req_seen, fill_done_seen;
static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

// ---- DDR2 and the MIG app port --------------------------------------------------------
static const uint32_t DDR_BYTES = 0x4000000;
// the image: Gideon's 0x3C00000, 4 MB (ROM_SEL 0); EasyFlash 3 (CRT type 90): the expansion-port block 0x1000000, 8 MB (ROM_SEL 1, T51)
static uint32_t CART_BASE = 0x3C00000, CART_MAX = 0x400000;
static std::vector<uint8_t> ddr(DDR_BYTES, 0);
struct Rd { uint32_t addr; uint64_t due; };
static std::deque<Rd> rdq;
struct Wr { uint32_t addr; bool have_cmd; };
static std::deque<uint32_t> wcmd;                     // write commands waiting for data
static std::deque<std::pair<std::vector<uint8_t>, uint16_t>> wdat;  // data waiting for a command
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

// the RISC-V's port: random 32-bit accesses below the cartridge image, checked
static std::vector<uint8_t> shadow(0x10000, 0);
static bool a0_busy, a0_rd;
static uint32_t a0_off;
static uint64_t a0_count;
static int a0_seen;
static void a0_step()        // on the C64 clock (any clock does for the toggles)
{
    if (a0_busy) {
        if (top->a0_done_tgl != a0_seen) {
            a0_seen = top->a0_done_tgl;
            a0_busy = false;
            a0_count++;
            if (a0_rd) {
                uint32_t want = 0;
                for (int b = 0; b < 4; b++) want |= uint32_t(shadow[a0_off + b]) << (8 * b);
                CHECK(top->a0_rdata == want, "RISC-V read %X: %08X, want %08X", a0_off, top->a0_rdata, want);
            }
        }
        return;
    }
    if (rand32() % 64) return;
    a0_off = (rand32() % 0x4000) * 4;
    a0_rd = rand32() & 1;
    uint32_t byte = 0x100000 + a0_off;
    top->a0_addr = byte / 2;
    top->a0_we = !a0_rd;
    uint32_t v = rand32();
    top->a0_wdata = v;
    if (!a0_rd) for (int b = 0; b < 4; b++) shadow[a0_off + b] = uint8_t(v >> (8 * b));
    top->a0_tgl = !top->a0_tgl;
    a0_busy = true;
}

// ---- flash parts and the app's handler ------------------------------------------------
static const trx_flash_part_t *part;     // from the type's descriptor entry, as the app has it
static int evt_seen;
static uint64_t evt_due;
static int flash_ops;

static uint32_t flash_img(uint32_t a, int chip)   // the app's image: 16 KB per bank
{
    // EasyFlash 3: the chip's order {slot, bank_hi, ROMH, bank_lo, A12-A0} -> {slot, bank_hi, bank_lo, ROMH, A12-A0}
    if (part->linear == 2) return (((a >> 17) & 0x3F) << 17) | (((a >> 13) & 7) << 14) | (((a >> 16) & 1) << 13) | (a & 0x1FFF);
    if (part->linear) return a;
    return ((a >> 13) << 14) | (part->chips == 2 && chip ? 0x2000 : 0) | (a & 0x1FFF);
}

static void flash_sector(uint32_t a, uint32_t &start, uint32_t &size)
{
    start = 0; size = part->chip_size;
    for (int r = 0; r < part->runs; r++)
        for (int n = 0; n < part->run[r].count; n++) {
            if (a < start + part->run[r].size) { size = part->run[r].size; return; }
            start += part->run[r].size;
        }
}

static void flash_step()     // on the C64 clock
{
    if (top->evt_tgl != evt_seen) {
        evt_seen = top->evt_tgl;
        evt_due = c64_clocks + 32 * 8;             // the handler takes some microseconds
    }
    if (!evt_due || c64_clocks < evt_due) return;
    evt_due = 0;
    uint64_t e = top->evt_word;
    uint32_t cmd = (e >> 30) & 3, chip = (e >> 29) & 1, a = ((e >> 8) & 0x1FFFFF) | (uint32_t((e >> 32) & 3) << 21), d = e & 0xFF;
    uint8_t *img = &ddr[CART_BASE];
    a &= part->chip_size - 1;
    if (cmd == 1) {
        img[flash_img(a, chip)] &= d;
    } else if (cmd == 2 || cmd == 3) {
        uint32_t start = 0, size = part->chip_size;
        if (cmd == 2) flash_sector(a, start, size);
        for (uint32_t i = start; i < start + size; i++) img[flash_img(i, chip)] = 0xFF;
    }
    flash_ops++;
    top->inval_tgl = !top->inval_tgl;
    top->fl_ack_tgl = !top->fl_ack_tgl;
}

// ---- clocks ---------------------------------------------------------------------------
static int frame_y = -1;
enum { FW = 512, FH = 320 };
static uint8_t frame[FH][FW];

static uint64_t c2_next = ~0ull;
static void step()
{
    uint64_t t = std::min(clk_next, ui_next);
    if (c2_next < t) t = c2_next;
    now_ps = t;
    // clk2x (the SID engine's, T29's CPU and RAM port): rises with every clk edge, falls halfway
    if (c2_next == t) { top->clk2x = 0; c2_next = ~0ull; top->eval(); }
    if (ui_next == t) {
        if (!top->ui_clk) mig_edge();
        else { top->ui_clk = 0; top->eval(); }
        ui_next += UI_HALF;
    }
    if (clk_next == t) {
        top->clk = !top->clk;
        c2_next = clk_next + CLK_HALF / 2; top->clk2x = 1;
        top->eval();
        if (top->clk) {
            c64_clocks++;
            if (top->wait_cpu) wait_clocks++;
            if (top->fill_req != fill_req_seen) { fill_req_seen = top->fill_req; fill_start = now_ps; }
            if (top->fill_done != fill_done_seen) {
                fill_done_seen = top->fill_done;
                uint64_t d = now_ps - fill_start;
                fills++; fill_ps += d;
                if (d > fill_ps_max) fill_ps_max = d;
            }
            a0_step();
            if (part) flash_step();
            if (top->pix_y < FH && top->pix_x < FW) frame[top->pix_y][top->pix_x] = top->pix_color;
        }
        clk_next += CLK_HALF;
    }
}
static void clocks(uint64_t n) { uint64_t e = c64_clocks + n; while (c64_clocks < e) step(); }

static uint8_t peek(uint16_t a) { top->dbg_addr = a; clocks(3); return top->dbg_rdata; }

// ---- the lens unit (dma_win LK, T21 §3.4.1) and the bus DMA to compare with ------
// LK_RES: b7:0 data, b8 done, b9 hit, b10 ext, b11 real, b12 refused, b31 busy
static uint32_t lk(uint16_t a, int cls, int real)
{
    top->lk_req = a | (cls << 16) | (real << 18);
    top->lk_go = 1;
    clocks(1);
    top->lk_go = 0;
    int n = 0;
    clocks(1);
    while ((top->lk_res >> 31) && n++ < 8000) clocks(1);
    CHECK(!(top->lk_res >> 31), "lens request $%04X cls %d real %d did not finish", a, cls, real);
    return top->lk_res;
}
// the look as the freeze session has it. cls 0 ROML, 1 ROMH, 2 IO1, 3 IO2; -1: nothing answers
static int look(uint16_t a, int cls)
{
    uint32_t r = lk(a, cls, 0);
    CHECK(r & 0x100, "look at $%04X: no result", a);
    CHECK(!(r & 0x1800), "look at $%04X: real or refused (%08X)", a, r);
    if (!(r & 0x200)) return -1;
    return r & 0xFF;
}
// the same with a bus cycle and the select forced, -1 when refused
static int real(uint16_t a, int cls)
{
    // an emulated cartridge's cache: a bus cycle is not held for a line not cached yet (only the
    // CPU is, cart_slot.v), so the first read fetches it; on a real port there is nothing to fetch
    lk(a, cls, 1);
    clocks(400);
    uint32_t r = lk(a, cls, 1);
    if (r & 0x1000) return -1;
    CHECK((r & 0x900) == 0x900, "real read at $%04X: %08X", a, r);

    return r & 0xFF;
}
static uint8_t dma_read(uint16_t a)
{
    top->dma_addr = a; top->dma_req = 1;
    clocks(1);
    top->dma_req = 0;
    for (int n = 0; n < 4000 && !top->dma_done; n++) clocks(1);
    CHECK(top->dma_done, "DMA $%04X not done", a);
    uint8_t v = top->dma_rdata;
    clocks(1);
    return v;
}
static void peek_test()
{
    // the look needs no pause (the machine runs: the answer is the model's of that clock);
    // a REAL cycle does (refused), and neither disturbs the other
    {
        uint32_t r = lk(0x8000, 0, 0);
        CHECK((r & 0x100) && !(r & 0x1800), "look while running: %08X", r);
        r = lk(0x8000, 0, 1);
        CHECK((r & 0x100) && (r & 0x1000) && !(r & 0x800), "REAL while running must be refused: %08X", r);
    }
    top->halt = 1;
    for (int n = 0; n < 4000 && !top->halted; n++) clocks(1);
    CHECK(top->halted, "the machine did not stop");
    clocks(64);
    struct P { uint16_t a; int cls; };
    static const P ps[] = {
        {0x8000, 0}, {0x8001, 0}, {0x8123, 0}, {0x9FFF, 0}, {0xE000, 1}, {0xE001, 1}, {0xF456, 1}, {0xFFFF, 1},
        {0xDF00, 3}, {0xDF01, 3}, {0xDF02, 3}, {0xDF0F, 3}, {0xDF80, 3}, {0xDFFF, 3},
        {0xDE00, 2}, {0xDE01, 2}, {0xDE02, 2}, {0xDE80, 2},
    };
    const int n = sizeof ps / sizeof ps[0];
    int lkv[n];
    uint64_t f0 = fills;
    for (int i = 0; i < n; i++) lkv[i] = look(ps[i].a, ps[i].cls);
    uint64_t f1 = fills;
    bool act = top->cart_active;
    // the same reads as bus cycles, IO1 last (some carts bank on an IO1 read)
    top->ultimax = 1;
    clocks(4);
    int same = 0, cmp = 0, open = 0;
    std::string line;
    char b[16];
    for (int i = 0; i < n; i++) {
        // a bus DMA cycle is not held for a line not cached yet (only the
        // CPU is, cart_slot.v): the first read fetches the line, the second
        // one after it reads the byte
        dma_read(ps[i].a);
        clocks(400);
        uint8_t v = dma_read(ps[i].a);
        if (lkv[i] < 0) { open++; snprintf(b, sizeof b, " %04X:--/%02X", ps[i].a, v); line += b; continue; }
        cmp++;
        if (v == lkv[i]) same++;
        else printf("FAIL: look at $%04X (class %d) %02X, bus read %02X\n", ps[i].a, ps[i].cls, lkv[i], v);
        snprintf(b, sizeof b, " %04X:%02X", ps[i].a, lkv[i]); line += b;
    }
    top->ultimax = 0;
    clocks(4);
    // the lens unit's bus cycle with the select forced: the look's answer where the cartridge answers
    // (ultimax stays off for it: the unit drops the app's), at the asked address
    int rsame = 0, rcmp = 0;
    for (int i = 0; i < n; i++) {
        int want = look(ps[i].a, ps[i].cls);
        if (want < 0) continue;
        int v = real(ps[i].a, ps[i].cls);
        rcmp++;
        if (v == want) rsame++;
        else printf("FAIL: REAL read at $%04X (class %d) %02X, look %02X\n", ps[i].a, ps[i].cls, v, want);
    }
    printf("real: %d of %d equal to the look\n", rsame, rcmp);
    fails += rcmp - rsame;
    // ROMH asked at $A000 + offset: a PLA with the cartridge in 8K or Ultimax mode would not select it
    // there; the forced select does, and the image offset only has A12-A0 in it
    int asame = 0, acmp = 0;
    for (uint16_t off : {0x0000, 0x0001, 0x0456, 0x1FFF}) {
        int want = look(0xE000 + off, 1);
        if (want < 0) continue;
        int lv = look(0xA000 + off, 1), rv = real(0xA000 + off, 1);
        acmp++;
        if (lv == want && rv == want) asame++;
        else printf("FAIL: ROMH at $%04X: look %02X, REAL %02X, ROMH at $%04X look %02X\n", 0xA000 + off, lv, rv,
                    0xE000 + off, want);
    }
    printf("romh $A000: %d of %d equal to $E000\n", asame, acmp);
    fails += acmp - asame;
    printf("look:%s\n", line.c_str());
    printf("look: %d of %d equal to bus reads, %d with nothing answering; no cache fetch for it (%llu)\n",
           same, cmp, open, (unsigned long long)(f1 - f0));
    fails += cmp - same;
    CHECK(f1 == f0 && act == top->cart_active, "the look changed the cartridge");
    top->halt = 0;
    for (int k = 0; k < 2000000 && top->halted; k++) clocks(1);   // resumes after whole frames
}

// ---- the app's side ------------------------------------------------------------------
static uint16_t be16(const uint8_t *p) { return (p[0] << 8) | p[1]; }
static uint32_t be32(const uint8_t *p) { return (uint32_t(p[0]) << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }

// C64_CRT::read_crt + configure_cart (software/io/c64/c64_crt.cc), C64 carts
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
    int hw = be16(&file[0x16]);
    if (hw == 90) { CART_BASE = 0x1000000; CART_MAX = 0x800000; top->rom_sel = 1; }
    uint8_t *img = &ddr[CART_BASE];
    memset(img, 0xFF, CART_MAX);
    int exrom = file[0x18], game = file[0x19], subtype = file[0x1A];
    uint32_t pos = be32(&file[0x10]);
    if (pos < 0x40) pos = 0x40;
    int highest = 0;
    uint32_t total = 0;
    bool a000 = false;
    while (pos + 16 <= file.size() && !memcmp(&file[pos], "CHIP", 4)) {
        uint32_t len = be32(&file[pos + 4]);
        int type = be16(&file[pos + 8]), bank = be16(&file[pos + 10]);
        int load = be16(&file[pos + 12]), size = be16(&file[pos + 14]);
        if (load == 0xA000) a000 = true;
        uint32_t off = uint32_t(bank) * 0x4000 + (load & 0x2000);
        if (size && (type == 0 || type == 2) && load != 0xDE00 && off < CART_MAX) {
            memcpy(img + off, &file[pos + 16], size);
            total += size;
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
    switch (hw) {
    case 0:  t = (exrom == 1 && game == 0) ? 0xA1 : (exrom == 0 && game == 0) ? 0x01 : (exrom == 0 && game == 1) ? 0x41 : 0; break;
    case 1:  t = 0x1B; break;                                   // Action Replay
    case 2:  t = 0x1C; break;                                   // KCS
    case 3:  t = 0x19 | (total > 65536 ? 0x20 : 0); break;     // Final Cartridge III
    case 4:  t = 0x05; break;                                   // Simons Basic
    case 5:  t = a000 ? (0x09 | 0x60) : 0x08; break;            // Ocean
    case 8:  t = 0x0B; break;                                   // Super Games
    case 9:  t = 0x1B | 0x40; break;                            // Atomic Power (Nordic)
    case 10: t = 0x02; break;                                   // Epyx FastLoad
    case 11: t = 0x04 | 0x40; break;                            // Westermann
    case 13: t = 0x18; break;                                   // Final Cartridge I
    case 15: t = 0x0A; break;                                   // C64 Game System
    case 18: t = 0x0D; break;                                   // Zaxxon
    case 19: t = 0x08 | 0x20; break;                            // Magic Desk, Domark
    case 20: t = 0x1A | (total > 65536 ? 0x20 : 0); break;     // Super Snapshot 5
    case 21: t = subtype == 1 ? (0x09 | 0x40) : total > 65536 ? (0x09 | 0x20) : 0x09; break;  // COMAL 80
    case 17: t = 0x88; break;                                   // Dinamic
    case 32: t = 0x11; break;                                   // EasyFlash
    case 36: t = 0x1B | 0x20; break;                            // Retro Replay
    case 60: t = 0x48; break;                                   // GMod2
    case 85: t = 0xA8; break;                                   // Magic Desk 16K
    case 86: t = 0x0F; break;                                   // MegaByter
    case 88: t = 0x68; break;                                   // C64MegaCart
    case 89: t = 0x2F; break;                                   // TwoMegabyter
    case 90:  t = 0x12; break;                                  // EasyFlash 3
    case 232: t = 0x31; break;                                  // EasyFlashXl
    default: printf("CRT type %d not in this table\n", hw); exit(2);
    }
    if (hw == 18) memcpy(img + 0x4000, img, 0x2000);           // Zaxxon: first chip in both banks
    // trx_io page 4, as the app sets it (trx_cart_configure): the descriptor, the flash part
    const trx_cart_desc_t *e = nullptr;
    for (auto &c : c_cart_descs) if (c.cart_type == t) e = &c;
    for (int w = 0; w < 10; w++) top->cart_desc[w] = 0;
    if (e) for (int b = 0; b < TRX_CART_DESC_BYTES; b++) top->cart_desc[b / 4] |= uint32_t(e->desc[b]) << (8 * (b % 4));
    else printf("no descriptor for CART_TYPE $%02X: the cartridge stays off\n", t);
    part = e ? e->flash : nullptr;
    if (part) {
        top->fl_magic1 = part->magic1; top->fl_magic2 = part->magic2; top->fl_mask = part->mask;
        top->fl_id = part->id_mfr | (part->id_dev << 8) | (part->id_ofs << 16);
        top->fl_enable = 1;
    }
    printf("%s: CRT type %d, EXROM %d GAME %d, %u bytes, CART_TYPE $%02X\n", name, hw, exrom, game, total, t);
    return t;
}

// C64 matrix positions as col * 8 + row (CIA 1 PA column, PB row), as sim/input has them
static int key_of(char c)
{
    static const char *cols[8] = {
        "\x7f\r\0\0\0\0\0\0", "3WA4ZSE\0", "5RD6CFTX", "7YG8BHUV", "9IJ0MKON", "+PL-.:@,", "\0*;\0\0=\0/", "1\0\0" "2 \0Q\0" };
    for (int col = 0; col < 8; col++)
        for (int r = 0; r < 8; r++)
            if (cols[col][r] == c && c) return col * 8 + r;
    return -1;
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

int main(int argc, char **argv)
{
    Verilated::commandArgs(argc, argv);
    if (argc < 2) { printf("usage: tb_cart <crt> [--expect HEX] [--ddr OFF=VAL] [--cycles N] [--screen] [--ppm F]\n"); return 2; }
    std::vector<uint8_t> expect;
    std::vector<std::pair<uint32_t, uint8_t>> ddr_checks;
    uint64_t max_cycles = 2000000;
    bool screen = false, inactive = false, kill = false, peek_end = false;
    std::vector<std::pair<uint64_t, int>> presses;     // C64 cycle, key index
    const char *ppm = nullptr;
    for (int i = 2; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--expect" && i + 1 < argc) {
            const char *h = argv[++i];
            for (size_t k = 0; k + 1 < strlen(h); k += 2) expect.push_back(uint8_t(strtoul(std::string(h + k, 2).c_str(), 0, 16)));
        } else if (a == "--ddr" && i + 1 < argc) {
            unsigned off, val;
            sscanf(argv[++i], "%x=%x", &off, &val);
            ddr_checks.push_back({off, uint8_t(val)});
        } else if (a == "--cycles" && i + 1 < argc) max_cycles = strtoull(argv[++i], 0, 0);
        else if (a == "--key" && i + 1 < argc) {
            const char *k = argv[++i];
            int key = key_of(k[0]);
            if (key < 0) { printf("--key: no key for %c\n", k[0]); return 2; }
            presses.push_back({strtoull(k + 2, 0, 0), key});
        }
        else if (a == "--screen") screen = true;
        else if (a == "--inactive") inactive = true;
        else if (a == "--kill") kill = true;
        else if (a == "--peek") peek_end = true;
        else if (a == "--ppm" && i + 1 < argc) ppm = argv[++i];
    }

    top = new Vtb_cart_top;
    for (uint32_t i = 0; i < 0x10000; i++) shadow[i] = ddr[0x100000 + i] = uint8_t(rand32());
    int type = load_crt(argv[1]);

    // power on: everything in reset
    top->rst = 1; top->ui_rst = 1; top->cpu_reset_req = 1; top->cart_type = 0;
    top->app_rdy = 1; top->app_wdf_rdy = 1; top->app_rd_data_valid = 0;
    clk_next = CLK_HALF; ui_next = UI_HALF;
    clocks(64);
    top->rst = 0; top->ui_rst = 0;
    clocks(2000);
    // C64::start_cartridge: reset, CART_TYPE, unreset
    top->cart_type = type;
    clocks(32 * 100);
    top->cpu_reset_req = 0;

    uint64_t start = c64_clocks;
    for (auto &pr : presses) {                              // keys: the menu of the EasyFlash 3, for one
        while (c64_clocks - start < pr.first * 32) clocks(32 * 1000);
        top->keys = 1ull << pr.second;
        clocks(32 * 80000);
        top->keys = 0;
    }
    bool done = expect.empty();
    while (c64_clocks - start < max_cycles * 32) {
        clocks(32 * 2000);
        if (!expect.empty() && peek(0x07E7) == 0xAA) { done = true; break; }
    }
    uint64_t cyc = (c64_clocks - start) / 32;
    printf("ran %llu cycles (%.3f s), CPU held %llu cycles for the cache, RISC-V port %llu accesses\n",
           (unsigned long long)cyc, cyc / 985248.0, (unsigned long long)(wait_clocks / 32),
           (unsigned long long)a0_count);
    if (fills)
        printf("%llu DDR2 requests (line fetches, writes), %.2f us on average, %.2f us at most\n", (unsigned long long)fills,
               fill_ps / 1e6 / fills, fill_ps_max / 1e6);
    if (!expect.empty()) {
        CHECK(done, "no end marker at $07E7");
        std::string got, want;
        char b[4];
        for (size_t i = 0; i < expect.size(); i++) {
            uint8_t v = peek(0x0400 + i);
            snprintf(b, sizeof b, "%02X", v); got += b;
            snprintf(b, sizeof b, "%02X", expect[i]); want += b;
            if (v != expect[i]) { fails++; printf("FAIL: $%04zX = $%02X, want $%02X\n", 0x400 + i, v, expect[i]); }
        }
        printf("results %s\n", got.c_str());
    }
    for (auto &c : ddr_checks) {
        uint8_t v = ddr[CART_BASE + c.first];
        if (v != c.second) { fails++; printf("FAIL: image $%06X = $%02X, want $%02X\n", c.first, v, c.second); }
    }
    if (peek_end) peek_test();
    if (inactive) CHECK(!top->cart_active, "the cartridge is still active");
    if (kill) {
        CHECK(top->cart_active, "the cartridge is not active before KILL");
        top->cart_kill_tgl = !top->cart_kill_tgl;
        clocks(100);
        CHECK(!top->cart_active, "the cartridge is still active after KILL");
    }
    if (screen) for (int r = 0; r < 25; r++) { std::string s = row(r); if (!s.empty()) printf("  | %s\n", s.c_str()); }
    if (ppm) save_ppm(ppm);
    if (part) printf("%d flash operations applied\n", flash_ops);
    printf("cartridge active %d\n", top->cart_active);
    printf(fails ? "FAILED\n" : "PASS\n");
    delete top;
    return fails ? 1 : 0;
}
