// The export "uci" (major 1) of trx64u's UCI library (trxuci_exports.cc): the Ultimate Command Interface at $DF1C-$DF1F. The library is linked into trx64u's app
// (../app/toml_b.toml, library "uci"; its bind entry uci_bind adds the export); trx64u's libraries find the table with SvcLib::find("uci", U2A3_VERSION(1, 0),
// &size) (base/lib_trx64u.cc x_table, ring/ringsys.cc). Not a member of LibApi.
//
// The slot registers are the block "cmd_if" of c64_basic's TOML A (slot_base, slot_enable; trxuci_slot.cc). The base library sets the slot from the menu's
// setting (base/trx64u_ops.cc), the ring library for a .c64re (set_slot_enable, set_slot_base 0x47), both through this table.
//
// PROVENANCE: copied from TRX64U-MultiCore t239.3 e8b76719 coreapp/src/uci_api.h (= TRX64 SDK 0.4.5 include/uci_api.h, GPL-3.0). Changes here: the file's
// name (a library's include path has the SDK's include/ first, where a uci_api.h would be found instead of this one), this paragraph, the include guard (a
// file that includes both headers is refused at the compile: they define the same types), and the comments of UciHooks, start and the slot calls, which
// say what trxuci_exports.cc does.
//
// Why no CoreCtx: the functions of trxuci.h take one; the library behind this table keeps the core's context (uci_bind) and uses it. The callbacks of the
// host table (UciHooks) follow: they lose it too.
//
// Rules: `size` first; members only appended inside a major. Task: "lock" = a task holding the core lock.
#ifndef TRX64U_TRXUCI_API_H
#define TRX64U_TRXUCI_API_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef DRVTYPE_MP3_DNP
#define DRVTYPE_MP3_DNP 31                    // io/c64/c64.h, the RAM drive type of a MegaPatch 3 native drive
#endif

// The machine's side of the interface: what only the core knows. The members are those of UciHost in trxuci.h without the context argument and with `size` first.
// The table stays valid for as long as the library that gave it is loaded (a static const of the caller). Every callback runs on the UCI's own task. A null
// callback, or one past the caller's `size`, answers as "the machine has none" (trxuci_exports.cc: reu -2; drive_info false; drive_do 1 with "This machine has
// no drive."; sid_map 0; ef_clear false; ramdrive type, size and base 0; palette nothing).
typedef struct UciHooks {
    uint32_t size;                        // sizeof(UciHooks) of the caller; members past it count as null
    // The REU, for CONTROL LOAD_REU / SAVE_REU: 0 and *size, *offset (bytes) and the image path in img; -2 the REU is not enabled; -4 its memory is in use (busy has the reason).
    int (*reu)(uint32_t *size, uint32_t *offset, char *img, int imgn, char *busy, int busyn);
    // Drive idx (0 = A, 1 = B): false when there is none. type in the encoding of the drive's type register (0 1541, 2 1581), addr the IEC address.
    bool (*drive_info)(int idx, uint8_t *type, uint8_t *addr, bool *power);
    // what: 0 mount path, 1 eject, 2 swap, 3 power off, 4 power on. 0 = done, else the reason is in msg.
    int (*drive_do)(int idx, int what, const char *path, char *msg, int msglen);
    // CONTROL GET_HWINFO device 1 (the SID map): fills data (at most 16 bytes) as the firmware does and returns its length, 0 when the machine has none.
    int (*sid_map)(uint8_t *data);
    // CONTROL U64_SAVEMEM: the file the 64 KB go to when the command names none; null: the library's default.
    const char *memory_file;
    // CONTROL EASYFLASH 0: clears the 8 banks of 8 KB of the cartridge in the port from `bank` (a multiple of 8), the ROMH half when high. False: no cartridge. The caller usually
    // forwards to c64cart.ef_clear.
    bool (*ef_clear)(uint8_t bank, bool high);
    // The MegaPatch 3 RAM drive n (0..3) in the REU: type 0 none, 1541, 1571, 1581 or DRVTYPE_MP3_DNP; size in bytes; base the REU offset of its first byte.
    void (*ramdrive)(int n, int *type, uint32_t *size, uint32_t *base);
    // The palette (CONTROL 0x51-0x54): what 0 reads all 16 colours into rgb, 1 sets all, 2 sets colour idx from rgb[0], 3 resets to the default colours.
    void (*palette)(int what, uint8_t idx, uint8_t (*rgb)[3]);
} UciHooks;

typedef struct UciApi {
    uint32_t size;
    // Starts the command interface once, with the machine's side (`hooks`). Null hooks, or a second call while the first table is in use, do nothing; the
    // interface itself starts at the first call only, and not at all when the bitstream lacks it (uci_start: the ITU's capability bits CAPAB_COMMAND_INTF and
    // CAPAB_CARTRIDGE). When the image that gave the table goes (an unload, a failed load), this library stops calling through it and a later start gives a
    // new one; memory_file is copied. Call it from the core's start hook (core_hooks.h), never from lib_register. Task: lock.
    void (*start)(const UciHooks *hooks);
    // The slot of the interface: whether it answers at $DF1C (SLOT_ENABLE, 0 or 1) and the base address it answers at (SLOT_BASE; 0x47 is $DF1C). slot_enable
    // reads the register; set_slot_enable and set_slot_base write it. The rule of this export (trxuci_slot.cc uci_slot_present): the writes are ignored and
    // slot_enable reads 0 unless the running core's descriptor lists the capability "uci" and, when the core has a board profile, its probe BC_UCI holds.
    // Task: lock.
    uint8_t (*slot_enable)(void);
    void (*set_slot_enable)(uint8_t v);
    void (*set_slot_base)(uint8_t v);
} UciApi;

#ifdef __cplusplus
}
#endif

#endif
