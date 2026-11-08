/*
 * cart_writer.h
 *
 *  Flash .CRT files to physical flash cartridges via the U64 cart slot. Which
 *  cartridges those are is data: every type with a flash block in
 *  tools/cart_descriptors/ (cart_flash.h). Port of the firmware CartWriter
 *  (software/io/c64/cart_writer.h).
 */

#ifndef CART_WRITER_H
#define CART_WRITER_H

#include "cart_base.h"
#include "cart_flash.h"


typedef struct
{
    cart_base_t base;

    // CRT file info
    char crt_path[256];
    char crt_filename[256];
    uint8_t *crt_data; // the whole CRT, read once when the writer opens (NULL when it is streamed)
    uint32_t crt_size;
    bool crt_stream;   // the CRT is read from the file by every pass (our core, or too big for the heap)

    // Detected cart: its profile, derived from the mapping descriptor (cart_flash.h, T95 D2)
    const cart_flash_t *chip;

    // Bank map (separate arrays for ROML and ROMH chips)
    uint8_t bank_status_lo[MAX_CART_BANKS];
    uint8_t bank_status_hi[MAX_CART_BANKS];
    int num_banks_found;

    // CRT data
    int crt_num_chips;
    int crt_total_bytes;
    char crt_name[33];
    uint16_t crt_type;                // hardware type from the CRT header
    bool crt_has_eeprom;              // the CRT holds GMod2 EEPROM contents (load $DE00)
    char ef_name[EF3_NAME_LEN + 1];   // "EF-Name:" found while writing (EasyFlash 3), empty if none

    int ef_current_bank; // target bank for EF byte programming

    int img_slot; // the slot of a type 90 image being written, -1 while none is
} cart_writer_t;

// Checks a CRT file before the writer opens on it (header, type, whole chip packets, banks, at least one chip).
// NULL = ok, else the reason for a popup. Reads the file only.
const char *cart_writer_check_crt(const char *path, const char *filename);

// Sets up the bus, detects the flash cart, reads the CRT summary and scans the banks. An empty filename opens
// the writer on the cartridge alone. Reports what it finds through cart_view.h.
void cart_writer_open(cart_writer_t *w, const char *path, const char *filename);
// Restores the bus and gives the heap back
void cart_writer_close(cart_writer_t *w);

// What a view can ask for. Each one reports its own progress and its own result; none of them knows a key.
void cart_writer_scan(cart_writer_t *w);
void cart_writer_write(cart_writer_t *w);
void cart_writer_verify(cart_writer_t *w);
void cart_writer_select(cart_writer_t *w); // EasyFlash 3: slot, full image, or erase the whole cart

#endif
