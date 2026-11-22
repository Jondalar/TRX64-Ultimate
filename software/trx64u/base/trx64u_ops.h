// Operations and states of the TRX64U menu beyond the C64U's registers (trx64u_ops.cc), in the trx64u library (U18).
#ifndef TRX64U_OPS_H
#define TRX64U_OPS_H

#include <stdint.h>
#include "ops.h"

// ---- the handlers of the library's record: lib_trx64u.cc gives them their names and arguments
int fx_ui(CoreCtx *c, const OpArgs &a, OpOut &o);
int fx_io(CoreCtx *c, const OpArgs &a, OpOut &o);
int fx_roms(CoreCtx *c, const OpArgs &a, OpOut &o);
int fx_power(CoreCtx *c, const OpArgs &a, OpOut &o);
int fx_drive_misc(CoreCtx *c, const OpArgs &a, OpOut &o);
int op_dma_prg(CoreCtx *c, const OpArgs &a, OpOut &o);
int op_dma_mount(CoreCtx *c, const OpArgs &a, OpOut &o);
int op_dma_crt(CoreCtx *c, const OpArgs &a, OpOut &o);
int op_dma_reu(CoreCtx *c, const OpArgs &a, OpOut &o);
int op_dma_sidcrt(CoreCtx *c, const OpArgs &a, OpOut &o);
int op_dma_power_off(CoreCtx *c, const OpArgs &a, OpOut &o);
int op_sid_play(CoreCtx *c, const OpArgs &a, OpOut &o);
int op_sid_songs(CoreCtx *c, const OpArgs &a, OpOut &o);
int op_sid_valid(CoreCtx *c, const OpArgs &a, OpOut &o);
int op_sid_info(CoreCtx *c, const OpArgs &a, OpOut &o);
int op_reu_load(CoreCtx *c, const OpArgs &a, OpOut &o);
int op_reu_preload(CoreCtx *c, const OpArgs &a, OpOut &o);
int op_insert_blank(CoreCtx *c, const OpArgs &a, OpOut &o);
int op_swap(CoreCtx *c, const OpArgs &a, OpOut &o);
int op_after_roms(CoreCtx *c, const OpArgs &a, OpOut &o);
int op_set_rom_store(CoreCtx *c, const OpArgs &a, OpOut &o);
int op_unlink(CoreCtx *c, const OpArgs &a, OpOut &o);
int op_load_kernal(CoreCtx *c, const OpArgs &a, OpOut &o);
int op_run_disk(CoreCtx *c, const OpArgs &a, OpOut &o);
int op_save_mp3(CoreCtx *c, const OpArgs &a, OpOut &o);
int op_load_mp3(CoreCtx *c, const OpArgs &a, OpOut &o);
int op_pause_toggle(CoreCtx *c, const OpArgs &a, OpOut &o);
int op_clear_reboot(CoreCtx *c, const OpArgs &a, OpOut &o);
int op_save_ram(CoreCtx *c, const OpArgs &a, OpOut &o);
int op_save_reu(CoreCtx *c, const OpArgs &a, OpOut &o);
int op_save_cart(CoreCtx *c, const OpArgs &a, OpOut &o);
const char *state_mp3_a(CoreCtx *c);
const char *state_mp3_b(CoreCtx *c);
const char *state_mp3_c(CoreCtx *c);
const char *state_mp3_d(CoreCtx *c);
const char *state_nomp3_a(CoreCtx *c);
const char *state_nomp3_b(CoreCtx *c);
const char *state_nomp3_c(CoreCtx *c);
const char *state_nomp3_d(CoreCtx *c);
const char *state_reu_on(CoreCtx *c);
const char *state_port_taken(CoreCtx *c);
const char *state_cartridge(CoreCtx *c);
const char *state_cartridge_path(CoreCtx *c);
const char *state_roms_kernal(CoreCtx *c);
const char *state_roms_basic(CoreCtx *c);
const char *state_roms_char(CoreCtx *c);
const char *state_roms_1541(CoreCtx *c);
const char *state_pause_label(CoreCtx *c);
const char *state_machine(CoreCtx *c);
const char *state_suggest_ram(CoreCtx *c);
const char *state_suggest_reu(CoreCtx *c);
const char *state_suggest_cart(CoreCtx *c);
const char *state_suggest_tw(CoreCtx *c);
const char *state_suggest_ring(CoreCtx *c);

// ---- what the core application calls today (BOUNDARY.md "the other way"); a hook of this library or an export when the core's C64 code moves here
// where a file named in the menu goes: the folder the menu was on, else the first medium; `ext` is added when the name has none
bool trx_target_path(CoreCtx *c, const char *name, const char *ext, char *out, int n);
void trx64u_load_assoc_cfg(CoreCtx *c, const char *path, bool usr_too);   // the .cfg (a PRG: or .usr) beside a file, loaded before it runs

// c64.cc tells the module every KERNAL image it puts into the aperture (the fast reset patch is made against that copy).
void trx64u_kernal_put(CoreCtx *c, const uint8_t *data, uint32_t n);

// The command interface at $DF1C for a cartridge that requires it (c64_cart.cc).
void trx64u_uci_df1c(CoreCtx *c);
// The SID player (T228 R21 item 7): c64.cc hands the BASIC ROM on for the player cartridge's image, the poll of c64.service drives the handshake, a reset of
// the application ends it.
void trx64u_basic_put(CoreCtx *c, const uint8_t *data, uint32_t n);
void trx64u_sid_service(CoreCtx *c);
void trx64u_sid_cancel(void);

#endif
