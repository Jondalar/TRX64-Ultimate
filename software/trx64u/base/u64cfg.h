// The C64U's own hardware configuration, written by the trx64u library (T104.2 UI, Alex 2026-10-28 decision 5; U18: the library, not the core application):
// video, audio mixer, SID sockets and addressing, joystick, turbo, serial bus, LEDs, power. Gideon's U64Config::effectuate_settings() reads a settings store
// and writes the registers of the U64 block; this is the same sequence reading the core's settings (the core's .cfg, the menu.toml groups video, audio, sid,
// input, expert).
//
// One operation per group, each takes the setting's value (ignored: the group is written as a whole, as Gideon's effectuate does, so a value changed by
// REST, by the menu or at the start gives the same registers). PROVENANCE: vendor/gideon/software/u64/u64_config.cc (Gideon Zweijtzer, GPL-3.0), the
// register sequences and tables of its effectuate_settings functions; the keys are the item names of its stores.
#ifndef TRX64U_U64CFG_H
#define TRX64U_U64CFG_H

#include <stdint.h>
#include "ops.h"

// ---- the handlers of the library's record: lib_trx64u.cc gives them their names and arguments
int fx_video(CoreCtx *c, const OpArgs &a, OpOut &o);
int fx_audio(CoreCtx *c, const OpArgs &a, OpOut &o);
int fx_sid(CoreCtx *c, const OpArgs &a, OpOut &o);
int fx_fsid(CoreCtx *c, const OpArgs &a, OpOut &o);
int fx_chip(CoreCtx *c, const OpArgs &a, OpOut &o);
int fx_arm_read1(CoreCtx *c, const OpArgs &a, OpOut &o);
int fx_arm_read2(CoreCtx *c, const OpArgs &a, OpOut &o);
int fx_fsid_q10(CoreCtx *c, const OpArgs &a, OpOut &o);
int fx_fsid_q11(CoreCtx *c, const OpArgs &a, OpOut &o);
int fx_fsid_q20(CoreCtx *c, const OpArgs &a, OpOut &o);
int fx_fsid_q21(CoreCtx *c, const OpArgs &a, OpOut &o);
int fx_misc(CoreCtx *c, const OpArgs &a, OpOut &o);
int fx_input(CoreCtx *c, const OpArgs &a, OpOut &o);
int op_palette_apply(CoreCtx *c, const OpArgs &a, OpOut &o);
int op_palette_default(CoreCtx *c, const OpArgs &a, OpOut &o);
int op_power_off(CoreCtx *c, const OpArgs &a, OpOut &o);
int op_power_cycle(CoreCtx *c, const OpArgs &a, OpOut &o);
const char *state_palette(CoreCtx *c);
const char *state_palettes(CoreCtx *c);
const char *state_sid1(CoreCtx *c);
const char *state_sid2(CoreCtx *c);
const char *state_s1_swin(CoreCtx *c);
const char *state_s2_swin(CoreCtx *c);
const char *state_s1_arm(CoreCtx *c);
const char *state_s2_arm(CoreCtx *c);
const char *state_s1_pd(CoreCtx *c);
const char *state_s2_pd(CoreCtx *c);
const char *state_s1_kick(CoreCtx *c);
const char *state_s2_kick(CoreCtx *c);
const char *state_s1_noarm(CoreCtx *c);
const char *state_s2_noarm(CoreCtx *c);
const char *state_s1_armver(CoreCtx *c);
const char *state_s2_armver(CoreCtx *c);
const char *state_fsid1_uid(CoreCtx *c);
const char *state_fsid2_uid(CoreCtx *c);
const char *state_fsid1_rev(CoreCtx *c);
const char *state_fsid2_rev(CoreCtx *c);
const char *state_fsid1_none(CoreCtx *c);
const char *state_fsid2_none(CoreCtx *c);
const char *state_fsid1_new(CoreCtx *c);
const char *state_fsid2_new(CoreCtx *c);
const char *state_sid_map0(CoreCtx *c);
const char *state_sid_map1(CoreCtx *c);
const char *state_sid_map2(CoreCtx *c);
const char *state_sid_map3(CoreCtx *c);

// The mixer's registers are write only: the SIDs are silenced through them and given back from the settings (Gideon u64_mute_sids / unmute). Nothing calls
// the two (nor did anything in the core application at TRX64U-MultiCore 3b8e2955): a freeze hook of this library would.
void u64cfg_mute_sids(void);
// T228 R21 (FW u64_config.cc:1277-1283): CTRL+P / CTRL+N held at power-on = PAL / NTSC for this start only (the setting is overridden, never stored). Once per start.
void u64cfg_power_on_keys(CoreCtx *c);
void u64cfg_restore_mixer(CoreCtx *c);

// The command interface's palette and SID map commands (uci.cc through UciHost): what 0 reads the 16 colours, 1 sets all, 2 sets colour idx from rgb[0], 3 resets; the SID map bytes.
void u64cfg_palette(CoreCtx *c, int what, uint8_t idx, uint8_t (*rgb)[3]);
int u64cfg_sid_map(CoreCtx *c, uint8_t *data);

// Which SID sits in a socket (the last detection, 0 none, 1 6581, 2 8580, ...) and the names for the screen.
const char *u64cfg_sid_type_name(int type);

// The SID player's autoconfig (T228 R21 item 7, FW U64Config::SidAutoConfig): `count` SIDs (1 to 8) the tune asks for, each with its base (0x40 = $D400)
// and type (1 6581, 2 8580, 3 either) are mapped on the sockets and the emulated SIDs, the mixer follows. The machine must be stopped. False: no mapping
// was found (every SID is left unmapped). True also when the setting "SID Player Autoconfig" is off (nothing is done then).
bool u64cfg_sid_autoconfig(CoreCtx *c, int count, const uint8_t *base, const uint8_t *type);

// The USB HID driver's mouse items (usb_hid.cc reads them by this C name, weak there).
extern "C" int u64_get_usb_hid_config_value(int key, int default_value);

#endif
