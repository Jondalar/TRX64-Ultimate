// The trx64u base library (U18 D5): its entry, its record and what its files share (trx64u_lib.h). The record is one list of operations and one of state
// values (LibRecord takes one of each): those of trx64u_ops.cc and of u64cfg.cc, which registered themselves as two contributors ("trx64u", "u64cfg") in the
// core application. A library has no contributor name of its own to give: the loader registers everything under the library's name, "trx64u".
#include "trx64u_lib.h"
#include "libfmt.h"
#include "trx64u_ops.h"
#include "u64cfg.h"
#include <string.h>

const LibApi *g_api;

void lib_logf(const char *fmt, ...)
{
    char line[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    size_t n = strlen(line);
    if (n && line[n - 1] == '\n') {
        line[n - 1] = 0;
    }
    if (g_api->sys) {
        g_api->sys->log(line);
    }
}

// api->sys->sleep_ms rounds a short wait down to one tick (TRX64U-MultiCore 3b8e2955 coreapp/src/libboard.cc BoardLibHost::delay_ms: below 5 ms it is
// vTaskDelay(1)), and the SID chips' handshakes need their milliseconds: the wait goes on until the clock has moved past ms.
void wait_ms(uint32_t ms)
{
    uint32_t t0 = g_api->sys->ms();
    g_api->sys->sleep_ms(ms);
    while (g_api->sys->ms() - t0 <= ms) {
        g_api->sys->sleep_ms(1);
    }
}

uint8_t *xfer_alloc(uint32_t *cap)
{
    static const uint32_t sizes[2] = { 0x10000, 0x1000 };
    for (int i = 0; i < 2; i++) {
        uint8_t *p = (uint8_t *)g_api->mem->malloc(sizes[i]);
        if (p) {
            *cap = sizes[i];
            return p;
        }
    }
    *cap = 0;
    return 0;
}

// ---- the record -----------------------------------------------------------------------------------------------------------------------------------
static const ArgSpec value_arg[] = { ARG_INT("value", -2000, 2000) };
static const ArgSpec name_arg[] = { ARG_STRING("name", 63) };
static const ArgSpec file_name_arg[] = { ARG_PATH("name") };
static const ArgSpec path_arg[] = { ARG_PATH("path") };
static const ArgSpec save_mp3_args[] = { ARG_INT("drive", 0, 3), ARG_STRING("name", 63) };
static const ArgSpec load_mp3_args[] = { ARG_PATH("path"), ARG_INT("drive", 0, 3) };
static const ArgSpec run_disk_args[] = { ARG_SLOT("slot"), ARG_PATH("path") };
static const ArgSpec sid_args[] = { ARG_PATH("path"), ARG_INT("song", 0, 65535) };
static const ArgSpec dma_prg_args[] = { ARG_PATH("path"), ARG_INT("mode", 0, 2) };
static const ArgSpec dma_mount_args[] = { ARG_PATH("path"), ARG_INT("run", 0, 1) };
static const ArgSpec dma_sidcrt_args[] = { ARG_BLOB("data") };
static const ArgSpec dma_reu_args[] = { ARG_INT("offset", 0, 0xFFFFFF), ARG_BLOB("data") };

static const LibOpDecl ops[] = {
    // trx64u_ops.cc
    { "dma.prg", dma_prg_args, 2, op_dma_prg },
    { "dma.mount", dma_mount_args, 2, op_dma_mount },
    { "dma.crt", path_arg, 1, op_dma_crt },
    { "dma.reu", dma_reu_args, 2, op_dma_reu },
    { "dma.sidcrt", dma_sidcrt_args, 1, op_dma_sidcrt },
    { "dma.power_off", 0, 0, op_dma_power_off },
    { "ui.fx", value_arg, 1, fx_ui },
    { "c64.fx_io", value_arg, 1, fx_io },
    { "c64.fx_roms", value_arg, 1, fx_roms },
    { "u64.fx_power", value_arg, 1, fx_power },
    { "drive.fx_misc", value_arg, 1, fx_drive_misc },
    { "c64.sid_play", sid_args, 2, op_sid_play },
    { "c64.sid_songs", sid_args, 2, op_sid_songs },
    { "c64.sid_valid", sid_args, 2, op_sid_valid },
    { "c64.sid_info", sid_args, 2, op_sid_info },
    { "reu.load", path_arg, 1, op_reu_load },
    { "reu.preload", path_arg, 1, op_reu_preload },
    { "drive.insert_blank", 0, 0, op_insert_blank },
    { "drive.swap", 0, 0, op_swap },
    { "machine.after_roms", 0, 0, op_after_roms },
    { "drive.set_rom_store", path_arg, 1, op_set_rom_store },
    { "drive.unlink", 0, 0, op_unlink },
    { "c64.load_kernal", path_arg, 1, op_load_kernal },
    { "drive.run_disk", run_disk_args, 2, op_run_disk },
    { "machine.save_mp3", save_mp3_args, 2, op_save_mp3 },
    { "reu.load_mp3", load_mp3_args, 2, op_load_mp3 },
    { "machine.pause_toggle", 0, 0, op_pause_toggle },
    { "machine.clear_reboot", 0, 0, op_clear_reboot },
    { "machine.save_ram", name_arg, 1, op_save_ram },
    { "machine.save_reu", name_arg, 1, op_save_reu },
    { "machine.save_cart", name_arg, 1, op_save_cart },
    // u64cfg.cc
    { "u64.fx_video", value_arg, 1, fx_video },
    { "u64.fx_audio", value_arg, 1, fx_audio },
    { "u64.fx_sid", value_arg, 1, fx_sid },
    { "u64.fx_fsid", value_arg, 1, fx_fsid },
    { "u64.fx_chip", value_arg, 1, fx_chip },
    { "u64.arm_read1", 0, 0, fx_arm_read1 },
    { "u64.arm_read2", 0, 0, fx_arm_read2 },
    { "u64.fx_fsid_q10", value_arg, 1, fx_fsid_q10 },
    { "u64.fx_fsid_q11", value_arg, 1, fx_fsid_q11 },
    { "u64.fx_fsid_q20", value_arg, 1, fx_fsid_q20 },
    { "u64.fx_fsid_q21", value_arg, 1, fx_fsid_q21 },
    { "u64.fx_misc", value_arg, 1, fx_misc },
    { "u64.fx_input", value_arg, 1, fx_input },
    { "u64.apply_palette", file_name_arg, 1, op_palette_apply },
    { "u64.set_palette", file_name_arg, 1, op_palette_default },
    { "u64.power_off", 0, 0, op_power_off },
    { "u64.power_cycle", 0, 0, op_power_cycle },
};

static const LibStateDecl states[] = {
    // trx64u_ops.cc
    { "reu.mp3.a", state_mp3_a },
    { "reu.mp3.b", state_mp3_b },
    { "reu.mp3.c", state_mp3_c },
    { "reu.mp3.d", state_mp3_d },
    { "reu.nomp3.a", state_nomp3_a },
    { "reu.nomp3.b", state_nomp3_b },
    { "reu.nomp3.c", state_nomp3_c },
    { "reu.nomp3.d", state_nomp3_d },
    { "reu.on", state_reu_on },
    { "c64.port_taken", state_port_taken },
    { "c64.cartridge", state_cartridge },
    { "c64.cartridge_path", state_cartridge_path },
    { "c64.roms_kernal", state_roms_kernal },
    { "c64.roms_basic", state_roms_basic },
    { "c64.roms_char", state_roms_char },
    { "drive.roms_1541", state_roms_1541 },
    { "machine.pause_label", state_pause_label },
    { "machine.state", state_machine },
    { "machine.suggest.ram", state_suggest_ram },
    { "machine.suggest.reu", state_suggest_reu },
    { "machine.suggest.cart", state_suggest_cart },
    { "machine.suggest.tw", state_suggest_tw },
    { "machine.suggest.ring", state_suggest_ring },
    // u64cfg.cc
    { "u64.palette", state_palette },
    { "u64.palettes", state_palettes },
    { "u64.sid1_type", state_sid1 },
    { "u64.sid2_type", state_sid2 },
    { "u64.s1_swin", state_s1_swin },
    { "u64.s2_swin", state_s2_swin },
    { "u64.s1_arm", state_s1_arm },
    { "u64.s2_arm", state_s2_arm },
    { "u64.s1_pd", state_s1_pd },
    { "u64.s2_pd", state_s2_pd },
    { "u64.s1_kick", state_s1_kick },
    { "u64.s2_kick", state_s2_kick },
    { "u64.s1_noarm", state_s1_noarm },
    { "u64.s2_noarm", state_s2_noarm },
    { "u64.s1_armver", state_s1_armver },
    { "u64.s2_armver", state_s2_armver },
    { "u64.fsid1_uid", state_fsid1_uid },
    { "u64.fsid2_uid", state_fsid2_uid },
    { "u64.fsid1_rev", state_fsid1_rev },
    { "u64.fsid2_rev", state_fsid2_rev },
    { "u64.fsid1_none", state_fsid1_none },
    { "u64.fsid2_none", state_fsid2_none },
    { "u64.fsid1_new", state_fsid1_new },
    { "u64.fsid2_new", state_fsid2_new },
    { "u64.sid_map0", state_sid_map0 },
    { "u64.sid_map1", state_sid_map1 },
    { "u64.sid_map2", state_sid_map2 },
    { "u64.sid_map3", state_sid_map3 },
};

// lib.ld puts this section first, so the entry is at offset 0 of the image
#ifdef __riscv
#define LIB_ENTRY_ATTR __attribute__((visibility("default"), section(".text.lib_register")))
#else
#define LIB_ENTRY_ATTR
#endif
// The groups every handler uses (sys, mem, ops, fs) have to be there; api->trx and api->c64 are read at each use (the base fills them after the load).
extern "C" LIB_ENTRY_ATTR int lib_register(const LibApi *api, LibRecord *out)
{
    if (api->abi_major != U2A_ABI_MAJOR || !api->sys || !api->mem || !api->ops || !api->fs) {
        return 1;
    }
    g_api = api;
    out->ops = ops;
    out->nops = (int)(sizeof(ops) / sizeof(ops[0]));
    out->states = states;
    out->nstates = (int)(sizeof(states) / sizeof(states[0]));
    return 0;
}
