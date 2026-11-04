/*
 * mock.h - host test harness for trex_crt (T57 spec 8.1): a mock u2p_api_t with a C64 bus (PLA, CPU port,
 * app Ultimax), the TRX slot registers and behavioural cartridge models. The app's real C files are linked
 * against it; nothing here is part of the app.
 */
#ifndef MOCK_H
#define MOCK_H

#include "u2p_app.h"
#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>

// ---- cartridges -------------------------------------------------------------------------------------------
typedef struct cart cart_t;
struct cart {
    const char *name;
    void *priv;
    int (*pins)(cart_t *c);                                       // b0 GAME, b1 EXROM, 1 = high
    int (*io_rd)(cart_t *c, uint16_t a, uint8_t *v);              // 1 = the cart drives the bus
    void (*io_wr)(cart_t *c, uint16_t a, uint8_t v);
    int (*rom_rd)(cart_t *c, int half, uint16_t off, uint8_t *v); // half 0 ROML, 1 ROMH
    void (*rom_wr)(cart_t *c, int half, uint16_t off, uint8_t v);
    void (*reset)(cart_t *c);
    uint8_t (*bank_byte)(cart_t *c, int bank, int half, int off); // content as a dump shows it, for comparison
    uint8_t *eeprom;                                              // GMod2 (2 KB), else NULL
    int banks;                                                    // banks of 8K per half the dump has
    int crt_type;                                                 // the type the dump must say
    bool broken;                                                  // failure injection: the chip stops answering
    int view_slot;                                                // EF3: the slot bank_byte shows
};

cart_t *cart_rom16(void);
cart_t *cart_ef1(void);
cart_t *cart_ef3(void);
cart_t *cart_megabyter(void);
cart_t *cart_gmod2(void);
cart_t *cart_megacart(void);
void cart_free(cart_t *c);
// Fills the cart's flash with the pattern of the bank's content (rom16, ef1: banks 0..63) so dumps are distinct
void cart_fill_pattern(cart_t *c, uint32_t seed);
// EasyFlash 3 only: every slot and the system area hold data
void cart_ef3_fill_full(cart_t *c, uint32_t seed);

// ---- the machine ------------------------------------------------------------------------------------------
typedef struct {
    bool trx;               // api->trx != NULL (our bitstream)
    bool has_slot;          // SLOT_STATUS b31
    bool armed;             // b16
    int ext_mask, int_mask; // as left by the last reset (b11:8, b15:12)
    int fault;              // b17, cause in fault_cause
    int fault_cause;
    int pref;               // Cartridge Preference
    uint8_t prog_pdir, prog_pout; // the frozen program's CPU port
    bool app_ultimax;       // C64_MODE Ultimax of the freeze
    bool wr0;               // ROM_WR_EN at start
    bool has_media_reset;
    bool masks_stuck;       // cart_setup_bus does not change the masks (U5)
    bool wr_stuck;          // ROM_WR_EN cannot be set (U6)
    bool wr_sticky;         // ROM_WR_EN cannot be cleared again (U7)
    bool bad_id;            // api->trx is there but the block does not read TRX2
    bool open_echo;         // an undriven read returns the last byte on the bus (bus capacitance) instead of a fixed byte
    long fail_at;           // access index from which the cart is broken (0 = never)
    long max_accesses;      // hang guard
} mock_cfg_t;

typedef struct {
    // observation
    long accesses;
    long unguarded_rom_writes; // ROML/ROMH write cycles the app attempted while ROM_WR_EN = 0 (TRX)
    long blocked_rom_writes;
    long rom_writes;
    int reset_requested;
    bool wr_en;
    int ext_mask, int_mask;
    uint8_t pdir, pout;
    bool app_ultimax;
    bool bus_open;          // between cart_setup_bus and cart_restore_bus
    int popups;
    char last_popup[200];
    char last_log[256];
    long trx_reads, trx_writes;
    int hang;
} mock_state_t;

extern char mock_logbuf[];   // every log line of the run (the last 64 KB)
extern mock_cfg_t mock_cfg;
extern mock_state_t mock;
extern cart_t *mock_cart;
extern FILE *mock_trace;   // access trace (bus traffic only), NULL = off

void mock_init(cart_t *cart, const mock_cfg_t *cfg);   // resets state, builds the api
const u2p_api_t *mock_api(void);
void mock_ctx(u2p_app_context_t *ctx);

// UI script: answers of the dialogs
typedef struct {
    int popup_yes;                 // answer to YES/NO popups: BUTTON_YES or BUTTON_NO
    int menu_choice;               // menu() result
    char dir[128];                 // path_input
    char name[64];                 // string_input default overrides when non-empty
    int keys[64];                  // getch queue
    int nkeys, keypos;
} mock_ui_t;
extern mock_ui_t mock_ui;
extern size_t mock_alloc_max, mock_alloc_big;   // heap model: largest request that succeeds (0 = any), largest asked
extern int mock_open_files;
extern int mock_wfail_opens, mock_wfail_skip; // the next N write-opens (after skipping some) lose their drive ...
extern int mock_alloc_n, mock_alloc_live;  // only N more allocations succeed (0 = all, -1 none) / not freed yet
extern long mock_wfail_after;              // ... after this many bytes (fwrite then returns 0)
extern int mock_retry_popups, mock_retry_yes; // "Saving failed. Retry?" popups seen / answered YES
extern int mock_seekcur_works; // 1: the firmware's _lseek has SEEK_CUR (newlib fseek then always works)
extern long mock_failed_seeks; // fseeks the mock failed like today's firmware

// fs root: files are mapped to this host directory
extern char mock_fsroot[256];

// SLOT regs for tests
uint8_t mock_trx_rd8(uint32_t off);
void mock_trx_wr8(uint32_t off, uint8_t v);
uint32_t mock_status_word(void);

#endif
