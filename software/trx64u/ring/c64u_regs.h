// The C64 machine registers the ring library reads and writes, as offsets into the windows of the base's io group (svc_io.h, base 3.3). They are
// ours (U18, MultiCore's decision on the ring's list, 2026-11-22) and go into our core's API TOML (D8); no other file of the library names an address
// for them. The numbers are the bitstream's (firmware io/c64/c64.h, system/u64.h, itu).
#ifndef RING_C64U_REGS_H
#define RING_C64U_REGS_H

// ---- the cartridge-port control block: io.cart_rd / io.cart_wr (FW C64_CARTREGS_BASE) -----------------------------------------------------------------
#define C64U_CART_MODE        0x0   // C64_MODE: read with cart_rd; written through c64.cart_mode (the cartridge mapping)
#define C64U_CART_STOP        0x1   // C64_STOP: written through io.stop, which knows it; listed for the API TOML
#define C64U_CART_TYPE        0x5   // C64_CARTRIDGE_TYPE: the type the bitstream's cartridge emulation was given (base 0x1F, variant b7:5)
#define C64U_CART_REU_ENABLE  0x8   // C64_REU_ENABLE
#define C64U_CART_REU_SIZE    0x9   // C64_REU_SIZE
#define C64U_CART_SAMPLER     0xE   // C64_SAMPLER_ENABLE

// C64_MODE values
#define C64U_MODE_NORMAL      0x00
#define C64U_MODE_ULTIMAX     0x02

// ---- the bus bridge's block: io.bus_rd (FW C64_IO_BASE) -----------------------------------------------------------------------------------------------
#define C64U_BUS_VIDEOFORMAT  0x1   // C64_VIDEOFORMAT, the System Mode's bits (u64_config.cc writes the colour timing's mode bits)
#define C64U_BUS_DMA_MEMONLY  0x3   // C64_DMA_MEMONLY: read with bus_rd; written through io.ram_memonly

// C64_VIDEOFORMAT bits (FW u64.h VIDEO_FMT_*)
#define C64U_VIDEO_NTSC_ENCODING 0x01
#define C64U_VIDEO_60_HZ         0x02

// ---- the bitstream's capability bits: board.fpga_caps (FW itu getFpgaCapabilities, CAPAB_*) -------------------------------------------------------------
#define CAPAB_COMMAND_INTF    0x00040000u
#define CAPAB_SAMPLER         0x00200000u
#define CAPAB_EEPROM          0x00400000u

#endif
