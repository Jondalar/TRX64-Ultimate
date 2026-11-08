/*
 * cart_flash.c
 *
 *  Builds the flash profiles of cart_flash.h from the generated descriptor table. T95 D2.
 *
 *  How the numbers come about:
 *    has_romh    the bank's ROMH half belongs to the bank (descriptor byte 32 bit 0: L16 and SPLIT).
 *                A GMod2 or MegaByter mirrors ROML into ROMH, so its banks are 8 KiB.
 *    total_size  what the user can write: a two-chip board has both chips (EasyFlash 1 MiB), a linear
 *                one has its chip (TwoMegabyter 2 MiB), and the EasyFlash 3's slot has its share of the
 *                8 MiB part (1 MiB), because the writer works one slot at a time (T51).
 *    max_banks   total_size / bank_bytes, never more than the bank field can address.
 *    mode_addr   the IO address of the rule that loads the register the MODE field sits in, and the two
 *                values are that field set to flash.wmode and flash.rmode.
 */

#include "cart_flash.h"

#define MAX_PROFILES 8

static cart_flash_t s_prof[MAX_PROFILES];
static int s_nprof = -1;

static void build(void)
{
    s_nprof = 0;
    for (unsigned i = 0; i < sizeof(c_cart_descs) / sizeof(c_cart_descs[0]) && s_nprof < MAX_PROFILES; i++) {
        const trx_cart_desc_t *d = &c_cart_descs[i];
        if (!d->flash) {
            continue;
        }
        cart_flash_t *f = &s_prof[s_nprof++];
        trx_reg_field_t lo = trx_desc_bank(d, 1), hi = trx_desc_bank(d, 0), m = trx_desc_mode(d);
        int slots = (d->flash->linear == 2 && hi.width) ? (1 << hi.width) : 1;
        int addressable = 1 << (lo.width + (d->flash->linear == 2 ? 0 : hi.width));

        f->desc = d;
        f->part = d->flash;
        f->name = d->short_name;
        f->cart_type = d->cart_type;
        f->crt_type = d->crt_type;
        f->has_romh = (d->desc[32] & 0x01) != 0;
        f->bank_bytes = f->has_romh ? 0x4000 : 0x2000;
        f->total_size = (int)(d->flash->linear ? d->flash->chip_size / slots
                                               : d->flash->chip_size * d->flash->chips);
        f->max_banks = f->total_size / f->bank_bytes;
        if (f->max_banks > addressable) {
            f->max_banks = addressable;
        }
        f->bank_addr = lo.addr;
        f->mode_addr = m.addr;
        f->wval = trx_desc_reg_value(d, m.reg, 0, d->flash->wmode);
        f->rval = trx_desc_reg_value(d, m.reg, 0, d->flash->rmode);
        f->mode_in_bank_reg = (m.width != 0 && m.reg == lo.reg);
        f->eeprom = (d->desc[32] & 0x08) != 0;
    }
}

const cart_flash_t *cart_flash_by_cart_type(uint8_t cart_type)
{
    if (s_nprof < 0) {
        build();
    }
    for (int i = 0; i < s_nprof; i++) {
        if (s_prof[i].cart_type == cart_type) {
            return &s_prof[i];
        }
    }
    return NULL;
}

const cart_flash_t *cart_flash_by_crt_type(uint16_t crt_type)
{
    if (s_nprof < 0) {
        build();
    }
    for (int i = 0; i < s_nprof; i++) {
        if (s_prof[i].crt_type == crt_type) {
            return &s_prof[i];
        }
    }
    return NULL;
}

uint8_t cart_flash_bank_value(const cart_flash_t *f, int bank, int mode)
{
    return trx_desc_reg_value(f->desc, trx_desc_bank(f->desc, 1).reg, bank, mode);
}
