// What every file of the trx64u library shares: the base's table (lib_trx64u.cc keeps it), the log, the settings as the base shows them (setting.cc), and
// small helpers over the groups of base 3.1. A group the base does not give (null) makes its helper do nothing and read 0; the code that needs a group
// checks it first. The groups are read at each use, not at the registration: the base fills api->trx and api->c64 after the libraries are loaded.
#ifndef TRX64U_LIB_H
#define TRX64U_LIB_H

#include <stdint.h>
#include "libapi.h"
#include "libc_extra.h"

extern const LibApi *g_api;

// printf for the log: one line (a trailing newline is dropped), at most 200 bytes.
void lib_logf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// ---- the settings: what api->ops->state("set.<key>") says, turned back into what the code works with (setting.cc, the lists of setlists.h)
int setting_ix(const char *key, int def);           // the index of the value (an enum's position, a range's number - its low end); def: no such setting or no value
int setting_num(const char *key, int def);          // the index plus the setting's base: a range's number as shown (-780), an enum's index
int setting_count(const char *key);                 // the number of values, 0 for a key the menu does not have
bool setting_known(const char *key);                // the menu has a choice setting of that key
const char *setting_text(const char *key);          // a text setting's text without leading blanks, "" when empty or unknown
bool settings_ready(void);                          // the base has the settings (a state of the menu's own answers)

// ---- the groups
static inline uint32_t now_ms(void) { return g_api->sys ? g_api->sys->ms() : 0; }
void wait_ms(uint32_t ms);                          // at least ms (lib_trx64u.cc)

// The TRX block, byte registers (api->trx->regs).
static inline uint8_t trx_rd(uint32_t reg) { return g_api->trx ? g_api->trx->regs[reg] : 0; }
static inline void trx_wr(uint32_t reg, uint8_t v)
{
    if (g_api->trx) {
        g_api->trx->regs[reg] = v;
    }
}

// The C64's memory window (api->c64): the bus as the machine sees it, or the RAM's second port while C64_DMA_MEMONLY is on.
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
