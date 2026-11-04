// Writable cartridges on the TRX64 Ultimate bitstream. See trx_cart.h.
//
// Hardware (TRX64-Ultimate designs/c64_basic/trx_io.v): IO block IOBASE + 0x1A0000,
// byte wide, multi-byte registers little endian.
//   page 0: +000 ID "TRX2", +004 CAPS (b4 cartridge page), +010 IRQ_EN,
//           +014 IRQ_CAUSE (W1C; b4 flash operation pending, b5 EEPROM written)
//   page 4: +000 FL_ENABLE, +004 FL_MAGIC1, +008 FL_MAGIC2, +00C FL_MASK (16 bit),
//           +010..012 FL_ID (manufacturer, device, device ID address),
//           +020..023 EVT (reading byte 0 latches 1-3 and 024): [31:30] 1 program,
//           2 sector erase, 3 chip erase, [29] chip, [28:8] chip address, [7:0] data,
//           +024 EVT_HI (b1:0 chip address bits 22:21, EasyFlash 3's 8 MiB, T51),
//           +028 CMD (b1 drop the cartridge's cached lines, b0 ACK),
//           +02C..02D WR_COUNT (reading byte 0 latches byte 1),
//           +038 XPORT_CAPS (R: b0 8 MiB image window, b1 EasyFlash 3; older bitstreams read 0),
//           +030 ROM_SEL (b1:0, where the image is read from: 0 Gideon's 0x03C00000, 1 the
//           expansion-port block 0x01000000, 2 the built-in cartridge slot 0x00F00000),
//           +040..064 DESC: the cartridge's mapping descriptor (T33); the bitstream
//           knows no cartridge type, the descriptor is all it maps by. It takes
//           a copy at the C64's reset and at a cartridge force (+06 b1), so a
//           descriptor written meanwhile changes nothing yet.
// The interrupt is ITU irq_in(4), level: the line UCI uses on Gideon's bitstreams.
// The TRX bitstream has no UCI; trx_io.cc owns the line and dispatches by cause.

#include "trx_cart.h"
#include "trx_cart_desc.h"
#include "trx_flash_geom.h"
#include "trx_io.h"
#include "itu.h"
#include "c64.h"
#include "u64.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stdio.h>
#include <string.h>

#define TRX_CART_BASE    (TRX_IO_BASE + 0x4000)
#define TRX_FL_ENABLE    (TRX_CART_BASE + 0x000)
#define TRX_FL_MAGIC1    (TRX_CART_BASE + 0x004)
#define TRX_FL_MAGIC2    (TRX_CART_BASE + 0x008)
#define TRX_FL_MASK      (TRX_CART_BASE + 0x00C)
#define TRX_FL_ID        (TRX_CART_BASE + 0x010)
#define TRX_EVT          (TRX_CART_BASE + 0x020)
#define TRX_EVT_HI       (TRX_CART_BASE + 0x024)
#define TRX_CMD          (TRX_CART_BASE + 0x028)
#define TRX_WR_COUNT     (TRX_CART_BASE + 0x02C)
#define TRX_ROM_SEL      (TRX_CART_BASE + 0x030)
#define TRX_XPORT_CAPS   (TRX_CART_BASE + 0x038)
#define TRX_XCAPS_8MB    0x01
#define TRX_XCAPS_EF3    0x02
#define TRX_DESC         (TRX_CART_BASE + 0x040)
#define TRX_CMD_ACK      0x01
#define TRX_CMD_INVAL    0x02

#define EVT_PROGRAM      1
#define EVT_SECTOR_ERASE 2
#define EVT_CHIP_ERASE   3

#define TRX_QUIET_MS     2000
#define TRX_MAX_AGE_MS   30000

// The image: up to 8 MiB (EasyFlash 3), 16 KiB per bank, 8 KiB blocks in the dirty map.
#define IMAGE_SIZE       0x800000
#define BLOCK_SHIFT      13
#define MAP_WORDS        ((IMAGE_SIZE >> BLOCK_SHIFT) / 32)

// The descriptor and the flash part per CART_TYPE: trx_cart_desc.h, generated from
// TRX64-Ultimate tools/cart_descriptors/*.json (T33, T16.2 §7.2).
static const trx_cart_desc_t *find_desc(uint8_t cart_type)
{
    for (unsigned i = 0; i < sizeof(c_cart_descs) / sizeof(c_cart_descs[0]); i++) {
        if (c_cart_descs[i].cart_type == cart_type) {
            return &c_cart_descs[i];
        }
    }
    return NULL;
}

static int present = -1; // not looked yet
static const trx_flash_part_t *profile;
static bool writable;

static volatile uint32_t pend_map[MAP_WORDS];
static uint32_t flight_map[MAP_WORDS];
static volatile bool ee_pending, ee_flight;
static volatile bool any_pending;
static volatile bool flush_requested;

static volatile TickType_t first_tick, last_tick;

bool trx_cart_present(void)
{
    if (present < 0) {
        present = (trx_io_caps() & TRX_CAPS_CART) != 0;
        printf("[TRX] cartridge write-back hardware %s\n", present ? "present" : "absent");
    }
    return present;
}

bool trx_ef3_present(void)
{
    static int ef3 = -1; // not looked yet
    if (ef3 < 0) {
        ef3 = trx_cart_present() && (ioRead8(TRX_XPORT_CAPS) & TRX_XCAPS_EF3) && (ioRead8(TRX_XPORT_CAPS) & TRX_XCAPS_8MB);
        printf("[TRX] EasyFlash 3 banking (8 MiB image window): %s\n", ef3 ? "present" : "absent");
    }
    return ef3 != 0;
}

bool trx_xport_shared(void)
{
    static int shared = -1; // not looked yet
    if (shared < 0) {
        shared = 0;
        if (trx_cart_present()) {
            if (ioRead8(TRX_ROM_SEL) & 3) {
                shared = 1; // a bitstream without the register never reads anything but 0
            } else {
                ioWrite8(TRX_ROM_SEL, 1); // the image base 0 -> 1 -> 0 with no cartridge running
                shared = (ioRead8(TRX_ROM_SEL) == 1);
                ioWrite8(TRX_ROM_SEL, 0);
            }
        }
        printf("[TRX] expansion-port block shared by REU and cartridge image: %s\n", shared ? "yes" : "no (Gideon's layout)");
    }
    return shared != 0;
}

void trx_cart_select_rom(bool builtin)
{
    if (!trx_xport_shared()) {
        return;
    }
    ioWrite8(TRX_ROM_SEL, builtin ? 2 : 1);
    ioWrite8(TRX_CMD, TRX_CMD_INVAL);
}

uint16_t trx_cart_wr_count(void)
{
    if (!trx_cart_present()) {
        return 0;
    }
    uint8_t lo = ioRead8(TRX_WR_COUNT);     // latches byte 1
    uint8_t hi = ioRead8(TRX_WR_COUNT + 1);
    return (uint16_t(hi) << 8) | lo;
}

static inline uint32_t image_offset(uint32_t a, int chip)
{
    return trx_image_offset(profile, a, chip);
}

static inline void mark(uint32_t offset)
{
    uint32_t block = offset >> BLOCK_SHIFT;
    pend_map[block >> 5] |= (1UL << (block & 31));
}

// Erased flash reads $FF. Every sector is a whole number of 8 KiB pieces, and
// each piece is contiguous in the image.
static volatile uint32_t *erase_range(uint8_t *image, uint32_t start, uint32_t size, int chip)
{
    volatile uint32_t *p = NULL;
    for (uint32_t a = start; a < start + size; a += 0x2000) {
        uint32_t off = image_offset(a, chip);
        p = (volatile uint32_t *)(image + off);
        for (int i = 0; i < 0x2000 / 4; i++) {
            p[i] = 0xFFFFFFFF;
        }
        mark(off);
    }
    return p;
}

static void note_change_isr(void)
{
    TickType_t now = xTaskGetTickCountFromISR();
    if (!any_pending) {
        first_tick = now;
    }
    last_tick = now;
    any_pending = true;
}

// Short and allocation free. An operation stays pending (the C64 sees the chip
// busy) until the ACK, so nothing new can arrive while this applies one.
static uint8_t trx_cart_irq(uint8_t act)
{
    if (act & TRX_IRQ_EEPROM) {
        ioWrite8(TRX_IRQ_CAUSE, TRX_IRQ_EEPROM);
        ee_pending = true; // the 93C86 applied it; the copy is the writer's, under the seqlock
        note_change_isr();
    }
    if (act & TRX_IRQ_FLASH) {
        // Cleared before the ACK: the next operation cannot be started before it.
        ioWrite8(TRX_IRQ_CAUSE, TRX_IRQ_FLASH);
        uint32_t evt = ioRead8(TRX_EVT); // latches bytes 1-3
        evt |= uint32_t(ioRead8(TRX_EVT + 1)) << 8;
        evt |= uint32_t(ioRead8(TRX_EVT + 2)) << 16;
        evt |= uint32_t(ioRead8(TRX_EVT + 3)) << 24;
        uint32_t evt_hi = ioRead8(TRX_EVT_HI) & 3; // chip address bits 22:21 (0 on older bitstreams)
        int cmd = evt >> 30;
        if (cmd == 0) {
            return 0; // acknowledged by trx_cart_configure already
        }
        if (profile) {
            uint8_t *image = C64 :: get_cartridge_rom_addr();
            int chip = (profile->chips == 2) ? ((evt >> 29) & 1) : 0;
            uint32_t a = (((evt >> 8) & 0x1FFFFF) | (evt_hi << 21)) & (profile->chip_size - 1);
            volatile uint32_t *last = NULL;
            volatile uint8_t *last8 = NULL;
            if (cmd == EVT_PROGRAM) {
                uint32_t off = image_offset(a, chip);
                last8 = image + off;
                *last8 &= uint8_t(evt); // a program only clears bits
                mark(off);
            } else { // sector erase, or chip erase: the whole chip, the EasyFlash 3's system area included
                uint32_t start, size;
                trx_erase_span(profile, cmd == EVT_CHIP_ERASE, a, &start, &size);
                last = erase_range(image, start, size, chip);
            }
            // The rvlite core has no data cache; its stores are posted to the memory
            // controller, though, and the ACK goes over the IO bus. A load from the
            // last address written waits until the store ahead of it is done.
            __asm__ volatile ("fence" ::: "memory");
            if (last8) {
                (void)*last8;
            } else if (last) {
                (void)*last;
            }
            note_change_isr();
        }
        ioWrite8(TRX_CMD, TRX_CMD_INVAL | TRX_CMD_ACK);
    }
    return 0;
}

void trx_cart_configure(uint8_t cart_type)
{
    if (!trx_cart_present()) {
        return;
    }
    // Whatever the previous cartridge left pending is not this image's: acknowledge it unapplied.
    trx_irq_enable(TRX_IRQ_FLASH | TRX_IRQ_EEPROM, false);
    profile = NULL;
    writable = false;
    ioWrite8(TRX_IRQ_CAUSE, TRX_IRQ_FLASH | TRX_IRQ_EEPROM);
    ioWrite8(TRX_CMD, TRX_CMD_INVAL | TRX_CMD_ACK);

    portENTER_CRITICAL();
    memset((void *)pend_map, 0, sizeof(pend_map));
    memset(flight_map, 0, sizeof(flight_map));
    ee_pending = ee_flight = false;
    any_pending = false;
    flush_requested = false;
    portEXIT_CRITICAL();

    // The mapping: the type's descriptor, or none (the bitstream keeps the cartridge off).
    const trx_cart_desc_t *desc = find_desc(cart_type);
    for (int i = 0; i < TRX_CART_DESC_BYTES; i++) {
        ioWrite8(TRX_DESC + i, desc ? desc->desc[i] : 0);
    }
    if (cart_type && !desc) {
        printf("[TRX] cartridge type %02x: no descriptor, the cartridge stays off\n", cart_type);
    }
    profile = desc ? desc->flash : NULL;
    if (!profile) {
        ioWrite8(TRX_FL_ENABLE, 0);
        return;
    }
    ioWrite8(TRX_FL_ENABLE, 0);
    ioWrite8(TRX_FL_MAGIC1 + 0, uint8_t(profile->magic1));
    ioWrite8(TRX_FL_MAGIC1 + 1, uint8_t(profile->magic1 >> 8));
    ioWrite8(TRX_FL_MAGIC2 + 0, uint8_t(profile->magic2));
    ioWrite8(TRX_FL_MAGIC2 + 1, uint8_t(profile->magic2 >> 8));
    ioWrite8(TRX_FL_MASK + 0, uint8_t(profile->mask));
    ioWrite8(TRX_FL_MASK + 1, uint8_t(profile->mask >> 8));
    ioWrite8(TRX_FL_ID + 0, profile->id_mfr);
    ioWrite8(TRX_FL_ID + 1, profile->id_dev);
    ioWrite8(TRX_FL_ID + 2, profile->id_ofs);
    ioWrite8(TRX_FL_ENABLE, 1);
    writable = true;

    trx_irq_attach(TRX_IRQ_FLASH | TRX_IRQ_EEPROM, trx_cart_irq);
    trx_irq_enable(TRX_IRQ_FLASH | TRX_IRQ_EEPROM, true);
    printf("[TRX] cartridge type %02x: %s, flash write-back on\n", cart_type, profile->part);
}

bool trx_cart_writable(void)
{
    return writable;
}

bool trx_cart_pending(void)
{
    return writable && any_pending;
}

bool trx_cart_due(void)
{
    if (!trx_cart_pending()) {
        return false;
    }
    TickType_t now = xTaskGetTickCount();
    return flush_requested ||
           ((now - last_tick) >= pdMS_TO_TICKS(TRX_QUIET_MS)) ||
           ((now - first_tick) >= pdMS_TO_TICKS(TRX_MAX_AGE_MS));
}

void trx_cart_request_flush(void)
{
    flush_requested = true;
}

void trx_cart_hold(bool on)
{
    if (writable) {
        trx_irq_enable(TRX_IRQ_FLASH, !on);
    }
}

void trx_cart_begin_writeback(void)
{
    portENTER_CRITICAL();
    for (int i = 0; i < MAP_WORDS; i++) {
        flight_map[i] |= pend_map[i];
        pend_map[i] = 0;
    }
    ee_flight = ee_flight || ee_pending;
    ee_pending = false;
    any_pending = false;
    flush_requested = false;
    portEXIT_CRITICAL();
}

void trx_cart_end_writeback(bool ok)
{
    portENTER_CRITICAL();
    if (!ok) {
        bool any = ee_flight;
        for (int i = 0; i < MAP_WORDS; i++) {
            pend_map[i] |= flight_map[i];
            any = any || flight_map[i];
        }
        ee_pending = ee_pending || ee_flight;
        if (any && !any_pending) {
            first_tick = last_tick = xTaskGetTickCount();
        }
        any_pending = any_pending || any;
    }
    memset(flight_map, 0, sizeof(flight_map));
    ee_flight = false;
    portEXIT_CRITICAL();
}

void trx_cart_forget(void)
{
    trx_cart_begin_writeback();
    trx_cart_end_writeback(true);
}

// ---- the expansion port's write gate ---------------------------------------------------------------
// SLOT_CTRL (page 7 +000) b1 ROM_WR_EN, SLOT_STATUS (+004) b18 its value, b31 the slot block. 32-bit
// registers: the write commits on byte 3.
#define TRX_SLOT_CTRL    (TRX_IO_BASE + 0x7000)
#define TRX_SLOT_STATUS  (TRX_IO_BASE + 0x7004)
#define TRX_SLOT_WR_EN   0x02u
#define TRX_SLOT_WR_BIT  (1u << 18)
#define TRX_SLOT_BLOCK   (1u << 31)

bool trx_slot_present(void)
{
    static int slot = -1;
    if (slot < 0) {
        slot = ((trx_io_caps() != 0) && (trx_io_read32(TRX_SLOT_STATUS) & TRX_SLOT_BLOCK)) ? 1 : 0;
    }
    return slot > 0;
}

bool trx_slot_armed(void)
{
    return trx_slot_present() && (trx_io_read32(TRX_SLOT_STATUS) & (1u << 16));
}

// the last value the policy set: ConfigureU64SystemBus runs often, the bus is not touched for nothing
static int applied = -1;
static void trx_slot_forget(void) { applied = -1; }

void trx_slot_apply_policy(int internal, int external)
{
    if (!trx_slot_present()) {
        return;
    }
    // Writes always reach the expansion port, as on a real C64 (Alex, 2026-10-04: no write
    // protection; the "Cartridge Port Writes" policy is gone).
    (void)internal;
    (void)external;
    bool want = true;
    if ((int)want == applied) {
        return;
    }
    bool have = (trx_io_read32(TRX_SLOT_STATUS) & TRX_SLOT_WR_BIT) != 0;
    if (want != have) {
        trx_io_write32(TRX_SLOT_CTRL, want ? TRX_SLOT_WR_EN : 0);
    }
    applied = want;
}

void trx_slot_apply_policy_now(void)
{
    if (trx_slot_present()) {
        trx_slot_forget();
        trx_slot_apply_policy(C64_BUS_INTERNAL, C64_BUS_EXTERNAL);
    }
}
