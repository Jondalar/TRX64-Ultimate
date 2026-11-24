// The machine a state file describes, read by the ring library itself (U18, MultiCore's decision "Machine info and menu close", 2026-11-22): the media
// in the descriptor's slots (ctx.media_source), the cartridge in the port (export "c64cart"), drive A (export "cbm1541") and the System Mode (the video
// format, c64_video of c64_basic's TOML A, through the driver table). Every dump, save, .trxring and load gets it from ring_machine_build(), whether the menu's operations or the
// exported table started it (ringfile.cc trx_c64re_source_begin).
#ifndef RING_RINGMACHINE_H
#define RING_RINGMACHINE_H

#include <stdint.h>
#include "trx_c64re.h"

// The media slots of the C64 descriptor, the names ctx.media_source takes (TRX64U-MultiCore coreapp/cores/c64.cc, MountSlots `slot_names`, branch
// t239.3-boundary d6afb393). The stored mounts ("Mount Drive A=", "Mount Cartridge=") are keyed by the same names. The C64 descriptor has no drive B.
#define RING_SLOT_CARTRIDGE "Cartridge"
#define RING_SLOT_DRIVE_A   "Drive A"

struct RingMachine {
    TrxC64reSrc src;            // what the files read; its strings point into the buffers below
    char cart_path[256];        // the .crt of the cartridge slot, "" when none
    char disk_path[256];        // the image in drive A's slot, "" when none
    uint8_t eeprom[2048];       // a GMod2's 93C86 for a dump (src.eeprom points here when it was read)
};

// Fills m for a dump or a save (load false: the cartridge's image is read through src.rom_read, a GMod2's EEPROM is read now), or for a load (load true:
// the hooks a load builds the machine with instead). It reads registers through io: call it on a task that holds the core lock (an operation, a hook,
// a poll).
void ring_machine_build(RingMachine *m, bool load);

#endif
