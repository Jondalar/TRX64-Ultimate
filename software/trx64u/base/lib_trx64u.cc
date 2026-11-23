// The trx64u base library (U18 D5): its entry, its record, its core hooks and what its files share (trx64u_lib.h). The record is one list of operations and
// one of state values (LibRecord takes one of each): those of trx64u_ops.cc, of u64cfg.cc and of c64core.cc (the C64 descriptor's code), which registered
// themselves as three contributors ("trx64u", "u64cfg", "c64") in the core application. A library has no contributor name of its own to give: the loader
// registers everything under the library's name, "trx64u".
#include "trx64u_lib.h"
#include "libfmt.h"
#include "core_hooks.h"
#include "c64u_regs.h"
#include "trx64u_ops.h"
#include "u64cfg.h"
#include "c64core.h"
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

int op_call(const char *name, const OpIn *in, int nin, char *msg, int msglen)
{
    OpResult r;
    r.msg[0] = 0;
    int rc = g_api->ops->call(name, in, nin, &r);
    if (msg && msglen > 0) {
        snprintf(msg, msglen, "%s", r.msg);
    }
    return rc;
}

// ---- the exports of the shared libraries ----------------------------------------------------------------------------------------------------------
// A table is kept once found: the loaded libraries stay. `size` is the smaller of what find reports and the table's own first word. Two tasks may look
// at the same time; either order of the two stores leaves an entry that answers null or the table, never a wrong size.
static struct {
    const char *name;
    const void *t;
    uint32_t size;
} g_x[X_N] = { { "c64cart", 0, 0 }, { "cbm1541", 0, 0 }, { "uci", 0, 0 }, { "sid", 0, 0 }, { "tempdisk", 0, 0 } };

const void *x_table(int which, uint32_t off)
{
    if (which < 0 || which >= X_N) {
        return 0;
    }
    if (!g_x[which].t && g_api->lib) {
        uint32_t n = 0;
        const void *t = g_api->lib->find(g_x[which].name, U2A3_VERSION(1, 0), &n);
        if (t && n >= sizeof(uint32_t)) {
            uint32_t own = *(const uint32_t *)t;
            g_x[which].size = own < n ? own : n;
            g_x[which].t = t;
        }
    }
    const char *t = (const char *)g_x[which].t;
    if (!t || g_x[which].size < off + sizeof(void *)) {
        return 0;
    }
    return *(void *const *)(t + off) ? t : 0;
}

// ---- the register windows (c64u_regs.h C64U_WINDOWS, desc.toml, io.window) ---------------------------------------------------------------------------
// The windows are the core's description (desc.toml in record 6 of the .cor), so a window found stays. One that is missing is looked for again at the next
// use (the io group may come later than the first call) and logged once.
static const struct {
    const char *name;
    uint32_t need;
} g_win_def[C64U_W_N] = {
#define X(id, name, need) { name, (uint32_t)(need) },
    C64U_WINDOWS(X)
#undef X
};
static volatile uint8_t *g_win[C64U_W_N];
static bool g_win_said[C64U_W_N];

volatile uint8_t *c64u_win(int w)
{
    if (w < 0 || w >= C64U_W_N) {
        return 0;
    }
    if (g_win[w]) {
        return g_win[w];
    }
    const SvcIo *io = b33_io();
    uint32_t size = 0;
    volatile uint8_t *p = 0;
    if (io && io->size >= offsetof(SvcIo, window) + sizeof(io->window) && io->window) {
        p = io->window(g_win_def[w].name, &size);
    }
    if (p && size >= g_win_def[w].need) {
        g_win[w] = p;
        return p;
    }
    if (!g_win_said[w]) {
        g_win_said[w] = true;
        if (p) {
            lib_logf("[trx64u] register window \"%s\" is %u bytes, %u needed: its registers are not written", g_win_def[w].name, (unsigned)size,
                     (unsigned)g_win_def[w].need);
        } else {
            lib_logf("[trx64u] no register window \"%s\" (io group, desc.toml or address map): its registers are not written", g_win_def[w].name);
        }
    }
    return 0;
}

// ---- the core hooks (core_hooks.h, the export "corehooks") ----------------------------------------------------------------------------------------
// start: the command interface with this machine's side (uci.start, which does nothing when the bitstream lacks it) and the ring library's engine
// ("ring.init", idempotent; no ring library: nothing; "ring.start" is the log's start, not this). A failure of the ring is the answer: the base logs
// it and tells the person.
static int hook_start(char *msg, int msglen)
{
    const UciApi *u = X_GET(X_UCI, UciApi, start);
    if (u) {
        u->start(trx64u_uci_hooks());
    }
    char m[OP_MSG_MAX];
    int rc = op_call("ring.init", 0, 0, m, sizeof(m));
    if (rc != OPE_OK && rc != OPE_NOT_FOUND) {
        snprintf(msg, msglen, "Timewarp ring: %s", m);
        return 1;
    }
    return 0;
}

// menu_closed: what the debugger held is given back, then the ring lands its pending frame and lets the machine go on (core_hooks.h: the menu task, the
// machine still stopped, no core lock; the monitor's when_frozen poll and an executor operation may run meanwhile). The hook keeps no state of its own; the
// two operations guard what they share with their own tasks. A library that is not loaded has no operation: OPE_NOT_FOUND is no failure.
static void hook_menu_closed(void)
{
    static const char *const ops[2] = { "monitor.release", "ring.unfreeze" };
    char m[OP_MSG_MAX];
    for (int i = 0; i < 2; i++) {
        int rc = op_call(ops[i], 0, 0, m, sizeof(m));
        if (rc != OPE_OK && rc != OPE_NOT_FOUND) {
            lib_logf("[trx64u] %s: %s", ops[i], m);
        }
    }
}

// base_setting_changed: the base applied one of its own settings, `key` its key (core_hooks.h: after each change, and after the start hook once for each
// base-owned setting; lock task). Of those, this core's hardware follows "Joystick Input": its paddle swap (u64cfg.cc); the keyboard joystick is the
// base's. Keys compare without case, as the base's settings do.
static void hook_base_setting_changed(const char *key)
{
    if (key && !strcasecmp(key, "Joystick Input")) {
        u64cfg_paddle_swap();
    }
}

// init, release, media_flush, media_pending: the C64 descriptor's (c64core.cc, from MultiCore's c64.cc, whose descriptor had no warm_reset and no cold_start).
static const CoreHooks core_hooks = { sizeof(CoreHooks), c64_hook_init, c64_hook_release, 0, 0, c64_hook_media_flush, c64_hook_media_pending, hook_start,
                                      hook_menu_closed, hook_base_setting_changed };

// The library's named exports (base 3.2, LibRecord::export_list); the Makefile's LIB_EXPORTS lists the same names and majors, and the loader refuses the
// library when lib_register does not return each of them.
static const LibExportDecl exports[] = {
    { "corehooks", 1, &core_hooks, sizeof(core_hooks) },
};

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
static const ArgSpec rom_arg[] = { ARG_BLOB("data") };
static const ArgSpec freeze_arg[] = { ARG_INT("value", 0, 1) };
static const ArgSpec load_args[] = { ARG_PATH("path"), ARG_INT("start", 0, 0xFFFF) };
static const ArgSpec t64_args[] = { ARG_PATH("path"), ARG_INT("index", 0, 255) };
static const ArgSpec rom_set_args[] = { ARG_PATH("path"), ARG_INT("which", 0, 2) };
static const ArgSpec text_arg[] = { ARG_STRING("text", 63) };
static const ArgSpec dma_write_args[] = { ARG_INT("address", 0, 0xFFFF), ARG_BLOB("data") };
static const ArgSpec mem_read_args[] = { ARG_INT("address", 0, 0xFFFF), ARG_INT("length", 1, OP_DATA_MAX) };
static const ArgSpec mem_write_args[] = { ARG_INT("address", 0, 0xFFFF), ARG_STRING("data", 254) };

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
    { "u64.apply_palette", file_name_arg, 1, op_palette_apply },
    { "u64.set_palette", file_name_arg, 1, op_palette_default },
    { "u64.power_off", 0, 0, op_power_off },
    { "u64.power_cycle", 0, 0, op_power_cycle },
    // c64core.cc: what desc.toml names (put_op, service_op, type_op, run_op), the program and cartridge runs of the menu, the base's "Run with App" (MultiCore
    // PRG_APP_OP), the Library's T64 list (menuvlist.cc), the DMA service (dmasvc.cc) and the machine's memory (MultiCore memops.h)
    { "machine.read_mem", mem_read_args, 2, c64_mem_read },
    { "machine.write_mem", mem_write_args, 2, c64_mem_write },
    { "c64.put_basic", rom_arg, 1, c64_put_basic },
    { "c64.put_kernal", rom_arg, 1, c64_put_kernal },
    { "c64.put_char", rom_arg, 1, c64_put_char },
    { "c64.set_freeze_mode", freeze_arg, 1, c64_freeze_mode },
    { "c64.run_prg", path_arg, 1, c64_run },
    { "files.run_with_app", path_arg, 1, c64_run_with_app },
    { "c64.load_prg", load_args, 2, c64_load },
    { "c64.run_crt", path_arg, 1, c64_run_crt },
    { "c64.dma_prg", path_arg, 1, c64_dma },
    { "dma.write", dma_write_args, 2, c64_dma_write },
    { "dma.kernal", rom_arg, 1, c64_put_kernal },
    { "c64.run_t64", t64_args, 2, c64_run_t64 },
    { "c64.dma_t64", t64_args, 2, c64_dma_t64 },
    { "c64.set_rom", rom_set_args, 2, c64_set_rom },
    { "c64.remove_cart", 0, 0, c64_remove_cart },
    { "c64.service", 0, 0, c64_service },
    { "c64.type_script", text_arg, 1, c64_type },
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
    // c64core.cc
    { "c64.program", c64_prg_name },
    { "c64.pending", c64_prg_pending },
    { "c64.program_path", c64_prg_path },
};

// lib.ld puts this section first, so the entry is at offset 0 of the image
#ifdef __riscv
#define LIB_ENTRY_ATTR __attribute__((visibility("default"), section(".text.lib_register")))
#else
#define LIB_ENTRY_ATTR
#endif
// The groups every handler uses (sys, mem, ops, fs) have to be there; api->trx, api->c64, the 3.3 groups and the exports are read at each use. The core
// hooks are the export "corehooks", major 1.
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
    out->export_list = exports;
    out->nexport_list = (int)(sizeof(exports) / sizeof(exports[0]));
    return 0;
}
