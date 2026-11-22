// What the ring library's files share of the base (U18): the LibApi pointer, the base 3.3 groups (base33.h), the exports of MultiCore's shared
// libraries, memory on the library's own account, text without a C library, the TRX IO block's registers, the C64 machine's registers and memory
// window, and files. ringsys.cc.
#ifndef RING_RINGSYS_H
#define RING_RINGSYS_H

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <string.h>
#include "libapi.h"
#include "base33.h"
#include "c64cart_api.h"
#include "cbm1541_api.h"
#include "uci_api.h"
#include "c64u_regs.h"

// Set by lib_register (lib_ring.cc) before anything else runs.
extern const LibApi *g_api;

// ---- the exports of MultiCore's shared libraries (SvcLib::find(name, 1, &size)) -----------------------------------------------------------------------
// Looked up at their first use and kept once found; while one is not loaded every use asks again.
enum RExportId { RX_C64CART, RX_CBM1541, RX_UCI, RX_COUNT };
// The table of export `id` when it is loaded, its size covers the member at `off` (`len` bytes) and that member is set; else null.
const void *r_export(RExportId id, uint32_t off, uint32_t len);
// The table, typed, when `member` can be called through it; null otherwise.
#define R_TABLE(Type, id, member) ((const Type *)r_export((id), uint32_t(offsetof(Type, member)), uint32_t(sizeof(((Type *)0)->member))))

// ---- time ------------------------------------------------------------------------------------------------------------------------------------------------
// The 1 ms counter the engine's busy waits time themselves by (sys.ms_timer, 16 bit): spans are `uint16_t(r_ms_timer() - then)`, under 65 s, never
// compared as readings. Without the 3.3 member it is the low half of sys.ms (5 ms steps), which wraps the same way.
uint16_t r_ms_timer(void);

// ---- memory: api->mem, the library's own account ---------------------------------------------------------------------------------------------------------
static inline void *r_malloc(uint32_t n) { return g_api->mem->malloc(n); }
static inline void r_free(void *p) { if (p) { g_api->mem->free(p); } }
void *r_calloc(uint32_t n, uint32_t size);

// ---- text ------------------------------------------------------------------------------------------------------------------------------------------------
// snprintf of the conversions this library writes: %d %i %u %x %X %c %s %%, flags '-' and '0', a width, a precision for %s, the length modifiers l and ll
// (64 bit). Returns the length the text would have had, as snprintf does.
int r_vsnprintf(char *dst, int cap, const char *fmt, va_list ap);
int r_snprintf(char *dst, int cap, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
// A line into api->log (the base adds the line end; a trailing '\n' in fmt is dropped).
void r_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int r_strcasecmp(const char *a, const char *b);
const char *r_strstr(const char *hay, const char *needle);
int r_atoi(const char *s);
int64_t r_strtoll(const char *s);              // decimal, an optional sign
// At most n characters of s into out (outlen bytes, NUL included): a string precision without a format.
void r_copy(char *out, int outlen, const char *s, size_t n);

// ---- the TRX IO block (FW io/c64/trx_io.h, trx_io.cc) ----------------------------------------------------------------------------------------------------
// api->trx->regs is the block, byte wide; the register macros of this library are offsets into it (TRX_IO_BASE 0). Without the block (api->trx null)
// every read gives 0 and every write is dropped.
#define TRX_IO_BASE        0u
#define TRX_ID             (TRX_IO_BASE + 0x000)
#define TRX_CAPS           (TRX_IO_BASE + 0x004)
#define TRX_CAPS2          (TRX_IO_BASE + 0x005)
#define TRX_CAPS_VIDEO     0x40 // page 6: pixel layer and still capture
#define TRX_CAPS2_F4       0x01 // the drain writes stream 2's F4 only after an F3 marker
#define TRX_CAPS2_APPLY    0x20 // b5: the engine applies stream 0 records itself (AP_START/AP_COUNT/AP_CMD)
#define TRX_CAPS2_TURBO    0x40 // b6: the turbo engine (T29): TURBO_STAT, TURBO_W1/W2
#define TRX_CAPS2_TRACE    0x80 // b7: the instruction trace former (LOG_CTRL b4/b9, LOG_CNT[3], T21 §4.11.1)
#define TRX_TURBO_STAT     (TRX_IO_BASE + 0x140) // b30 engine on, b28 ran since cleared, b22:16 CPU cycles per PHI2
#define TRX_TURBO_CTRL     (TRX_IO_BASE + 0x144) // W b0 clears "ran"
#define TRX_TURBO_W1       (TRX_IO_BASE + 0x148) // {$D031 b31:24, $D030 bit 0 b11, enable word b10:8, PREFER b7:0}
#define TRX_TURBO_W2       (TRX_IO_BASE + 0x14C) // reset hold left in PHI2 cycles
#define TRX_TURBO_ON       (1u << 30)
#define TRX_TURBO_RAN      (1u << 28)

uint8_t trx_io_caps(void);                     // CAPS, 0 without the block
uint8_t trx_rd8(uint32_t reg);
void trx_wr8(uint32_t reg, uint8_t v);
// The 32-bit registers over the byte bus: writing byte 3 commits bytes 0-2, reading byte 0 takes bytes 1-3; the four accesses are one critical section
// (sync.enter / sync.leave), as the firmware's trx_io_read32 / trx_io_write32 make them.
uint32_t trx_io_read32(uint32_t reg);
void trx_io_write32(uint32_t reg, uint32_t v);
// The slim core: TX_CTRL (page 6, +0C0) b31:24 reads 2 or more; false without page 6.
bool trx_io_slim(void);

// ---- the C64 machine's registers (c64u_regs.h) through the io group (base33.h); without the group a read gives 0 and a write is dropped --------------
uint8_t io_cart_rd(uint32_t reg);                // the cartridge-port control block
void io_cart_wr(uint32_t reg, uint8_t v);
uint8_t io_bus_rd(uint32_t off);                 // the bus bridge's block
void io_stop(bool on);                           // C64_STOP: the app's stop holds the machine
void io_memonly(bool on);                        // C64_DMA_MEMONLY: the memory window sees RAM only (true) or the C64's memory map

// ---- the C64's memory window (api->c64) ------------------------------------------------------------------------------------------------------------------
static inline uint8_t c64_peek(uint32_t a) { return g_api->c64->peek(uint16_t(a)); }
static inline void c64_poke(uint32_t a, uint8_t v) { g_api->c64->poke(uint16_t(a), v); }
static inline void c64_set_mode(uint8_t m) { g_api->c64->cart_mode(m); }   // C64_MODE: C64U_MODE_NORMAL, C64U_MODE_ULTIMAX

// ---- files: api->fs with a position, as the firmware's File reads and writes ----------------------------------------------------------------------------
struct RFile {
    struct SvcFile *h;
    uint64_t pos;
};
bool rf_open(RFile *f, const char *path);                      // to read
bool rf_create(RFile *f, const char *path);                    // a new empty file to write (one that is there is replaced)
bool rf_read(RFile *f, void *buf, uint32_t len);               // len bytes from the position on, all or nothing
bool rf_write(RFile *f, const void *buf, uint32_t len);        // the same; at the end it makes the file longer
static inline bool rf_seek(RFile *f, uint64_t pos) { f->pos = pos; return true; }   // as FatFs f_lseek: true
uint64_t rf_size(RFile *f);
void rf_close(RFile *f);
bool rf_remove(const char *path);
bool rf_exists(const char *path);

#endif
