// The slot registers of trx64u's UCI library (trxuci_slot.h). PROVENANCE: copied from TRX64U-MultiCore t239.3 e8b76719 coreapp/src/uci_slot.cc (= TRX64 SDK
// 0.4.5 base/src/coreapp/src/uci_slot.cc, GPL-3.0). Changes here: the file's name and its header's. The block cmd_if is a core block of c64_basic's TOML A
// (../gateware/c64_basic.toml), with the names of the SDK's map uci.
#include "trxuci_slot.h"
#include "desc.h"
#include "board.h"
#include "drvtab.h"

bool uci_slot_present(const CoreCtx *c)
{
    return c && c->desc && desc_has_capability(c->desc, "uci") && (!c->board || (c->board->probes & BC_UCI));
}

uint8_t uci_slot_read_enable(void) { return DRV_cmd_if_slot_enable_RD(); }
void uci_slot_write_enable(uint8_t v) { DRV_cmd_if_slot_enable_WR(v); }
void uci_slot_write_base(uint8_t v) { DRV_cmd_if_slot_base_WR(v); }
