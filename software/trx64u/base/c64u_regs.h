// The C64U's registers this library reads and writes, as offsets: into the blocks the base's io group reaches by member (svc_io.h, base 3.3: rd / wr of
// the core window, cart_rd / cart_wr, bus_wr, stop, mode_reset, ram_memonly, video_format, rom_read / rom_write) and into the register windows of the
// bitstream that io.window hands out by name.
// They are ours (U18; MultiCore, round 2's open points: the blocks outside the core window are windows of our API TOML, name / address / size: desc.toml)
// and no other source file of the library names a register or an address; the windows' addresses are in desc.toml. The numbers are the bitstream's as
// the firmware names them (software/ultimate-app/software/system/u64.h, u2p.h, io/c64/c64.h). The ring library names the C64U_CART_* and C64U_BUS_*
// offsets it shares with this file in ../ring/c64u_regs.h: the same names, the same values.
// The io helpers that take these offsets (c64_cart_rd, c64_bus_wr, c64u_win, ...) are in trx64u_lib.h.
#ifndef TRX64U_C64U_REGS_H
#define TRX64U_C64U_REGS_H

#include <stdint.h>

// ---- the cartridge-port control block: io.cart_rd / io.cart_wr (FW C64_CARTREGS_BASE) -------------------------------------------------------------
#define C64U_CART_MODE        0x0   // C64_MODE (written by io.mode_reset for the reset, by cart_wr for the mapping)
#define C64U_CART_STOP        0x1   // C64_STOP: written through io.stop; read: b1 the machine has stopped
#define C64U_CART_STOP_MODE   0x2   // C64_STOP_MODE
#define C64U_CART_RESET_SEEN  0x3   // b4: the machine's reset line is still seen (FW c64_reset_detect)
#define C64U_CART_REU_ENABLE  0x8   // C64_REU_ENABLE
#define C64U_CART_REU_SIZE    0x9   // C64_REU_SIZE
#define C64U_CART_SAMPLER     0xE   // C64_SAMPLER_ENABLE

#define C64U_STOPPED          0x02  // C64U_CART_STOP read: the machine has stopped (FW C64_HAS_STOPPED)
#define C64U_STOP_COND_FORCE  0x02  // C64U_CART_STOP_MODE: stop at once
#define C64U_RESET_SEEN       0x10  // C64U_CART_RESET_SEEN
#define C64U_MODE_ULTIMAX     0x02  // C64_MODE: the I/O range forced on

// ---- the C64 I/O block, the bus bridge's block of io.bus_wr (FW C64_IO_BASE) ----------------------------------------------------------------------
#define C64U_BUS_SCANLINES      0x00
#define C64U_BUS_VIDEOFORMAT    0x01  // C64_VIDEOFORMAT: written through io.video_format
#define C64U_BUS_TURBOREGS_EN   0x02
#define C64U_BUS_PHASE_INCR     0x05
#define C64U_BUS_SID1_BASE      0x08
#define C64U_BUS_SID2_BASE      0x09
#define C64U_BUS_EMUSID1_BASE   0x0A
#define C64U_BUS_EMUSID2_BASE   0x0B
#define C64U_BUS_SID1_MASK      0x0C
#define C64U_BUS_SID2_MASK      0x0D
#define C64U_BUS_EMUSID1_MASK   0x0E
#define C64U_BUS_EMUSID2_MASK   0x0F
#define C64U_BUS_SID1_EN        0x11
#define C64U_BUS_SID2_EN        0x12
#define C64U_BUS_PADDLE_EN      0x13
#define C64U_BUS_STEREO_ADDRSEL 0x14
#define C64U_BUS_EMUSID1_WAVES  0x20
#define C64U_BUS_EMUSID2_WAVES  0x21
#define C64U_BUS_EMUSID1_RES    0x22
#define C64U_BUS_EMUSID2_RES    0x23
#define C64U_BUS_EMUSID1_DIGI   0x27
#define C64U_BUS_EMUSID2_DIGI   0x28
#define C64U_BUS_EMUSID_SPLIT   0x29
#define C64U_BUS_BRIDGE         0x2A
#define C64U_BUS_INTERNAL       0x2B
#define C64U_BUS_EXTERNAL       0x2C
#define C64U_BUS_SPEED_PREFER   0x2D
#define C64U_BUS_SPEED_UPDATE   0x2E

#define VIDEO_FMT_RGB_OUTPUT    0x04  // C64_VIDEOFORMAT

// ---- the core window: io.rd / io.wr, one byte each; a 32-bit register is four bytes little endian, byte 3 commits a write, byte 0 latches a read --------
// (trx_sys.v, trx_io.v; the same offsets as MultiCore's coreapp/cores/c64_regs.h, which c64.cc and c64_go.cc used before they moved here)
#define C64U_CAPS             0x004   // b1 the state bus, b3 the debug page
#define C64U_CAPS_STATEBUS    0x02
#define C64U_CAPS_DEBUG       0x08
#define C64U_CAPS2            0x005   // b2 the CPU write path
#define C64U_CAPS2_CPUWRITE   0x04
#define C64U_GATE_CTRL        0x100   // 32 bit; b0 FRZ: C64_STOP pauses the whole machine, not only the CPU
#define C64U_GATE_FRZ         0x01
#define C64U_GATE_STATUS      0x101   // the byte that holds b8 (halted) of GATE_CTRL at bit 0
#define C64U_GATE_HALTED      0x01
#define C64U_DBG_OWNER        0x118   // 32 bit; b0: the debugger owns run control, C64_STOP stops nothing
#define C64U_SB_ADDR          0x1000  // 32 bit; {chip 11:7, word 6:0}
#define C64U_SB_DATA          0x1004  // 32 bit; a read or a write moves SB_ADDR on by one word
#define C64U_SB_STATUS        0x1008  // 32 bit; b0 the gate is shut, b1 a write was refused (a write clears b1)
#define C64U_SB_LOAD          0x100C  // 32 bit; b1: the staged CPU words load at the next opening edge; read: still armed
#define C64U_DBG_CMD          0x3000  // 32 bit
#define C64U_DBG_STATUS       0x3008  // 32 bit
#define C64U_DBG_IPC_SET      0x3018  // 32 bit; {valid 24, opcode 23:16, PC 15:0}: what the debug block reports as the PC
#define C64U_CMD_PAUSE_IB     0x02
#define C64U_CMD_RUN          0x03
#define C64U_ST_SHUT          0x01
#define C64U_ST_AT_IB         0x04

// ---- the ROM aperture: io.rom_write (FW U64_ROMS_BASE): BASIC at +0, KERNAL at +0x2000, each 8 KB; CHAR 4 KB at +0x4000 ------------------------------
#define C64_ROM_BASIC    0x0000u
#define C64_ROM_CHAR     0x4000u
#define C64_ROM_KERNAL   0x2000u
#define C64_BASIC_BYTES  8192u
#define C64_KERNAL_BYTES 8192u
#define C64_CHAR_BYTES   4096u

// ---- the register windows: io.window(name, &size) -----------------------------------------------------------------------------------------------
// Each window is one run of a block of the firmware's map, declared in desc.toml (name, address, size), and its offsets follow. The base refuses a window
// over a register it drives itself, so a block that holds one is split into the runs this library writes: the speaker enable alone of its block
// (FW U2P_IO_BASE: I2C, MDIO, the USB hub's reset are the base's), three runs of the U64 block (FW U64_IO_BASE: power, RESTORE, the HDMI PLL reset, the
// keyboard scan and the case LED select are the base's; the case LEDs are board.case_led_select).
// speaker_enable (FW U2PIO_SPEAKER_EN, U2P_IO_BASE + 0x0C)
#define C64U_SPEAKER_EN         0x00  // b0: the speaker on
// u64_userport (FW U64_USERPORT_EN, U64_IO_BASE + 0x05)
#define C64U_U64_USERPORT_EN    0x00
// u64_hdmi_connectors (FW U64_HDMI_ENABLE, U64_INT_CONNECTORS: U64_IO_BASE + 0x08, + 0x09)
#define C64U_U64_HDMI_ENABLE    0x00
#define C64U_U64_INT_CONNECTORS 0x01
// u64_pwm_duty (FW U64_PWM_DUTY, U64_IO_BASE + 0x0D)
#define C64U_U64_PWM_DUTY       0x00
// audio_mixer (FW U64_AUDIO_MIXER): one byte per channel and side, right then left
#define C64U_MIXER_CHANNELS     10
// speaker_mixer (FW U64_SPEAKER_MIXER): the bitstream decodes four bytes, +0x00, +0x01, +0x04, +0x05 (designs/c64_basic/ultimate_sys.vhd spk_gain 0..3),
// and the speaker plays the highest of them. The window ends at +0x05: +0x10 and +0x11 are registers the base drives (master volume and tone in
// MultiCore's coreapp/src/basereg.def; this bitstream does not decode them), and a window may not cover one.
#define C64U_SPEAKER_GAINS      4
static const uint8_t c64u_speaker_gain[C64U_SPEAKER_GAINS] = { 0x00, 0x01, 0x04, 0x05 };
// resampler (FW U64_RESAMPLER)
#define C64U_RESAMPLE_DATA      0x00  // 32 bit, bytes +0 .. +3
#define C64U_RESAMPLE_RESET     0x04
#define C64U_RESAMPLE_LABOR     0x08
#define C64U_RESAMPLE_FLUSH     0x09
// c64_palette (FW C64_PALETTE): 16 colours of 4 bytes (R, G, B) at +0, their YUV at +0x400
#define C64U_PALETTE_RGB        0x000
#define C64U_PALETTE_YUV        0x400
// hdmi_palette (FW U64II_HDMI_PALETTE): 16 colours of 4 bytes (R, G, B)

// The windows: id, the name desc.toml declares, and the bytes the offsets above need (c64u_win refuses a window that is shorter). A name here and in
// desc.toml is the same string; nothing checks the two against each other at build time.
#define C64U_WINDOWS(X)                                                                         \
    X(SPEAKER_EN, "speaker_enable", C64U_SPEAKER_EN + 1)                                        \
    X(U64_USERPORT, "u64_userport", C64U_U64_USERPORT_EN + 1)                                   \
    X(U64_HDMI_CONNECTORS, "u64_hdmi_connectors", C64U_U64_INT_CONNECTORS + 1)                  \
    X(U64_PWM_DUTY, "u64_pwm_duty", C64U_U64_PWM_DUTY + 1)                                      \
    X(AUDIO_MIXER, "audio_mixer", 2 * C64U_MIXER_CHANNELS)                                      \
    X(SPEAKER_MIXER, "speaker_mixer", 0x05 + 1)                                                 \
    X(RESAMPLER, "resampler", C64U_RESAMPLE_FLUSH + 1)                                          \
    X(C64_PALETTE, "c64_palette", C64U_PALETTE_YUV + 16 * 4)                                    \
    X(HDMI_PALETTE, "hdmi_palette", 16 * 4)

enum {
#define X(id, name, need) C64U_W_##id,
    C64U_WINDOWS(X)
#undef X
    C64U_W_N
};

#endif
