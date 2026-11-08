/*
 * cart_port.h
 *
 *  The platform under the cartridge tools (T95 D5): the C64 bus, the cartridge mode, the port's register block,
 *  time, heap and files. One struct of function pointers, installed once at start-up.
 *
 *  Who fills it: the ABI 2 app (cart_port_u2p.c, out of g_api->c64 / g_api->trx / g_api->sys / g_api->mem /
 *  g_api->fs), a later ABI, and the host harness's mock. Nothing above this header knows which one it got, so
 *  cart_base, cart_flash, cart_dumper and cart_writer hold no ABI call and no #ifdef.
 */

#ifndef CART_PORT_H
#define CART_PORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// FatFs result codes the tools pass around, same values as the firmware
typedef enum {
    FR_OK = 0,
    FR_DISK_ERR = 1,
    FR_DENIED = 7,
    FR_EXIST = 8,
    FR_NOT_ENOUGH_CORE = 17,
    FR_INVALID_PARAMETER = 19
} FRESULT;

// Cartridge mode of the machine's aperture: 0 normal, 2 Ultimax
#define MODE_NORMAL 0

typedef struct
{
    // ---- the C64 bus, through the machine's aperture ----
    uint8_t (*peek)(uint16_t addr);
    void (*poke)(uint16_t addr, uint8_t val);
    int (*dma_read)(uint16_t addr, void *dst, int len);
    void (*cart_mode)(uint8_t mode);
    void (*setup_bus)(void);   // saves the bus registers and opens the aperture
    void (*restore_bus)(void); // puts them back
    uint8_t (*cart_detect)(void);
    int (*cart_preference)(void); // < 0 when the machine does not say
    void (*reset_machine)(void);  // NULL when the platform cannot reset the C64

    // ---- the cartridge port's register block of our core; both NULL on Gideon's ----
    uint8_t (*reg_rd)(uint32_t off);
    void (*reg_wr)(uint32_t off, uint8_t val);
    bool has_regs; // the block answers at all (api->trx with its register window)

    // ---- time ----
    uint32_t (*tick_ms)(void);
    void (*delay_ms)(uint32_t ms);

    // ---- heap ----
    void *(*mem_alloc)(size_t size);
    void *(*mem_realloc)(void *p, size_t size);
    void (*mem_free)(void *p);

    // ---- files; path and name are joined with '/', like FileManager::fopen(path, name) ----
    void *(*fopen)(const char *path, const char *name, const char *mode);
    int (*fread)(void *f, void *buf, int len);
    int (*fwrite)(void *f, const void *buf, int len);
    int (*fseek)(void *f, long offset);
    void (*fclose)(void *f);
    bool (*fexists)(const char *path, const char *name);

    // ---- the firmware console ----
    int (*log)(const char *fmt, ...);
} cart_port_t;

extern const cart_port_t *g_port;

void cart_port_set(const cart_port_t *p);

// The platform's own filler, defined by the platform (cart_port_u2p.c for the ABI 2 app)
const cart_port_t *cart_port_platform(void);

// Debug output to the firmware console, like printf in the firmware tools
#define LOG g_port->log

#endif
