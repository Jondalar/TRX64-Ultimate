/*
 * cart_dumper.h
 *
 *  Dump physical cartridges to .CRT files via the U64 cart slot, with a
 *  visual bank map showing scan/dump progress. Port of the firmware
 *  CartDumper (software/io/c64/cart_dumper.h).
 */

#ifndef CART_DUMPER_H
#define CART_DUMPER_H

#include "cart_base.h"

typedef struct
{
    int hw_type;
    const char *type_name;
    bool banking_found;
    int bank_count;
} cart_probe_result_t;

typedef struct
{
    cart_base_t base;

    // Detection
    int det_banks;
    int crt_hw_type;
    bool has_romh;      // true for 16K/Ultimax carts
    uint16_t romh_addr; // $A000 (16K) or $E000 (Ultimax)

    // Bank map (separate arrays for ROML and ROMH chips)
    uint8_t bank_status_lo[MAX_CART_BANKS];
    uint8_t bank_status_hi[MAX_CART_BANKS];

    // Detected probe result (saved for later use in the dump)
    cart_probe_result_t probe_result;

    int slot_shown; // the slot of a whole EasyFlash 3 dump being read, -1 while none is
    char file[40];  // the file last written
} cart_dumper_t;

// Sets up the bus, probes the cartridge and scans its banks. Reports what it finds through cart_view.h.
void cart_dumper_open(cart_dumper_t *d);
// Restores the bus
void cart_dumper_close(cart_dumper_t *d);

// What a view can ask for; none of them knows a key or a window
void cart_dumper_scan(cart_dumper_t *d);
void cart_dumper_dump(cart_dumper_t *d);
void cart_dumper_select(cart_dumper_t *d); // EasyFlash 3: which slot, or the whole image

#endif
