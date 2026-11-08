/*
 * cart_port_u2p.c
 *
 *  The cart_port_t of the ABI 2 app (T95 D5): every call of the cartridge tools into the machine, in one place.
 *  The tools above cart_port.h hold no ABI call, so a later ABI is another file like this one and nothing else.
 *
 *  The port's register block is the one exception that knows about the host test: the harness puts a model of
 *  trx_sys.v behind the registers, which no ABI pointer can carry.
 */

#include "cart_port.h"
#include "ui_shim.h"

static uint8_t p_peek(uint16_t addr) { return g_api->c64->peek(addr); }
static void p_poke(uint16_t addr, uint8_t val) { g_api->c64->poke(addr, val); }
static int p_dma_read(uint16_t addr, void *dst, int len) { return g_api->c64->dma_read(addr, dst, len); }
static void p_cart_mode(uint8_t mode) { g_api->c64->cart_mode(mode); }
static void p_setup_bus(void) { g_api->c64->cart_setup_bus(); }
static void p_restore_bus(void) { g_api->c64->cart_restore_bus(); }
static uint8_t p_cart_detect(void) { return g_api->c64->cart_detect(); }
static int p_cart_preference(void) { return g_api->c64->cart_preference(); }
static void p_reset_machine(void) { g_api->media->reset(); }

#ifdef HOST_TEST
// The harness's model of the slot block (host/mock.c)
extern uint8_t mock_trx_rd8(uint32_t off);
extern void mock_trx_wr8(uint32_t off, uint8_t v);
static uint8_t p_reg_rd(uint32_t off) { return mock_trx_rd8(off); }
static void p_reg_wr(uint32_t off, uint8_t val) { mock_trx_wr8(off, val); }
#else
static uint8_t p_reg_rd(uint32_t off) { return g_api->trx->regs[off]; }
static void p_reg_wr(uint32_t off, uint8_t val) { g_api->trx->regs[off] = val; }
#endif

static uint32_t p_tick_ms(void) { return g_api->sys->tick_ms(); }
static void p_delay_ms(uint32_t ms) { g_api->sys->delay_ms(ms); }

static void *p_mem_alloc(size_t size) { return g_api->mem->malloc(size); }
static void *p_mem_realloc(void *p, size_t size) { return g_api->mem->realloc(p, size); }
static void p_mem_free(void *p) { g_api->mem->free(p); }

// path and name are joined with '/', like FileManager::fopen(path, name)
static void *p_fopen(const char *path, const char *name, const char *mode)
{
    char full[256];
    int n = 0;
    while (path[n] && n < 250) {
        full[n] = path[n];
        n++;
    }
    if (n > 0 && full[n - 1] != '/') {
        full[n++] = '/';
    }
    for (int i = 0; name[i] && n < 255; i++) {
        full[n++] = name[i];
    }
    full[n] = 0;
    return g_api->fs->fopen(full, mode);
}

static int p_fread(void *f, void *buf, int len) { return g_api->fs->fread(buf, 1, len, f); }
static int p_fwrite(void *f, const void *buf, int len) { return g_api->fs->fwrite(buf, 1, len, f); }
static int p_fseek(void *f, long offset) { return g_api->fs->fseek(f, offset, 0); }
static void p_fclose(void *f) { g_api->fs->fclose(f); }
static bool p_fexists(const char *path, const char *name) { return g_api->fs->exists(path, name) != 0; }

static cart_port_t s_port = {
    .peek = p_peek,
    .poke = p_poke,
    .dma_read = p_dma_read,
    .cart_mode = p_cart_mode,
    .setup_bus = p_setup_bus,
    .restore_bus = p_restore_bus,
    .cart_detect = p_cart_detect,
    .cart_preference = p_cart_preference,
    .reg_rd = p_reg_rd,
    .reg_wr = p_reg_wr,
    .tick_ms = p_tick_ms,
    .delay_ms = p_delay_ms,
    .mem_alloc = p_mem_alloc,
    .mem_realloc = p_mem_realloc,
    .mem_free = p_mem_free,
    .fopen = p_fopen,
    .fread = p_fread,
    .fwrite = p_fwrite,
    .fseek = p_fseek,
    .fclose = p_fclose,
    .fexists = p_fexists,
};

const cart_port_t *cart_port_platform(void)
{
    // The log of the ABI takes the varargs itself, so the tools' LOG is that call, not a copy of it
    s_port.log = g_api->sys->log;
    s_port.has_regs = (g_api->trx != NULL && g_api->trx->regs != NULL);
    s_port.reset_machine = U2P_HAS(g_api->media, reset) ? p_reset_machine : NULL;
    return &s_port;
}
