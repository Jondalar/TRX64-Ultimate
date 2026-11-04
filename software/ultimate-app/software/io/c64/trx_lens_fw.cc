// Memory through a lens for the firmware. See trx_lens_fw.h.
//
// The hardware operations trx_lens.c needs, as the firmware reaches them:
//   state bus   page 1 of the TRX block: SB_ADDR (+000), SB_DATA (+004, a read moves SB_ADDR on)
//   RAM         the DMA window with C64_DMA_MEMONLY (no bus cycle)
//   ROM block   the firmware's BASIC/KERNAL/CHAR apertures
//   drive A     its 64 K image in DDR2 (drive_unit.vhd g_ram_base), written through by the drive
//   lens unit   page 5: LK_REQ (+020), LK_RES (+024); CAPS2 b4

#include "trx_lens_fw.h"
#include "trx_lens.h"
#include "trx_io.h"
#include "u64.h"
#include "c64.h"
#include "FreeRTOS.h"
#include "task.h"
#include "app_api_net.h"
#include <string.h>

#define SB_ADDR        (TRX_IO_BASE + 0x1000)
#define SB_DATA        (TRX_IO_BASE + 0x1004)
#define LK_REQ         (TRX_IO_BASE + 0x5020)
#define LK_RES         (TRX_IO_BASE + 0x5024)
#define TRX_CAPS2_LENS 0x10
#define DRIVE_IMAGE    0x0EE0000u

static void fw_sb_block(int chip, int word, uint32_t *dst, int n)
{
    trx_io_lock(); // SB_ADDR is one pointer for every master of the block
    trx_io_write32(SB_ADDR, ((uint32_t)chip << 7) | (uint32_t)word);
    for (int i = 0; i < n; i++) {
        dst[i] = trx_io_read32(SB_DATA);
    }
    trx_io_unlock();
}

// The window in 256-byte pieces, each one with the interrupts off the way U64Machine does it
// for the C64's memory: C64_DMA_MEMONLY is the firmware's, shared with everything else that reads
// through the window.
static void fw_ram_read(uint16_t a, uint8_t *d, int n)
{
    volatile uint8_t *ram = (volatile uint8_t *)C64_MEMORY_BASE;
    int pos = 0;
    while (pos < n) {
        int chunk = (n - pos) > 256 ? 256 : (n - pos);
        uint8_t saved = C64_DMA_MEMONLY;
        portENTER_CRITICAL();
        C64_DMA_MEMONLY = 1;
        for (int i = 0; i < chunk; i++) {
            d[pos + i] = ram[(uint16_t)(a + pos + i)];
        }
        C64_DMA_MEMONLY = saved;
        portEXIT_CRITICAL();
        pos += chunk;
    }
}

static uint8_t fw_rom(uint16_t off)
{
    if (off < 0x1000) {
        return *(volatile uint8_t *)(U64_CHARROM_BASE + off);
    }
    if ((off >= 0x2000) && (off < 0x4000)) {
        return *(volatile uint8_t *)(U64_KERNAL_BASE + (off - 0x2000));
    }
    if ((off >= 0x4000) && (off < 0x6000)) {
        return *(volatile uint8_t *)(U64_BASIC_BASE + (off - 0x4000));
    }
    return 0xFF;
}

static int fw_drive_peek(uint16_t a)
{
    if (a < 0x1800) {
        return *(volatile uint8_t *)(uintptr_t)(DRIVE_IMAGE + (a & 0x07FFu)); // 2 K RAM, mirrored
    }
    if (a < 0x2000) {
        return -1; // the VIAs are not on the state bus
    }
    return *(volatile uint8_t *)(uintptr_t)(DRIVE_IMAGE + a);
}

static uint32_t fw_lk(uint16_t a, int cls, int real)
{
    trx_io_write32(LK_REQ, (uint32_t)a | ((uint32_t)(cls & 3) << 16) | (real ? (1u << 18) : 0));
    for (int n = 0; n < 100000; n++) {
        uint32_t r = trx_io_read32(LK_RES);
        if (!(r & LK_BUSY)) {
            return r;
        }
    }
    return 0;
}

static bool has_lens_unit(void)
{
    return (trx_io_caps() != 0) && ((ioRead8(TRX_CAPS2) & TRX_CAPS2_LENS) != 0);
}

static lens_hw_t s_hw = { fw_sb_block, fw_ram_read, fw_rom, fw_drive_peek, fw_lk, 0, 0 };

bool trx_lens_present(void) { return (trx_io_caps() & TRX_CAPS_STATEBUS) != 0; }

bool trx_lens_busy(void) { return app_trx_rpc_port() != 0; }

bool trx_chip_words(int chip, int word, uint32_t *dst, int n)
{
    if (!trx_lens_present() || trx_lens_busy()) {
        return false;
    }
    fw_sb_block(chip, word, dst, n);
    return true;
}

bool trx_lens_valid(const char *lens) { return lens_valid(lens) != 0; }

bool trx_lens_needs_pause(const char *lens)
{
    return !strcmp(lens, "cpu") || !strcmp(lens, "io") || !strcmp(lens, "cart");
}

// The lens unit makes a REAL cycle only while the whole machine is paused (LK_REFUSED otherwise).
// The caller's C64_STOP pauses only the CPU unless GATE_CTRL b0 is set (clear after reset and in
// the freeze menu's default), so the read sets b0 for its duration and puts the old value back.
#define GATE_CTRL      (TRX_IO_BASE + 0x0100)
#define GATE_PAUSE     0x001
#define GATE_HALTED    0x100

int trx_lens_read(const char *lens, uint16_t addr, uint8_t *buffer, uint32_t len, bool sidefx)
{
    s_hw.lk = has_lens_unit() ? fw_lk : 0;
    uint32_t gate = 0;
    bool set_pause = s_hw.lk && trx_lens_needs_pause(lens) && C64_STOP;
    if (set_pause) {
        gate = trx_io_read32(GATE_CTRL) & ~GATE_HALTED;
        set_pause = !(gate & GATE_PAUSE);
    }
    if (set_pause) {
        trx_io_write32(GATE_CTRL, gate | GATE_PAUSE);
        for (int i = 0; (i < 100) && !(trx_io_read32(GATE_CTRL) & GATE_HALTED); i++) {
            vTaskDelay(1);
        }
    }
    int n = lens_read(&s_hw, lens, addr, buffer, len, sidefx);
    if (set_pause) {
        trx_io_write32(GATE_CTRL, gate);
    }
    return n;
}
