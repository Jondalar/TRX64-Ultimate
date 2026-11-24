/*
 * cart_screen.h
 *
 *  What the dumper and the writer show: the header naming the tool and the
 *  cartridge, the EasyFlash 3 slot, the CRT file, one bank map per chip, a
 *  status line and the keys. This port draws no frame: each call sets a field
 *  of g_ui (cart_screen.c, trex_ui.h), and the screens of app.toml show them.
 */

#ifndef CART_SCREEN_H
#define CART_SCREEN_H

#include "ui_shim.h"
#include <stdbool.h>
#include <stdint.h>

// The text area of the firmware's former frame (rows 3 to 22, columns 1 to 38); the tools still cut their lines to it
#define CART_SCREEN_ROWS 20
#define CART_SCREEN_COLS 38

// The parts of the screen by name (cart_screen.c: a number names the field a call sets, no row is drawn). -1 where a part is absent.
typedef struct {
    int banks;
    int rows;           // map rows per chip, 16 banks each
    bool has_romh;
    int y_header;
    int y_slot;         // EasyFlash 3 only
    int y_file;         // -1 when the map needs the line (256 banks)
    int y_roml;         // column header of the first map
    int y_romh;         // column header of the second map
    int y_status;
    int y_keys;
    int y_keys2;        // EasyFlash 3 only: F7=Select slot
} cart_layout_t;

// The window of the shim (ui_shim.h window_open); nothing is drawn into it
Window *cart_screen_open(void);

// The parts for a cartridge of `banks` banks, and the map's bank counts and the EasyFlash 3 flags of g_ui
void cart_layout_compute(cart_layout_t *l, int banks, bool has_romh, bool ef3);

// A line of text: the header (y_header) goes to the log, the slot line (y_slot) to g_ui.slot
void cart_screen_line(Window *w, int y, int color, const char *text);

void cart_screen_status(Window *w, const cart_layout_t *l, const char *msg);

// "CRT: name", or "CRT: -" without one
void cart_screen_file(Window *w, const cart_layout_t *l, const char *name);

// Keys as label/action pairs: "F1", "Scan", "F3", "Dump", ...
void cart_screen_keys(Window *w, int y, const char *const *pairs, int npairs);

void cart_draw_bank_map(Window *w, const cart_layout_t *l, const uint8_t *status_lo, const uint8_t *status_hi);

#endif
