// What the ring library's files share of the base (U18): the LibApi pointer, the base 3.3 groups (g_api->sync, settings, ctx, io, board), the exports
// of MultiCore's shared libraries, memory on the library's own account, text without a C library, the TRX IO block's registers, the C64 machine's
// registers and memory window, and files. ringsys.cc.
#ifndef RING_RINGSYS_H
#define RING_RINGSYS_H

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <string.h>
#include "libapi.h"
#include "c64cart_api.h"
#include "cbm1541_api.h"
#include "uci_api.h"
#include "drvtab.h"

// Set by lib_register (lib_ring.cc) before anything else runs.
extern const LibApi *g_api;

// The five groups base 3.3 added to LibApi are there on every base this library loads on (LIB_NEEDS base>=3.3; api.md 3.2.12): sync, settings, ctx and
// board always, io on a core with a register window and once the core is handed over. Each is still tested for null before a call (ring.cc's mutex
// calls test `present`, which is set only after sync made the mutex). A member 3.3 appended
// to an older table (SvcSys, SvcFs) is called only when SVC_HAS says the table has it: its size covers the member and the member is set (api.md 3.2.2).
#define SVC_HAS(T, t, m) ((t) && (t)->size >= offsetof(T, m) + sizeof(((T *)0)->m) && (t)->m)

// ---- the exports of MultiCore's shared libraries (SvcLib::find(name, U2A3_VERSION(1, 0), &size), major 1) --------------------------------------------
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

// ---- the registers: the driver table of c64_basic's TOML A (drvtab.h from ../gateware/c64_basic.toml; MultiCore T239.3 D31, api.md 6.17) -----------
// DRV_<block>_<property>_RD() / _WR(v), a run's _RD(i) / _WR(i, v), _<field>_MASK / _SHIFT, _<state>. HW_RD, HW_RD_I and HW_WR take block and
// property as one name and touch the register only once the base has handed the core over (g_api->io); before that a read gives 0 and a write is
// dropped. A 32-bit register is one access: the bus converter makes it four byte accesses, byte 0 first, inside the one instruction (byte 3 commits a
// write, byte 0 latches a read: trx_sys.v), so no other task's access comes between them.
#define HW_RD(bp) (g_api->io ? DRV_##bp##_RD() : 0)
#define HW_RD_I(bp, i) (g_api->io ? DRV_##bp##_RD(i) : 0)
#define HW_WR(bp, v)                                                                                                                         \
    do {                                                                                                                                       \
        if (g_api->io) {                                                                                                                       \
            DRV_##bp##_WR(v);                                                                                                                  \
        }                                                                                                                                      \
    } while (0)
// A 32-bit register by its address, DRV_<block>_<property>_ADDR of an "r" or "rw" property, for a helper that waits on one (ring.cc wait_reg).
static inline uint32_t hw_rd32(uint32_t addr) { return g_api->io ? *DRV_PTR(uint32_t, addr) : 0; }
// The offset of a property in its block, for the io group's calls that take one (cart_rd, cart_wr).
#define DRV_OFF(b, p) ((uint32_t)(DRV_##b##_##p##_ADDR - DRV_##b##_ADDR))

uint8_t trx_io_caps(void);                     // ident.caps, 0 before the hand-over
// The slim core: the text layer's version (trx_text.ctrl b31:24) reads 2 or more; false without the video layers (ident.caps video).
bool trx_io_slim(void);

// The ITU capability bits of board.fpga_caps (the firmware's itu.h CAPAB_*; a library keeps its own, api.md 4.16.2.1).
#define CAPAB_COMMAND_INTF 0x00040000u
#define CAPAB_SAMPLER      0x00200000u
#define CAPAB_EEPROM       0x00400000u

// ---- the base's blocks through the io group (g_api->io); without the group a read gives 0 and a write is dropped ---------------------------------------
// The cartridge port's page (cart_regs) by the io group's cart_rd / cart_wr, the offset from the table: CART_RD(p), CART_WR(p, v). The video format
// (c64_video) and the DMA mode (c64_dma_mode) are read through the table directly (HW_RD); the base writes them.
uint8_t io_cart_rd(uint32_t off);
void io_cart_wr(uint32_t off, uint8_t v);
#define CART_RD(p) io_cart_rd(DRV_OFF(cart_regs, p))
#define CART_WR(p, v) io_cart_wr(DRV_OFF(cart_regs, p), (uint8_t)(v))
void io_stop(bool on);                           // the base's stop: the app's stop holds the machine
void io_memonly(bool on);                        // the DMA mode: the memory window sees RAM only (true) or the C64's memory map

// ---- the C64's memory window (api->c64) ------------------------------------------------------------------------------------------------------------------
static inline uint8_t c64_peek(uint32_t a) { return g_api->c64->peek(uint16_t(a)); }
static inline void c64_poke(uint32_t a, uint8_t v) { g_api->c64->poke(uint16_t(a), v); }
static inline void c64_set_mode(uint8_t m) { g_api->c64->cart_mode(m); }   // cart_regs.mode: DRV_cart_regs_mode_normal, _ultimax

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
