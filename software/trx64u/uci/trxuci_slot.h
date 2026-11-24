// The slot of the Ultimate Command Interface ($DF1C): the part of the UCI library that writes the block "cmd_if" of the driver table (drvtab.h; a core block
// of c64_basic's TOML A). The base's server writes slot_base once more by its own constant when trxuci.cc's uci_start starts it (command_intf.o
// CommandInterface::start: 0x47); nothing else in the app writes the two registers. trxuci.cc is the rest of the library.
// PROVENANCE: copied from TRX64U-MultiCore t239.3 e8b76719 coreapp/src/uci_slot.h (= TRX64 SDK 0.4.5 base/inc/coreapp/src/uci_slot.h, GPL-3.0). Changes
// here: the file's name, this paragraph and the include guard.
#ifndef TRX64U_TRXUCI_SLOT_H
#define TRX64U_TRXUCI_SLOT_H

#include "ctx.h"

// The core lists the capability "uci" and the board did not strike it (board_effective_caps, BC_UCI): the rule of every call below that takes a context.
bool uci_slot_present(const CoreCtx *c);

// The registers themselves, no rule: SLOT_ENABLE is read and written, SLOT_BASE written (0x47 is $DF1C, the map's state "df1c").
uint8_t uci_slot_read_enable(void);
void uci_slot_write_enable(uint8_t v);
void uci_slot_write_base(uint8_t v);

#endif
