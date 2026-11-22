// The C64U's register blocks this library writes directly: the C64 core's I/O block, the cartridge registers, the ROM aperture, the U64 block, the mixers, the
// palettes and the command interface's slot registers. The library is the C64's code (U18 D3/R4: the C64 table is ours), and base 3.1 has no group for these
// blocks (SvcC64 has the memory window, the cartridge mode and the port's GAME/EXROM, SvcTrx the TRX block), so they are reached at their bus addresses as the
// core application reached them. PROVENANCE: the addresses are those of TRX64U-MultiCore 3b8e2955 host-app/software/system/iomap.h, u64.h and u2p.h, which are
// Gideon's (vendor/gideon/software/system, GPL-3.0); the six helpers at the end are coreapp/src/hw.cc HwIo's ram_memonly, cart_rd, cart_wr, bus_wr, stop,
// mode_reset and rom_write of that commit.
#ifndef TRX64U_C64U_REGS_H
#define TRX64U_C64U_REGS_H

#include <stdint.h>

#define IOBASE             0x04000000u
#define C64_CARTREGS_BASE  (IOBASE + 0x40000u)
#define CMD_IF_BASE        (IOBASE + 0x44000u)

#define U2P_IO_BASE        0xA0000000u
#define U2PIO_SPEAKER_EN   (*(volatile uint8_t *)(U2P_IO_BASE + 0x0C))
#define U64_IO_BASE        (U2P_IO_BASE + 0x0400u)
#define U64_AUDIO_MIXER    (U2P_IO_BASE + 0x0500u)
#define U64_SPEAKER_MIXER  (U2P_IO_BASE + 0x0540u)
#define U64_RESAMPLER      (U2P_IO_BASE + 0x0580u)
#define VID_IO_BASE        (U2P_IO_BASE + 0x40000u)
#define U64II_HDMI_PALETTE (VID_IO_BASE + 0x5000u)
#define C64_IO_BASE        (U2P_IO_BASE + 0x80000u)
#define C64_PALETTE        (U2P_IO_BASE + 0x80800u)
#define U64_ROMS_BASE      (U2P_IO_BASE + 0x88000u)

#define U64_USERPORT_EN    (*(volatile uint8_t *)(U64_IO_BASE + 0x05))
#define U64_HDMI_ENABLE    (*(volatile uint8_t *)(U64_IO_BASE + 0x08))
#define U64_INT_CONNECTORS (*(volatile uint8_t *)(U64_IO_BASE + 0x09))
#define U64_PWM_DUTY       (*(volatile uint8_t *)(U64_IO_BASE + 0x0D))
#define U64_CASELED_SELECT (*(volatile uint8_t *)(U64_IO_BASE + 0x0E))

#define U64_RESAMPLE_DATA  (*(volatile uint32_t *)(U64_RESAMPLER + 0x00))
#define U64_RESAMPLE_RESET (*(volatile uint8_t *)(U64_RESAMPLER + 0x04))
#define U64_RESAMPLE_LABOR (*(volatile uint8_t *)(U64_RESAMPLER + 0x08))
#define U64_RESAMPLE_FLUSH (*(volatile uint8_t *)(U64_RESAMPLER + 0x09))

#define C64_SCANLINES      (*(volatile uint8_t *)(C64_IO_BASE + 0x00))
#define C64_VIDEOFORMAT    (*(volatile uint8_t *)(C64_IO_BASE + 0x01))
#define C64_TURBOREGS_EN   (*(volatile uint8_t *)(C64_IO_BASE + 0x02))
#define C64_DMA_MEMONLY    (*(volatile uint8_t *)(C64_IO_BASE + 0x03))
#define C64_PHASE_INCR     (*(volatile uint8_t *)(C64_IO_BASE + 0x05))
#define C64_SID1_BASE      (*(volatile uint8_t *)(C64_IO_BASE + 0x08))
#define C64_SID2_BASE      (*(volatile uint8_t *)(C64_IO_BASE + 0x09))
#define C64_EMUSID1_BASE   (*(volatile uint8_t *)(C64_IO_BASE + 0x0A))
#define C64_EMUSID2_BASE   (*(volatile uint8_t *)(C64_IO_BASE + 0x0B))
#define C64_SID1_MASK      (*(volatile uint8_t *)(C64_IO_BASE + 0x0C))
#define C64_SID2_MASK      (*(volatile uint8_t *)(C64_IO_BASE + 0x0D))
#define C64_EMUSID1_MASK   (*(volatile uint8_t *)(C64_IO_BASE + 0x0E))
#define C64_EMUSID2_MASK   (*(volatile uint8_t *)(C64_IO_BASE + 0x0F))
#define C64_SID1_EN        (*(volatile uint8_t *)(C64_IO_BASE + 0x11))
#define C64_SID2_EN        (*(volatile uint8_t *)(C64_IO_BASE + 0x12))
#define C64_PADDLE_EN      (*(volatile uint8_t *)(C64_IO_BASE + 0x13))
#define C64_STEREO_ADDRSEL (*(volatile uint8_t *)(C64_IO_BASE + 0x14))
#define C64_PADDLE_SWAP    (*(volatile uint8_t *)(C64_IO_BASE + 0x1A))
#define C64_EMUSID1_WAVES  (*(volatile uint8_t *)(C64_IO_BASE + 0x20))
#define C64_EMUSID2_WAVES  (*(volatile uint8_t *)(C64_IO_BASE + 0x21))
#define C64_EMUSID1_RES    (*(volatile uint8_t *)(C64_IO_BASE + 0x22))
#define C64_EMUSID2_RES    (*(volatile uint8_t *)(C64_IO_BASE + 0x23))
#define C64_EMUSID1_DIGI   (*(volatile uint8_t *)(C64_IO_BASE + 0x27))
#define C64_EMUSID2_DIGI   (*(volatile uint8_t *)(C64_IO_BASE + 0x28))
#define C64_EMUSID_SPLIT   (*(volatile uint8_t *)(C64_IO_BASE + 0x29))
#define C64_SPEED_PREFER   (*(volatile uint8_t *)(C64_IO_BASE + 0x2D))
#define C64_SPEED_UPDATE   (*(volatile uint8_t *)(C64_IO_BASE + 0x2E))

#define VIDEO_FMT_RGB_OUTPUT 0x04

// The C64's cartridge registers (C64_CARTREGS_BASE): +0 MODE, +1 STOP (b1 read: the machine has stopped), +2 STOP_MODE, +3 b4 the reset is seen, +8 REU enable,
// +9 REU size, +0xE sampler enable. The bus bridge's registers are in the C64 I/O block (+0x2A BUS_BRIDGE, +0x2B BUS_INTERNAL, +0x2C BUS_EXTERNAL).
#define C64_CARTREG_MODE     0x00
#define C64_CARTREG_STOP     0x01
#define C64_CARTREG_REU      0x08
#define C64_CARTREG_REU_SIZE 0x09
#define C64_CARTREG_SAMPLER  0x0E
#define C64_BUSREG_BRIDGE    0x2A
#define C64_BUSREG_INTERNAL  0x2B
#define C64_BUSREG_EXTERNAL  0x2C
#define C64_MODE_RESET       0x04
#define C64_MODE_UNRESET     0x08

// The ROM aperture (U64_ROMS_BASE): BASIC at +0, KERNAL at +0x2000, each 8 KB; CHAR 4 KB.
#define C64_ROM_KERNAL   0x2000u
#define C64_BASIC_BYTES  8192u
#define C64_KERNAL_BYTES 8192u
#define C64_CHAR_BYTES   4096u

// The command interface's slot registers: +0 the base ($DF1C is 0x47), +1 enable.
#define CMD_IF_SLOT_BASE_REG   (*(volatile uint8_t *)(CMD_IF_BASE + 0x0))
#define CMD_IF_SLOT_ENABLE_REG (*(volatile uint8_t *)(CMD_IF_BASE + 0x1))

// C64_DMA_MEMONLY: on, every byte of the memory window goes to the RAM's second port, $D000-$DFFF included.
static inline void c64_ram_memonly(bool on) { C64_DMA_MEMONLY = on ? 1 : 0; }
static inline uint8_t c64_cart_rd(uint32_t reg) { return *(volatile uint8_t *)(C64_CARTREGS_BASE + reg); }
static inline void c64_cart_wr(uint32_t reg, uint8_t v) { *(volatile uint8_t *)(C64_CARTREGS_BASE + reg) = v; }
static inline void c64_bus_wr(uint32_t off, uint8_t v) { *(volatile uint8_t *)(C64_IO_BASE + off) = v; }
// C64_STOP and C64_MODE: the machine stopped / running, held in reset / let go.
static inline void c64_stop(bool on) { c64_cart_wr(C64_CARTREG_STOP, on ? 1 : 0); }
static inline void c64_mode_reset(bool on) { c64_cart_wr(C64_CARTREG_MODE, on ? C64_MODE_RESET : C64_MODE_UNRESET); }
// The ROM aperture takes one byte per access.
static inline void c64_rom_write(uint32_t off, const uint8_t *p, uint32_t n)
{
    volatile uint8_t *d = (volatile uint8_t *)(U64_ROMS_BASE + off);
    for (uint32_t i = 0; i < n; i++) {
        d[i] = p[i];
    }
}

#endif
