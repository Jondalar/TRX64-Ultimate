// The address and sector geometry of the flash parts the TRX64 bitstream emulates (trx_cart_desc.h), as
// pure functions: trx_cart.cc applies flash operations with them, the host test (tests/trx_flash_geom_test.cc)
// checks them. The chip address is what the bitstream's responder reports (docs/specs/T51-ef3.md D4);
// the image is the app's: 16 KiB per bank slot, ROML first, ROMH 8 KiB above.
#ifndef TRX_FLASH_GEOM_H
#define TRX_FLASH_GEOM_H

#include <stdint.h>
#include <stdbool.h>   // the app includes this header from C (T95 D2)
#include "trx_cart_desc.h"

// Image offset of chip address a (chip: 0 ROML, 1 ROMH for a two-chip cartridge).
//   linear 0: {a[20:13], chip, a[12:0]}              (EasyFlash: two chips, interleaved per bank)
//   linear 1: a                                      (one chip across ROML and ROMH, TwoMegabyter)
//   linear 2: EasyFlash 3, the chip's order {slot(3), bank_hi(3), ROMH, bank_lo(3), A12-A0} becomes
//             {slot, bank_hi, bank_lo, ROMH, A12-A0}: bank n = slot * 64 + bank_hi * 8 + bank_lo
static inline uint32_t trx_image_offset(const trx_flash_part_t *p, uint32_t a, int chip)
{
    if (p->linear == 2) {
        return (((a >> 17) & 0x3F) << 17) | (((a >> 13) & 7) << 14) | (((a >> 16) & 1) << 13) | (a & 0x1FFF);
    }
    if (p->linear) {
        return a;
    }
    return ((a >> 13) << 14) | ((uint32_t)chip << 13) | (a & 0x1FFF);
}

// Where a chip address has to be reached from the C64 side: which bank must be selected, which window
// serves it, and the offset inside that window. The inverse of trx_image_offset(), and the reason the flash
// command addresses can stay the data sheet's (`magic1`/`magic2` of the profile, e.g. $555 / $2AA) instead
// of a per-cartridge constant: GMod2's AM29F040 wants $5555, which is bank 2 offset $1555 of its 8 KiB
// window, and that falls out here. T95 D1.
//   linear 0: the chip is given (0 ROML, 1 ROMH); bank = a[20:13]
//   linear 1: one chip across both windows; bank = a[20:14], window = a[13]
//   linear 2: EasyFlash 3; bank = {a[22:20], a[19:17], a[15:13]} = slot * 64 + bank_hi * 8 + bank_lo,
//             window = a[16]
// Invariant, checked for every part in tests/trx_flash_geom_test.cc:
//   trx_image_offset(p, a, acc.window) == acc.bank * 0x4000 + acc.window * 0x2000 + acc.off
typedef struct {
    uint16_t bank;      // the bank that must be selected to see this chip address
    uint8_t  window;    // 0 ROML, 1 ROMH
    uint16_t off;       // offset inside the 8 KiB window
} trx_chip_access_t;

static inline trx_chip_access_t trx_chip_access(const trx_flash_part_t *p, uint32_t a, int chip)
{
    trx_chip_access_t r;
    r.off = (uint16_t)(a & 0x1FFF);
    if (p->linear == 2) {
        r.bank = (uint16_t)((((a >> 20) & 7) << 6) | (((a >> 17) & 7) << 3) | ((a >> 13) & 7));
        r.window = (uint8_t)((a >> 16) & 1);
    } else if (p->linear) {
        r.bank = (uint16_t)(a >> 14);
        r.window = (uint8_t)((a >> 13) & 1);
    } else {
        r.bank = (uint16_t)(a >> 13);
        r.window = (uint8_t)(chip & 1);
    }
    return r;
}

// The CPU address of a window while the MODE field holds `mode` (the profile's wmode or rmode): ROML is
// always $8000; ROMH is $E000 where the mode drives ULTIMAX and $A000 where it does not. `write` takes the
// mode table's GAMEW into account - the C64MegaCart drives GAME low in CPU write cycles only (T33 D3), so
// its ROMH is at $E000 for programming and at $A000 for reading.
static inline uint16_t trx_window_cpu(const trx_cart_desc_t *d, int mode, int window, bool write)
{
    if (!window) {
        return 0x8000;
    }
    uint8_t e = d->desc[24 + (mode & 7)];
    bool exrom_high = (e & 0x01) != 0;
    bool game_low = (e & 0x02) == 0;
    bool game_low_on_write = (e & 0x08) != 0;
    bool ultimax = exrom_high && (game_low || (write && game_low_on_write));
    return ultimax ? 0xE000 : 0xA000;
}

// The window a flash command must go through, from flash.select: a board that answers writes only in ROMH
// is programmed there whatever the chip address says (GMod2 and the C64MegaCart, select ROMH_W without
// ROML_W), one that answers only in ROML likewise (MegaByter). Where both answer, the window follows the
// chip address (one chip across both windows) or the chip index (two chips).
#define TRX_SEL_ROML_R 0x01
#define TRX_SEL_ROML_W 0x02
#define TRX_SEL_ROMH_R 0x04
#define TRX_SEL_ROMH_W 0x08

// Which window the flash answers in: when only one of the two serves this direction, that one, whatever
// the layout says; when both do, the layout's. GMod2 and the C64MegaCart are programmed through ROMH and
// read through ROML, which is why the direction has to be part of the question.
static inline uint8_t trx_flash_window(const trx_flash_part_t *p, uint8_t window, bool write)
{
    bool lo = (p->sel & (write ? TRX_SEL_ROML_W : TRX_SEL_ROML_R)) != 0;
    bool hi = (p->sel & (write ? TRX_SEL_ROMH_W : TRX_SEL_ROMH_R)) != 0;
    if (hi && !lo) {
        return 1;
    }
    if (lo && !hi) {
        return 0;
    }
    return window;
}

// Everything a flash command needs: which bank to select and which CPU address to write. `a` is the chip
// address from the data sheet (the profile's magic1 / magic2, or a byte's own address); `chip` picks the
// chip of a two-chip board. This is what replaces the per-cartridge command addresses the tool used to
// carry (T95 D1/D2); tests/trx_flash_geom_test.cc checks it against all six of them.
static inline trx_chip_access_t trx_flash_cmd(const trx_cart_desc_t *d, uint32_t a, int chip, bool write)
{
    const trx_flash_part_t *p = d->flash;
    trx_chip_access_t r = trx_chip_access(p, a, chip);
    r.window = trx_flash_window(p, r.window, write);
    r.off = (uint16_t)(trx_window_cpu(d, write ? p->wmode : p->rmode, r.window, write) + (a & 0x1FFF));
    return r;
}

// Whether a command cycle has to select a bank at all. The chip only decodes the low bits of the address
// (`mask`), so bits above that are don't-care and the cycle may stay in whatever bank is selected: all the
// parts whose mask fits in one 8 KiB window take their command cycles where they stand. GMod2's AM29F040
// decodes 15 bits, so there the bank is part of the command address ($5555 = bank 2, offset $1555).
static inline bool trx_flash_cmd_needs_bank(const trx_flash_part_t *p)
{
    return p->mask >= 0x2000;
}

// ---- the register side: which IO write reaches a register, and where bank and mode sit in it ----------
//
// Rule n of the descriptor fires on an IO access with ((A7-A0 ^ match) & mask) == 0 in its area and
// direction, and loads its target register with the data (or the address, or a constant), ANDed with that
// register's write mask (cores/icepi/c64_cartridge.v:209-231). `match` itself always satisfies the decode,
// so it is the address a programmer writes to. The bank is {seg(b20) << width(seg(b21))} | seg(b21) and the
// mode is seg(b22) (c64_cartridge.v:241-245). This is what replaces the tool's FLASH_MODE_* enum and its
// ultimax_val / normal_val constants: $DE02, $DF00 and $DE00 come out of the rules, and the values out of
// the mode field together with flash.wmode / flash.rmode. T95 D1/D2.
#define TRX_IO1 0xDE00
#define TRX_IO2 0xDF00

typedef struct {
    uint16_t addr;      // the IO address a CPU write to this field's register goes to; 0: no write rule
    uint8_t  reg;       // 0 R0, 1 R1, 2 R2
    uint8_t  lsb;       // position of the field inside that register
    uint8_t  width;     // 0: the type has no such field
    uint8_t  wmask;     // bits a rule may set in the register (the loaded value is ANDed with this)
    uint8_t  reset;     // the register's value after reset
} trx_reg_field_t;

// The address a CPU write takes to load `reg` from its own data byte, 0 when no rule does that.
static inline uint16_t trx_desc_write_addr(const trx_cart_desc_t *d, int reg)
{
    for (int i = 0; i < 4; i++) {
        uint8_t f = d->desc[4 + 4 * i];
        int tgt = ((f >> 2) & 1) | (((d->desc[37] >> (4 + i)) & 1) << 1);
        if (!(f & 0x80)) { continue; }              // the rule is not there
        if (f & 0x02) { continue; }                 // a read, not a write
        if (((f >> 3) & 3) != 0) { continue; }      // the value would come from the address or a constant
        if (tgt != reg) { continue; }
        return (uint16_t)(((f & 0x01) ? TRX_IO2 : TRX_IO1) | d->desc[5 + 4 * i]);
    }
    return 0;
}

static inline trx_reg_field_t trx_desc_seg(const trx_cart_desc_t *d, int byte, int reg_hi_bit)
{
    trx_reg_field_t f;
    uint8_t s = d->desc[byte];
    f.reg = (uint8_t)((s & 1) | (((d->desc[37] >> reg_hi_bit) & 1) << 1));
    f.lsb = (uint8_t)((s >> 1) & 7);
    f.width = (uint8_t)(s >> 4);
    f.wmask = d->desc[f.reg == 2 ? 39 : 2 + f.reg];
    f.reset = d->desc[f.reg == 2 ? 38 : f.reg];
    f.addr = trx_desc_write_addr(d, f.reg);
    return f;
}

static inline trx_reg_field_t trx_desc_mode(const trx_cart_desc_t *d) { return trx_desc_seg(d, 22, 2); }

// Bank segment 0 is the high part of a two-segment bank field (EasyFlash 3's slot), 1 the low part; a
// one-segment bank field has only segment 1 (width 0 in segment 0).
static inline trx_reg_field_t trx_desc_bank(const trx_cart_desc_t *d, int seg)
{
    return trx_desc_seg(d, seg ? 21 : 20, seg ? 1 : 0);
}

// The value to write to `reg` so that this type shows `bank` in mode `mode`. Fields of other registers do
// not appear; a field of this register that the caller does not mean reads out as 0, which is what the
// types with bank and mode in one register want (GMod2: $DE00 = mode << 6 | bank).
static inline uint8_t trx_desc_reg_value(const trx_cart_desc_t *d, int reg, int bank, int mode)
{
    trx_reg_field_t lo = trx_desc_bank(d, 1), hi = trx_desc_bank(d, 0), m = trx_desc_mode(d);
    uint8_t v = 0;
    if (lo.width && lo.reg == reg) {
        v |= (uint8_t)((bank & ((1 << lo.width) - 1)) << lo.lsb);
    }
    if (hi.width && hi.reg == reg) {
        v |= (uint8_t)(((bank >> lo.width) & ((1 << hi.width) - 1)) << hi.lsb);
    }
    if (m.width && m.reg == reg) {
        v |= (uint8_t)((mode & ((1 << m.width) - 1)) << m.lsb);
    }
    return v;
}

// The part's sector map: runs of equal sectors from the chip's start upwards.
static inline void trx_sector_of(const trx_flash_part_t *p, uint32_t a, uint32_t *start, uint32_t *size)
{
    uint32_t base = 0;
    for (int r = 0; r < p->runs; r++) {
        for (int n = 0; n < p->run[r].count; n++) {
            if (a < base + p->run[r].size) {
                *start = base;
                *size = p->run[r].size;
                return;
            }
            base += p->run[r].size;
        }
    }
    *start = a & ~0xFFFFUL; // beyond the map (cannot happen: a is masked to the chip)
    *size = 0x10000;
}

// What an erase command wipes, in chip addresses: the sector holding `a` (sector erase), or the whole chip
// (chip erase, as the MX29LV640 does: the EasyFlash 3's system area goes with the slots, T51 Q1).
static inline void trx_erase_span(const trx_flash_part_t *p, bool chip_erase, uint32_t a, uint32_t *start, uint32_t *size)
{
    if (chip_erase) {
        *start = 0;
        *size = p->chip_size;
    } else {
        trx_sector_of(p, a, start, size);
    }
}

#endif
