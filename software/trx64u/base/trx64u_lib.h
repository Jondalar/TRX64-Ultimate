// What every file of the trx64u library shares: the base's table (lib_trx64u.cc keeps it), the log, the settings as the base holds them (setting.cc), the
// groups of base 3.3 (../include/base33.h), the exports of MultiCore's shared libraries, and small helpers over them. A group or an export the base does
// not give (null) makes its helper do nothing and read 0; the code that needs one checks it first. Nothing is read at the registration: the groups and
// the exports are read at each use (api->trx and api->c64 are filled after the libraries are loaded, the shared libraries load in any order).
#ifndef TRX64U_LIB_H
#define TRX64U_LIB_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "libapi.h"
#include "libc_extra.h"
#include "base33.h"
#include "c64cart_api.h"
#include "cbm1541_api.h"
#include "uci_api.h"
#include "sid_api.h"
#include "tempdisk_api.h"
#include "cbmprg_api.h"

extern const LibApi *g_api;

// printf for the log: one line (a trailing newline is dropped), at most 200 bytes.
void lib_logf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// ---- the settings of the running core (setting.cc): the base's settings group, the index of each key of this menu kept while its generation holds
int setting_ix(const char *key, int def);           // the position of the value in force (an enum's index, a range's number - its low end); def: no such setting
int setting_num(const char *key, int def);          // the position plus the setting's base: a range's number as shown (-780), an enum's index
int setting_count(const char *key);                 // the number of values, 0 for a key the menu does not have
bool setting_known(const char *key);                // the menu has a choice setting of that key
const char *setting_label(const char *key, int ix); // the label of value ix of a choice setting of the menu; null: none (a range, no such key)
const char *setting_text(const char *key);          // a text setting's text without leading blanks, "" when empty or unknown; copy it at once
bool settings_ready(void);                          // the base has merged this menu's settings
bool setting_set(const char *key, int ix);          // settings.find + settings.set: stored (the .cfg with Auto Save Config), not applied
void setting_override(const char *key, int ix);     // settings.find + settings.override_value: this run only, not applied

// ---- the groups
static inline uint32_t now_ms(void) { return g_api->sys ? g_api->sys->ms() : 0; }
void wait_ms(uint32_t ms);                          // at least ms (lib_trx64u.cc)

// The running core's flags, CTX_* of svc_ctx.h (ctx.state); 0 without the group.
static inline uint32_t ctx_state(void)
{
    const SvcCtx *c = b33_ctx();
    return c ? c->state() : 0;
}

// The mount slots of this core, positions in desc.toml's [mount] list (core_hooks.h slot_*). Changes to them are told to the base by the shared
// libraries behind them (cbm1541, c64cart: ctx.mount_notify by the slot's name), not by this library.
enum { SLOT_DRIVE_A = 0, SLOT_CART = 1 };

// The DDR2 window of the core from offset 0 (io.ddr_read, io.ddr_write): the REU's memory. A write is complete on return. Without the group a read gives zeros.
static inline void ddr_read(uint32_t off, void *p, uint32_t n)
{
    const SvcIo *io = b33_io();
    if (io) {
        io->ddr_read(off, p, n);
    } else {
        memset(p, 0, n);
    }
}
static inline void ddr_write(uint32_t off, const void *p, uint32_t n)
{
    const SvcIo *io = b33_io();
    if (io) {
        io->ddr_write(off, p, n);
    }
}

// The C64U's register blocks through the io group, offsets in c64u_regs.h: the cartridge-port control block (cart_rd / cart_wr), the C64 I/O block
// (bus_wr), the machine stop, the reset, the memory-only mode of the RAM window, the video format and the ROM aperture. Without the group a write does
// nothing and a read gives 0.
static inline uint8_t c64_cart_rd(uint32_t reg)
{
    const SvcIo *io = b33_io();
    return io ? io->cart_rd(reg) : 0;
}
static inline void c64_cart_wr(uint32_t reg, uint8_t v)
{
    const SvcIo *io = b33_io();
    if (io) {
        io->cart_wr(reg, v);
    }
}
static inline void c64_bus_wr(uint32_t off, uint8_t v)
{
    const SvcIo *io = b33_io();
    if (io) {
        io->bus_wr(off, v);
    }
}
static inline void c64_stop(bool on)
{
    const SvcIo *io = b33_io();
    if (io) {
        io->stop(on);
    }
}
static inline void c64_mode_reset(bool on)
{
    const SvcIo *io = b33_io();
    if (io) {
        io->mode_reset(on);
    }
}
// on: every byte of the C64's memory window goes to the RAM's second port, $D000-$DFFF included
static inline void c64_ram_memonly(bool on)
{
    const SvcIo *io = b33_io();
    if (io) {
        io->ram_memonly(on);
    }
}
static inline void c64_video_format(uint8_t v)
{
    const SvcIo *io = b33_io();
    if (io) {
        io->video_format(v);
    }
}
static inline void c64_rom_write(uint32_t off, const void *p, uint32_t n)
{
    const SvcIo *io = b33_io();
    if (io) {
        io->rom_write(off, p, n);
    }
}

// A register window of the bitstream (io.window; the ids, names and needed sizes are C64U_WINDOWS of c64u_regs.h): its base, kept once found. Null when
// the io group is missing, desc.toml does not declare the window, the address map does not grant it, or it is shorter than the offsets need; the
// first null of each window is logged, and the caller then writes nothing. One byte per access.
volatile uint8_t *c64u_win(int w);
static inline void c64u_win_wr(int w, uint32_t off, uint8_t v)
{
    volatile uint8_t *p = c64u_win(w);
    if (p) {
        p[off] = v;
    }
}

// An operation of the registry by name (api->ops->call), its message copied to msg. Returns its OpErr (OPE_NOT_FOUND: no library has it).
int op_call(const char *name, const OpIn *in, int nin, char *msg, int msglen);

// ---- the exports of MultiCore's shared libraries (api->lib->find(name, U2A3_VERSION(1, 0), &size), major 1): found at the first use and kept, looked
// for again while not found.
// x_table answers the table when its size covers the member at `off` and that member is set; else null. X_GET names the member.
enum { X_CART, X_DRIVE, X_UCI, X_SID, X_TEMPDISK, X_PRG, X_N };   // "c64cart", "cbm1541", "uci", "sid", "tempdisk", "cbmprg"
const void *x_table(int which, uint32_t off);
#define X_GET(which, T, m) ((const T *)x_table((which), (uint32_t)offsetof(T, m)))

// The TRX block, byte registers (api->trx->regs).
static inline uint8_t trx_rd(uint32_t reg) { return g_api->trx ? g_api->trx->regs[reg] : 0; }
static inline void trx_wr(uint32_t reg, uint8_t v)
{
    if (g_api->trx) {
        g_api->trx->regs[reg] = v;
    }
}

// The C64's memory window (api->c64): the bus as the machine sees it, or the RAM's second port while c64_ram_memonly is on.
static inline bool c64_window(void) { return g_api->c64 != 0; }
static inline uint8_t c64_peek(uint32_t addr) { return g_api->c64 ? g_api->c64->peek((uint16_t)addr) : 0; }
static inline void c64_poke(uint32_t addr, uint8_t v)
{
    if (g_api->c64) {
        g_api->c64->poke((uint16_t)addr, v);
    }
}
static inline void ram_write(uint32_t addr, const void *p, uint32_t n)
{
    if (g_api->c64) {
        g_api->c64->dma_write((uint16_t)addr, p, (int)n);
    }
}
static inline void ram_read(uint32_t addr, void *p, uint32_t n)
{
    if (g_api->c64) {
        g_api->c64->dma_read((uint16_t)addr, p, (int)n);
    } else {
        memset(p, 0, n);
    }
}

// A transfer buffer from the library's own account (api->mem) while an operation runs: 64 KB, or 4 KB when the heap has no 64 KB in one piece. *cap is
// its size; null: not even 4 KB. Given back with xfer_free.
uint8_t *xfer_alloc(uint32_t *cap);
static inline void xfer_free(uint8_t *p) { g_api->mem->free(p); }

#endif
