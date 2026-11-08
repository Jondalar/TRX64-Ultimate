/*
 * cart_flash.h
 *
 *  The flash profile of a cartridge type, derived from its mapping descriptor (T95 D2).
 *
 *  Everything the writer used to carry as a literal per cartridge - the flash command addresses, which IO
 *  register opens the write window and with which value, how many banks there are and how big they are -
 *  comes out of tools/cart_descriptors (one JSON per type) through the generated trx_cart_desc.h. A new cartridge of
 *  the common kind is one JSON file: no struct here, no case in the writer.
 *
 *  Pure derivation: no bus, no screen, no g_api. The host test and the firmware see the same numbers
 *  (software/io/c64/trx_flash_geom.h, tests/trx_flash_geom_test.cc).
 */

#ifndef CART_FLASH_H
#define CART_FLASH_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "trx_cart_desc.h"
#include "trx_flash_geom.h"

typedef struct
{
    const trx_cart_desc_t *desc;
    const trx_flash_part_t *part;
    const char *name;        // the descriptor's short name, for the 40-column window title
    uint8_t cart_type;       // the app's CART_TYPE byte
    uint16_t crt_type;       // the CRT hardware type this cartridge takes
    int max_banks;           // banks the C64 can select on this board
    int bank_bytes;          // 0x4000 when the bank's ROMH half belongs to it, else 0x2000
    int total_size;          // what the user can actually write: banks x bank_bytes
    bool has_romh;           // the bank's upper 8 KiB is served through ROMH
    uint16_t bank_addr;      // IO address that takes the bank number
    uint16_t mode_addr;      // IO address that takes the MODE field
    uint8_t wval;            // value for mode_addr that opens the flash for writing
    uint8_t rval;            // value for mode_addr that shows the banks for reading
    bool mode_in_bank_reg;   // bank and mode share one register (GMod2), so one write carries both
    bool eeprom;             // an EEPROM hangs on the same register bits (GMod2's M93C86)
} cart_flash_t;

// The profile of a CART_TYPE / of the cartridge a CRT of this type belongs to; NULL when the type has no
// flash the C64 can program, or no descriptor at all.
const cart_flash_t *cart_flash_by_cart_type(uint8_t cart_type);
const cart_flash_t *cart_flash_by_crt_type(uint16_t crt_type);

// The register value that shows `bank` in `mode` on this cartridge (GMod2 carries both in $DE00).
uint8_t cart_flash_bank_value(const cart_flash_t *f, int bank, int mode);

#endif
