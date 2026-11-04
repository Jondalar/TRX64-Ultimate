/*
 * carts.c - behavioural models of the cartridges Alex has (and the C64MegaCart and EasyFlash 3 of the spec's
 * matrix): plain 16K ROM, EasyFlash 1 (2 x AM29F040), EasyFlash 3 (MX29LV640 slots), MegaByter (MX29F800),
 * GMod2 (AM29F040 + M93C86), C64MegaCart (M29F160FT). The flash is the textbook AMD state machine; what a
 * cartridge decodes follows the T16 bench (tb_slot_top.v) and tests/cart/t16_sim.py.
 */
#include "mock.h"
#include <stdlib.h>
#include <string.h>

// ---- AMD flash -------------------------------------------------------------------------------------------
typedef struct {
    uint8_t *d;
    uint32_t size;
    uint32_t cmask;     // address bits a command cycle compares
    uint32_t a1, a2;    // unlock addresses
    uint32_t sector;    // sector size
    int state;          // 0 read, 1 AA, 2 55, 3 prog, 4 80, 5 80 AA, 6 80 AA 55, 7 autoselect
    int busy;           // status reads left
    uint8_t busy_val;
    int busy_kind;      // 0 none, 1 program, 2 erase
    uint32_t op_addr;
    uint8_t op_val;
    uint32_t op_end;    // erase range
    int toggle;
    int boot8k;         // bottom boot: the first 64K are eight 8K sectors (MX29LV640EB)
} flash_t;

static void flash_init(flash_t *f, uint32_t size, uint32_t cmask, uint32_t a1, uint32_t a2, uint32_t sector)
{
    f->d = malloc(size);
    memset(f->d, 0xFF, size);
    f->size = size;
    f->cmask = cmask;
    f->a1 = a1;
    f->a2 = a2;
    f->sector = sector;
    f->state = 0;
    f->busy = 0;
}

static void flash_finish(flash_t *f)
{
    if (f->busy_kind == 1)
        f->d[f->op_addr] &= f->op_val;
    else if (f->busy_kind == 2)
        memset(f->d + f->op_addr, 0xFF, f->op_end - f->op_addr);
    f->busy_kind = 0;
    f->busy = 0;
}

static uint8_t flash_read(flash_t *f, uint32_t a, int broken)
{
    if (broken)
        return 0x20; // DQ5: the chip reports a failed operation
    if (f->busy > 0) {
        f->busy--;
        f->toggle ^= 0x40;
        uint8_t v = (f->busy_kind == 1) ? (uint8_t)(~f->op_val & 0x80) : 0x00;
        v |= (uint8_t)f->toggle;
        if (f->busy == 0)
            flash_finish(f);
        return v;
    }
    if (f->state == 7)
        return (a & 1) ? 0xA4 : 0x01;
    return f->d[a % f->size];
}

static void flash_write(flash_t *f, uint32_t a, uint8_t v, int broken)
{
    if (broken)
        return;
    uint32_t m = a & f->cmask;
    if (v == 0xF0 && f->state != 3) {
        f->state = 0;
        return;
    }
    switch (f->state) {
    case 0:
    case 7:
        if (m == f->a1 && v == 0xAA)
            f->state = 1;
        break;
    case 1:
        f->state = (m == f->a2 && v == 0x55) ? 2 : 0;
        break;
    case 2:
        if (m != f->a1)
            f->state = 0;
        else if (v == 0xA0)
            f->state = 3;
        else if (v == 0x80)
            f->state = 4;
        else if (v == 0x90)
            f->state = 7;
        else
            f->state = 0;
        break;
    case 3:
        f->op_addr = a % f->size;
        f->op_val = v;
        f->busy_kind = 1;
        f->busy = 2;
        f->state = 0;
        break;
    case 4:
        f->state = (m == f->a1 && v == 0xAA) ? 5 : 0;
        break;
    case 5:
        f->state = (m == f->a2 && v == 0x55) ? 6 : 0;
        break;
    case 6:
        if (v == 0x30) {
            uint32_t sec = (f->boot8k && (a % f->size) < 0x10000) ? 0x2000 : f->sector;
            f->op_addr = (a % f->size) / sec * sec;
            f->op_end = f->op_addr + sec;
            f->busy_kind = 2;
            f->busy = 3;
        } else if (v == 0x10 && m == f->a1) {
            f->op_addr = 0;
            f->op_end = f->size;
            f->busy_kind = 2;
            f->busy = 3;
        }
        f->state = 0;
        break;
    }
}

// ---- pattern ------------------------------------------------------------------------------------------------
static void fill_bytes(uint8_t *p, uint32_t n, uint32_t seed)
{
    uint32_t x = seed * 2654435761u + 12345u;
    for (uint32_t i = 0; i < n; i++) {
        x = x * 1664525u + 1013904223u;
        p[i] = (uint8_t)(x >> 24);
    }
}

// ---- ROM16 ---------------------------------------------------------------------------------------------------
typedef struct { uint8_t d[0x4000]; } rom16_t;
static int rom16_pins(cart_t *c) { (void)c; return 0; } // GAME and EXROM low: 16K
static int rom16_io_rd(cart_t *c, uint16_t a, uint8_t *v) { (void)c; (void)a; (void)v; return 0; }
static void rom16_io_wr(cart_t *c, uint16_t a, uint8_t v) { (void)c; (void)a; (void)v; }
static int rom16_rom_rd(cart_t *c, int half, uint16_t off, uint8_t *v)
{
    rom16_t *r = c->priv;
    *v = r->d[half * 0x2000 + off];
    return 1;
}
static void rom16_rom_wr(cart_t *c, int half, uint16_t off, uint8_t v) { (void)c; (void)half; (void)off; (void)v; }
static void rom16_reset(cart_t *c) { (void)c; }
static uint8_t rom16_bank_byte(cart_t *c, int bank, int half, int off)
{
    rom16_t *r = c->priv;
    (void)bank;
    return r->d[half * 0x2000 + off];
}

cart_t *cart_rom16(void)
{
    cart_t *c = calloc(1, sizeof(*c));
    rom16_t *r = calloc(1, sizeof(*r));
    fill_bytes(r->d, sizeof(r->d), 16);
    memcpy(r->d + 4, "CBM80", 5);
    *c = (cart_t){ "rom16", r, rom16_pins, rom16_io_rd, rom16_io_wr, rom16_rom_rd, rom16_rom_wr, rom16_reset,
                   rom16_bank_byte, NULL, 1, 0, false };
    return c;
}

// ---- EasyFlash 1 and 3 ----------------------------------------------------------------------------------------
typedef struct {
    int ef3;
    flash_t roml, romh; // EF1: two 512K chips; EF3: roml is the 8 MB chip
    uint8_t bank, mode, slot;
    uint8_t ram[256];
} ef_t;

static int ef_game_low(ef_t *e) { return (e->mode & 4) ? (e->mode & 1) : 1; }
static int ef_exrom_low(ef_t *e) { return (e->mode & 2) != 0; }
static int ef_pins(cart_t *c)
{
    ef_t *e = c->priv;
    return (!ef_game_low(e) ? 1 : 0) | (!ef_exrom_low(e) ? 2 : 0);
}
static int ef_io_rd(cart_t *c, uint16_t a, uint8_t *v)
{
    ef_t *e = c->priv;
    if (a >= 0xDF00) {
        *v = e->ram[a & 0xFF];
        return 1;
    }
    if (e->ef3 && a == 0xDE01) { // the slot register reads back
        *v = e->slot & 7;
        return 1;
    }
    return 0; // the bank and mode registers are write-only
}
static void ef_io_wr(cart_t *c, uint16_t a, uint8_t v)
{
    ef_t *e = c->priv;
    if (a >= 0xDF00)
        e->ram[a & 0xFF] = v;
    else if (a == 0xDE00)
        e->bank = v & 0x3F;
    else if (a == 0xDE02)
        e->mode = v & 0x87;
    else if (e->ef3 && a == 0xDE01)
        e->slot = v & 7;
    else if (!e->ef3 && a == 0xDE01)
        e->bank = v & 0x3F; // an EF1 takes DE01 like DE00's mirror
}
static uint32_t ef3_idx(ef_t *e, int half, uint16_t off)
{
    return (half ? 0x400000u : 0u) + (((uint32_t)e->slot * 64 + e->bank) * 0x2000u) + off;
}
static int ef_rom_rd(cart_t *c, int half, uint16_t off, uint8_t *v)
{
    ef_t *e = c->priv;
    if (e->ef3)
        *v = flash_read(&e->roml, ef3_idx(e, half, off), c->broken);
    else
        *v = flash_read(half ? &e->romh : &e->roml, (uint32_t)e->bank * 0x2000u + off, c->broken);
    return 1;
}
static void ef_rom_wr(cart_t *c, int half, uint16_t off, uint8_t v)
{
    ef_t *e = c->priv;
    if (e->ef3) {
        // MX29LV640: the command cycles compare the low 12 bits; sectors are 64K of ROML or ROMH
        flash_write(&e->roml, ef3_idx(e, half, off), v, c->broken);
    } else {
        flash_write(half ? &e->romh : &e->roml, (uint32_t)e->bank * 0x2000u + off, v, c->broken);
    }
}
static void ef_reset(cart_t *c)
{
    ef_t *e = c->priv;
    e->bank = 0;
    e->mode = 0;
    e->slot = 0;
    e->roml.state = e->romh.state = 0;
}
static uint8_t ef_bank_byte(cart_t *c, int bank, int half, int off)
{
    ef_t *e = c->priv;
    if (e->ef3)
        return e->roml.d[(half ? 0x400000u : 0u) + ((uint32_t)c->view_slot * 64u + (uint32_t)bank) * 0x2000u + (uint32_t)off];
    return (half ? &e->romh : &e->roml)->d[(uint32_t)bank * 0x2000u + (uint32_t)off];
}

static cart_t *ef_new(int ef3)
{
    cart_t *c = calloc(1, sizeof(*c));
    ef_t *e = calloc(1, sizeof(*e));
    e->ef3 = ef3;
    if (ef3) {
        flash_init(&e->roml, 8u << 20, 0xFFF, 0xAAA, 0x555, 0x10000);
        e->roml.boot8k = 1;
    } else {
        flash_init(&e->roml, 512 * 1024, 0x7FF, 0x555, 0x2AA, 0x10000);
        flash_init(&e->romh, 512 * 1024, 0x7FF, 0x555, 0x2AA, 0x10000);
    }
    *c = (cart_t){ ef3 ? "ef3" : "ef1", e, ef_pins, ef_io_rd, ef_io_wr, ef_rom_rd, ef_rom_wr, ef_reset, ef_bank_byte,
                   NULL, 64, 32, false };
    return c;
}
cart_t *cart_ef1(void) { return ef_new(0); }
cart_t *cart_ef3(void) { return ef_new(1); }

// ---- single-chip carts: MegaByter, C64MegaCart ------------------------------------------------------------------
typedef struct {
    int kind;       // 0 MegaByter, 1 C64MegaCart
    flash_t f;
    uint8_t bank;
    uint8_t mode;   // MegaByter $DE02, MegaCart $DF00
} one_t;

static int one_ultimax(one_t *o) { return o->kind == 0 ? (o->mode & 3) == 3 : (o->mode & 0xC0) == 0xC0; }
static int one_pins(cart_t *c)
{
    one_t *o = c->priv;
    return one_ultimax(o) ? 2 /* GAME low, EXROM high */ : 1 /* EXROM low, GAME high */;
}
static int one_io_rd(cart_t *c, uint16_t a, uint8_t *v) { (void)c; (void)a; (void)v; return 0; }
static void one_io_wr(cart_t *c, uint16_t a, uint8_t v)
{
    one_t *o = c->priv;
    if (a == 0xDE00)
        o->bank = (uint8_t)(o->kind == 0 ? v & 0x7F : v);
    else if (o->kind == 0 && a == 0xDE02)
        o->mode = v;
    else if (o->kind == 1 && a >= 0xDF00 && (a & 0xFF) == 0)
        o->mode = v;
}
static int one_rom_rd(cart_t *c, int half, uint16_t off, uint8_t *v)
{
    one_t *o = c->priv;
    if (o->kind == 1 && half == 1 && !one_ultimax(o))
        return 0; // in 8K mode there is no ROMH
    *v = flash_read(&o->f, (uint32_t)o->bank * 0x2000u + off, c->broken);
    return 1;
}
static void one_rom_wr(cart_t *c, int half, uint16_t off, uint8_t v)
{
    one_t *o = c->priv;
    (void)half;
    flash_write(&o->f, (uint32_t)o->bank * 0x2000u + off, v, c->broken);
}
static void one_reset(cart_t *c)
{
    one_t *o = c->priv;
    o->bank = 0;
    o->mode = 0;
    o->f.state = 0;
}
static uint8_t one_bank_byte(cart_t *c, int bank, int half, int off)
{
    one_t *o = c->priv;
    (void)half;
    return o->f.d[(uint32_t)bank * 0x2000u + (uint32_t)off];
}
static cart_t *one_new(int kind)
{
    cart_t *c = calloc(1, sizeof(*c));
    one_t *o = calloc(1, sizeof(*o));
    o->kind = kind;
    if (kind == 0)
        flash_init(&o->f, 1u << 20, 0xFFF, 0xAAA, 0x555, 0x10000);
    else
        flash_init(&o->f, 2u << 20, 0xFFF, 0xAAA, 0x555, 0x10000);
    *c = (cart_t){ kind ? "megacart" : "megabyter", o, one_pins, one_io_rd, one_io_wr, one_rom_rd, one_rom_wr,
                   one_reset, one_bank_byte, NULL, kind ? 256 : 128, kind ? 88 : 86, false };
    return c;
}
cart_t *cart_megabyter(void) { return one_new(0); }
cart_t *cart_megacart(void) { return one_new(1); }

// ---- GMod2 -----------------------------------------------------------------------------------------------------
typedef struct {
    flash_t f;
    uint8_t reg;           // $DE00 as written
    uint8_t ee[2048];
    int ee_cs, ee_clk, ee_open; // protocol
    uint32_t ee_bits;
    int ee_n;
    int ee_do;
    int ee_enabled;
    int ee_phase;          // 0 command, 1 data out, 2 write pending
    int ee_busy;
    int ee_outpos;
    uint16_t ee_word;
} gmod2_t;

// bits 7:6 of $DE00 as the writer's comments and the board show them: 11 Ultimax with the flash writable, 00 and
// 10 8K, 01 off (the EEPROM's chip select; "$DE00=64 turns the ROM off")
static int g2_mode(gmod2_t *g)
{
    int m = (g->reg >> 6) & 3;
    return m == 3 ? 3 : m == 1 ? 2 : 0; // 3 Ultimax, 2 off, 0 8K
}
static int g2_pins(cart_t *c)
{
    gmod2_t *g = c->priv;
    switch (g2_mode(g)) {
    case 3: return 2;  // Ultimax
    case 2: return 3;  // off
    default: return 1; // 8K
    }
}

static void ee_edge(gmod2_t *g, uint8_t old, uint8_t v)
{
    int cs = (v >> 6) & 1, clk = (v >> 5) & 1, di = (v >> 4) & 1;
    int ocs = (old >> 6) & 1, oclk = (old >> 5) & 1;
    if (cs && !ocs) { // CS rises: a command starts; a finished WRITE shows busy
        if (g->ee_phase == 2)
            g->ee_busy = 1;
        g->ee_bits = 0;
        g->ee_n = 0;
        g->ee_phase = 0;
        return;
    }
    if (!cs && ocs) { // CS falls: a complete WRITE executes
        if (g->ee_phase == 0 && g->ee_n == 29 && ((g->ee_bits >> 26) & 7) == 5) { // 1 01 aaaaaaaaaa dddd...
            uint32_t addr = (g->ee_bits >> 16) & 0x3FF;
            if (g->ee_enabled) {
                g->ee[2 * addr] = (uint8_t)(g->ee_bits >> 8);
                g->ee[2 * addr + 1] = (uint8_t)g->ee_bits;
            }
            g->ee_phase = 2;
            g->ee_n = 0;
        } else if (g->ee_n == 13 && ((g->ee_bits >> 10) & 7) == 4) { // 1 00 11xxxxxxxx EWEN / 00xxxxxxxx EWDS
            uint32_t a = (g->ee_bits >> 8) & 3;
            if (a == 3)
                g->ee_enabled = 1;
            else if (a == 0)
                g->ee_enabled = 0;
        }
        return;
    }
    if (cs && clk && !oclk) { // rising clock
        if (g->ee_phase == 1) {
            g->ee_do = (g->ee_word >> (15 - g->ee_outpos)) & 1;
            g->ee_outpos++;
            return;
        }
        g->ee_bits = (g->ee_bits << 1) | (uint32_t)di;
        g->ee_n++;
        if (g->ee_n == 13 && ((g->ee_bits >> 10) & 7) == 6) { // 1 10 aaaaaaaaaa: READ
            uint32_t addr = g->ee_bits & 0x3FF;
            g->ee_word = (uint16_t)((g->ee[2 * addr] << 8) | g->ee[2 * addr + 1]);
            g->ee_phase = 1;
            g->ee_outpos = 0;
        }
    }
}

static int g2_io_rd(cart_t *c, uint16_t a, uint8_t *v)
{
    gmod2_t *g = c->priv;
    if (a != 0xDE00)
        return 0;
    if (g->ee_busy > 0) { // after a WRITE: busy for one read
        g->ee_busy--;
        *v = 0x00;
        return 1;
    }
    *v = (uint8_t)(g->ee_do ? 0x80 : 0x00);
    if (g->ee_phase == 0 && g->ee_cs)
        *v = 0x80; // idle ready
    return 1;
}
static void g2_io_wr(cart_t *c, uint16_t a, uint8_t v)
{
    gmod2_t *g = c->priv;
    if (a != 0xDE00)
        return;
    ee_edge(g, g->reg, v);
    g->ee_cs = (v >> 6) & 1;
    g->reg = v;
}
static uint32_t g2_idx(gmod2_t *g, uint16_t off) { return (uint32_t)(g->reg & 0x3F) * 0x2000u + off; }
static int g2_rom_rd(cart_t *c, int half, uint16_t off, uint8_t *v)
{
    gmod2_t *g = c->priv;
    int m = g2_mode(g);
    if (m == 2)
        return 0;
    if (m == 3 && half == 0)
        ; // Ultimax: ROML and ROMH both show the flash bank
    if (m != 3 && half == 1)
        return 0;
    // the flash only reads in 8K mode; in Ultimax a read still reaches the chip (status or data)
    *v = flash_read(&g->f, g2_idx(g, off), c->broken);
    return 1;
}
static void g2_rom_wr(cart_t *c, int half, uint16_t off, uint8_t v)
{
    gmod2_t *g = c->priv;
    (void)half;
    if (g2_mode(g) != 3)
        return;
    flash_write(&g->f, g2_idx(g, off), v, c->broken);
}
static void g2_reset(cart_t *c)
{
    gmod2_t *g = c->priv;
    g->reg = 0;
    g->ee_cs = 0;
    g->ee_phase = 0;
    g->f.state = 0;
}
static uint8_t g2_bank_byte(cart_t *c, int bank, int half, int off)
{
    gmod2_t *g = c->priv;
    (void)half;
    return g->f.d[(uint32_t)bank * 0x2000u + (uint32_t)off];
}
cart_t *cart_gmod2(void)
{
    cart_t *c = calloc(1, sizeof(*c));
    gmod2_t *g = calloc(1, sizeof(*g));
    flash_init(&g->f, 512 * 1024, 0x7FF, 0x555, 0x2AA, 0x10000);
    fill_bytes(g->ee, sizeof(g->ee), 93);
    *c = (cart_t){ "gmod2", g, g2_pins, g2_io_rd, g2_io_wr, g2_rom_rd, g2_rom_wr, g2_reset, g2_bank_byte, g->ee, 64,
                   60, false };
    return c;
}

// ---- common ------------------------------------------------------------------------------------------------------
static flash_t *flashes(cart_t *c, flash_t **b)
{
    *b = NULL;
    if (c->priv == NULL)
        return NULL;
    if (!strcmp(c->name, "ef1")) {
        *b = &((ef_t *)c->priv)->romh;
        return &((ef_t *)c->priv)->roml;
    }
    if (!strcmp(c->name, "ef3"))
        return &((ef_t *)c->priv)->roml;
    if (!strcmp(c->name, "megabyter") || !strcmp(c->name, "megacart"))
        return &((one_t *)c->priv)->f;
    if (!strcmp(c->name, "gmod2"))
        return &((gmod2_t *)c->priv)->f;
    return NULL;
}

// An EasyFlash 3 with data everywhere: the system area (slot 0: KERNAL sectors in ROML of banks 0-7, the directory at bank
// $10, code in bank 0 ROMH) and eight full slots, so that a whole-cart dump and a type 90 image are not mostly $FF
void cart_ef3_fill_full(cart_t *c, uint32_t seed)
{
    ef_t *e = c->priv;
    uint8_t *d = e->roml.d;
    for (int slot = 0; slot < 8; slot++)
        for (int bank = 0; bank < 64; bank++) {
            uint32_t n = (uint32_t)(slot * 64 + bank);
            fill_bytes(d + n * 0x2000u, 0x2000, seed + n);
            fill_bytes(d + 0x400000u + n * 0x2000u, 0x2000, seed + 5000u + n);
        }
    static const uint8_t sign[16] = { 0xC5, 0xC6, 0x2D, 0xC4, 0x49, 0x52, 0x45, 0x43, 0x54, 0x4F, 0x52, 0x59, 0x20, 0xD6, 0x31, 0x3A };
    uint8_t *dir = d + 0x10u * 0x2000u;
    memcpy(dir, sign, 16);
    static const uint8_t n1[8] = { 0xC7, 0x41, 0x4D, 0x45, 0x20, 0xCF, 0x4E, 0x45 }; // "Game One" in PETSCII
    static const uint8_t n2[8] = { 0xC7, 0x41, 0x4D, 0x45, 0x20, 0xD4, 0x57, 0x4F };
    for (int slot = 1; slot < 8; slot++)
        memset(dir + 16 + slot * 16, 0, 16);
    memcpy(dir + 16 + 1 * 16, n1, 8);
    memcpy(dir + 16 + 2 * 16, n2, 8);
}

uint8_t *cart_ef3_raw(cart_t *c) { return ((ef_t *)c->priv)->roml.d; }

void cart_fill_pattern(cart_t *c, uint32_t seed)
{
    flash_t *b;
    flash_t *a = flashes(c, &b);
    if (!a) {
        return;
    }
    if (!strcmp(c->name, "ef3")) {
        // slot 1 holds a game in banks 0..63 of slot 1, ROML and ROMH; the system area (slot 0) a directory
        ef_t *e = c->priv;
        for (int s = 1; s < 3; s++)
            for (int bank = 0; bank < 64; bank++) {
                fill_bytes(a->d + (uint32_t)(s * 64 + bank) * 0x2000u, 0x2000, seed + (uint32_t)(s * 64 + bank));
                fill_bytes(a->d + 0x400000u + (uint32_t)(s * 64 + bank) * 0x2000u, 0x2000, seed + 777u + (uint32_t)(s * 64 + bank));
                a->d[(uint32_t)(s * 64 + bank) * 0x2000u] = (uint8_t)(bank + 1 + s);
            }
        (void)e;
        // EF-Directory in slot 0, bank $10, ROML
        static const uint8_t sign[16] = { 0xC5, 0xC6, 0x2D, 0xC4, 0x49, 0x52, 0x45, 0x43, 0x54, 0x4F, 0x52, 0x59, 0x20, 0xD6, 0x31, 0x3A };
        uint8_t *dir = a->d + 0x10u * 0x2000u;
        memcpy(dir, sign, 16);
        static const uint8_t n1[8] = { 0xC7, 0x41, 0x4D, 0x45, 0x20, 0xCF, 0x4E, 0x45 }; // "Game One" in PETSCII
        static const uint8_t n2[8] = { 0xC7, 0x41, 0x4D, 0x45, 0x20, 0xD4, 0x57, 0x4F };
        memset(dir + 16 + 1 * 16, 0, 16);
        memset(dir + 16 + 2 * 16, 0, 16);
        memcpy(dir + 16 + 1 * 16, n1, 8);
        memcpy(dir + 16 + 2 * 16, n2, 8);
        return;
    }
    int banks = c->banks;
    for (int bank = 0; bank < banks; bank++) {
        // a few banks stay erased: the dump marks them empty, the write tests use them
        if (bank > 5 && (bank % 7) == 0)
            continue;
        fill_bytes(a->d + (uint32_t)bank * 0x2000u, 0x2000, seed + (uint32_t)bank);
        a->d[(uint32_t)bank * 0x2000u] = (uint8_t)(bank + 1);
        if (b) {
            fill_bytes(b->d + (uint32_t)bank * 0x2000u, 0x2000, seed + 5000u + (uint32_t)bank);
        }
    }
    // an EasyFlash keeps its EAPI where the writer replaces it
}

void cart_free(cart_t *c)
{
    flash_t *b;
    flash_t *a = flashes(c, &b);
    if (a)
        free(a->d);
    if (b)
        free(b->d);
    if (c->priv)
        free(c->priv);
    free(c);
}
