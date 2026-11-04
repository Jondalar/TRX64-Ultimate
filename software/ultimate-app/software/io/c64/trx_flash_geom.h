// The address and sector geometry of the flash parts the TRX64 bitstream emulates (trx_cart_desc.h), as
// pure functions: trx_cart.cc applies flash operations with them, the host test (tests/trx_flash_geom_test.cc)
// checks them. The chip address is what the bitstream's responder reports (docs/specs/T51-ef3.md D4);
// the image is the app's: 16 KiB per bank slot, ROML first, ROMH 8 KiB above.
#ifndef TRX_FLASH_GEOM_H
#define TRX_FLASH_GEOM_H

#include <stdint.h>
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
    return ((a >> 13) << 14) | (uint32_t(chip) << 13) | (a & 0x1FFF);
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
