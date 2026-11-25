// What every file of the trx64u library shares: the base's table (lib_trx64u.cc keeps it), the log, the settings as the base holds them (setting.cc), the
// groups of base 3.3 (g_api->sync, settings, ctx, io, board), the exports of the app's libraries (MultiCore's shared ones and our UCI library), and small
// helpers over them. A group or an export the base does not give (null) makes its helper do nothing and read 0; the code that needs one checks it first.
// Nothing is read at the registration: the groups and the exports are read at each use (api->c64 is filled after the libraries are loaded, io once the
// core is handed to the base, the shared libraries load in any order). The registers are the driver table's (drvtab.h, below), generated from c64_basic's
// TOML A.
#ifndef TRX64U_LIB_H
#define TRX64U_LIB_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "libapi.h"
#include "libc_extra.h"
#include "c64cart_api.h"
#include "cbm1541_api.h"
#include "trxuci_api.h"                // our UCI library's export "uci" (../uci, base/Makefile LIB_INC)
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
// The five groups base 3.3 added to LibApi are there on every base this library loads on (LIB_NEEDS base>=3.3; api.md 3.2.12): sync, settings, ctx and
// board always, io on a core with a register window and once the core is handed over. Each is still tested for null before a call. A member 3.3 appended
// to an older table (SvcSys, SvcFs) is called only when SVC_HAS says the table has it: its size covers the member and the member is set (api.md 3.2.2).
#define SVC_HAS(T, t, m) ((t) && (t)->size >= offsetof(T, m) + sizeof(((T *)0)->m) && (t)->m)

static inline uint32_t now_ms(void) { return g_api->sys ? g_api->sys->ms() : 0; }
void wait_ms(uint32_t ms);                          // at least ms (lib_trx64u.cc)

// The running core's flags, CTX_* of svc_ctx.h (ctx.state); 0 without the group.
static inline uint32_t ctx_state(void)
{
    const SvcCtx *c = g_api->ctx;
    return c ? c->state() : 0;
}

// The mount slots of this core, positions in desc.toml's [mount] list (core_hooks.h slot_*). Changes to them are told to the base by the shared
// libraries behind them (cbm1541, c64cart: ctx.mount_notify by the slot's name), not by this library.
enum { SLOT_DRIVE_A = 0, SLOT_CART = 1 };

// The DDR2 window of the core from offset 0 (io.ddr_read, io.ddr_write): the REU's memory. A write is complete on return. Without the group a read gives zeros.
static inline void ddr_read(uint32_t off, void *p, uint32_t n)
{
    const SvcIo *io = g_api->io;
    if (io) {
        io->ddr_read(off, p, n);
    } else {
        memset(p, 0, n);
    }
}
static inline void ddr_write(uint32_t off, const void *p, uint32_t n)
{
    const SvcIo *io = g_api->io;
    if (io) {
        io->ddr_write(off, p, n);
    }
}

// The registers of the bitstream: every address, field and value is a name of the driver table, drvtab.h, which the build generates from c64_basic's
// TOML A (../gateware/c64_basic.toml; MultiCore T239.3 D31, D34, api.md 6.17): DRV_<block>_<property>_RD() / _WR(v), a run's _RD(i) / _WR(i, v),
// _<field>_MASK / _SHIFT, _<state>. HW_RD, HW_WR and HW_WR_I take a block and property as one name (HW_RD(cart_regs_stop)) and touch the register only
// once the base has handed the core over (g_api->io is set then, api.md 4.15.1); before that a write does nothing and a read gives 0. A 16- or
// 32-bit register is one access: the bus converter splits it into its bytes, byte 0 first (gateware/c64_basic.toml has the RTL lines).
#include "drvtab.h"
#define HW_RD(bp) (g_api->io ? DRV_##bp##_RD() : 0)
#define HW_WR(bp, v)                                                                                                                         \
    do {                                                                                                                                       \
        if (g_api->io) {                                                                                                                       \
            DRV_##bp##_WR(v);                                                                                                                  \
        }                                                                                                                                      \
    } while (0)
#define HW_WR_I(bp, i, v)                                                                                                                    \
    do {                                                                                                                                       \
        if (g_api->io) {                                                                                                                       \
            DRV_##bp##_WR(i, v);                                                                                                               \
        }                                                                                                                                      \
    } while (0)
// The offset of a property in its block, for the io group's calls that take one (cart_rd, cart_wr, rom_read, rom_write): DRV_OFF(c64_roms, kernal).
#define DRV_OFF(b, p) ((uint32_t)(DRV_##b##_##p##_ADDR - DRV_##b##_ADDR))

// A block of the core (owner "core" in the TOML A) is touched through its accessors above. A block of the base that the base's io group has a call for
// goes through that call, with the offset from the table: the cartridge port's page (cart_regs: CART_RD, CART_WR), the ROM apertures (c64_roms:
// c64_rom_write, io.rom_read), the video format (c64_video: c64_video_format) and the DMA mode (c64_dma_mode: c64_ram_memonly); the stop and the reset
// are the base's own operations on cart_regs (c64_stop, c64_mode_reset). Without the io group a write does nothing and a read gives 0.
static inline uint8_t c64_cart_rd(uint32_t off)
{
    const SvcIo *io = g_api->io;
    return io ? io->cart_rd(off) : 0;
}
static inline void c64_cart_wr(uint32_t off, uint8_t v)
{
    const SvcIo *io = g_api->io;
    if (io) {
        io->cart_wr(off, v);
    }
}
#define CART_RD(p) c64_cart_rd(DRV_OFF(cart_regs, p))
#define CART_WR(p, v) c64_cart_wr(DRV_OFF(cart_regs, p), (uint8_t)(v))
static inline void c64_stop(bool on)
{
    const SvcIo *io = g_api->io;
    if (io) {
        io->stop(on);
    }
}
static inline void c64_mode_reset(bool on)
{
    const SvcIo *io = g_api->io;
    if (io) {
        io->mode_reset(on);
    }
}
// on: every byte of the C64's memory window goes to the RAM's second port, $D000-$DFFF included
static inline void c64_ram_memonly(bool on)
{
    const SvcIo *io = g_api->io;
    if (io) {
        io->ram_memonly(on);
    }
}
static inline void c64_video_format(uint8_t v)
{
    const SvcIo *io = g_api->io;
    if (io) {
        io->video_format(v);
    }
}
static inline void c64_rom_write(uint32_t off, const void *p, uint32_t n)
{
    const SvcIo *io = g_api->io;
    if (io) {
        io->rom_write(off, p, n);
    }
}

// An operation of the registry by name (api->ops->call), its message copied to msg. Returns its OpErr (OPE_NOT_FOUND: no library has it).
int op_call(const char *name, const OpIn *in, int nin, char *msg, int msglen);

// ---- the exports of the app's libraries (MultiCore's shared ones, our UCI library's "uci": ../uci) and the ring library's "ringstill"
// (api->lib->find(name, U2A3_VERSION(1, 0), &size), major 1): found at the first use and kept, looked for again while not found.
// x_table answers the table when its size covers the member at `off` and that member is set; else null. X_GET names the member.
enum { X_CART, X_DRIVE, X_UCI, X_TEMPDISK, X_PRG, X_RINGSTILL, X_N };   // "c64cart", "cbm1541", "uci", "tempdisk", "cbmprg", "ringstill"
const void *x_table(int which, uint32_t off);
#define X_GET(which, T, m) ((const T *)x_table((which), (uint32_t)offsetof(T, m)))

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
