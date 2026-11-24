// The operations and states of the TRX64U menu (menu.toml) that are not the C64U's hardware registers (u64cfg.cc is those): the C64's cartridge port, REU and
// ROM settings, the machine's own actions. The register sequences of the cartridge port are Gideon's (vendor/gideon/software/io/c64/c64.cc
// ConfigureU64SystemBus and set_emulation_flags, GPL-3.0), the fast reset patch is his (c64.cc fastresetPatch).
// U18: built as the trx64u library against the TRX64 SDK (base 3.3). The settings are read through the settings group
// (setting.cc), the files through api->fs, the C64's memory window through api->c64, the registers through the driver table of c64_basic's TOML A
// (trx64u_lib.h), the running core's state, the REU's memory and the board through the 3.3 groups ctx, io and board (g_api->ctx, io, board), the cartridge port,
// drive A, the command interface and the temp disk through the exports of the app's libraries (X_GET; the command interface's is our UCI library's, ../uci). lib_trx64u.cc lists the
// operations and states of this file with their names and arguments.
#include "trx64u_ops.h"
#include "u64cfg.h"
#include "trx64u_lib.h"
#include <string.h>

// ---- the shared libraries' calls this file makes, each answering as "none there" when its library is not loaded ------------------------------------
static bool cart_active(void)
{
    const C64CartApi *c = X_GET(X_CART, C64CartApi, active);
    return c && c->active();
}
static const char *cart_name(void)
{
    const C64CartApi *c = X_GET(X_CART, C64CartApi, name);
    return c ? c->name() : "";
}
static const char *cart_path(void)
{
    const C64CartApi *c = X_GET(X_CART, C64CartApi, path);
    return c ? c->path() : "";
}
// Drive A as it stands (cbm1541.drive_info); false: no drive library or no drive. The strings are the drive library's until the next mount: copy them.
static bool drive_info(Cbm1541Info *info)
{
    const Cbm1541Api *d = X_GET(X_DRIVE, Cbm1541Api, drive_info);
    memset(info, 0, sizeof(*info));
    info->size = sizeof(*info);
    return d && d->drive_info(0, info);
}
// A spec of cbm1541.slot_mount: the path, with "ro " or "unlinked " in front for those modes. Drive A's slot is told changed whatever the answer: the base
// stores what the slot then holds, and writes nothing when that is what it had.
static int drive_mount(const char *path, int mode, char *msg, int msglen)
{
    const Cbm1541Api *d = X_GET(X_DRIVE, Cbm1541Api, slot_mount);
    if (!d) {
        snprintf(msg, msglen, "This machine has no drive A");
        return 1;
    }
    char spec[FS_PATH_MAX + 12];
    snprintf(spec, sizeof(spec), "%s%s", mode == CBM1541_MODE_RO ? "ro " : mode == CBM1541_MODE_UNLINKED ? "unlinked " : "", path);
    return d->slot_mount(spec, msg, msglen);
}
// The command interface's slot (uci.set_slot_enable, then set_slot_base at $DF1C when `at_df1c`); the export ignores both unless the core has the
// capability "uci" and the board's probe found the interface.
static void uci_slot(bool on, bool at_df1c)
{
    const UciApi *u = X_GET(X_UCI, UciApi, set_slot_enable);
    if (u) {
        u->set_slot_enable(on ? 1 : 0);
    }
    u = at_df1c ? X_GET(X_UCI, UciApi, set_slot_base) : 0;
    if (u) {
        u->set_slot_base(0x47);
    }
}

// ---- the temp folder ------------------------------------------------------------------------------------------------------------------------------
// "ui.fx" is the apply of "Temp Auto Cleanup" and "Temp Subfolders": the temp disk's two switches (the export "tempdisk"). The menu's look, the name
// squeeze, the navigation style and the keyboard joystick are settings the base owns and applies (svc_settings.h).
int fx_ui(CoreCtx *, const OpArgs &, OpOut &)
{
    const TempDiskApi *t = X_GET(X_TEMPDISK, TempDiskApi, set);
    if (t) {
        t->set(setting_ix("Temp Auto Cleanup", 1) != 0, setting_ix("Temp Subfolders", 1) != 0);
    }
    return OPE_OK;
}

// ---- the C64's cartridge port, REU, the sampler and the command interface -----------------------------------------------------------------------
static const uint8_t bus_mode_values[] = { 0x00, 0x01, 0x02, 0x03, 0x05 };     // Compatibility has write also enabled (c64.cc bus_mode_values)

// Gideon's fast reset: the KERNAL's RAM test at $FD6C (offset 0x1D6C of the image) is replaced by a short loop that tests one page and sets the top of memory
static const uint8_t fast_patch[22] = { 0xa2, 0x00, 0xa0, 0xa0, 0xad, 0x00, 0x80, 0x49, 0xff, 0x8d, 0x00, 0x80, 0xcd, 0x00, 0x80, 0xf0, 0x02, 0xa0, 0x80, 0x4c, 0x8c, 0xfd };
static const uint8_t fast_org[47] = { 0xe6, 0xc2, 0xb1, 0xc1, 0xaa, 0xa9, 0x55, 0x91, 0xc1, 0xd1, 0xc1, 0xd0, 0x0f, 0x2a, 0x91, 0xc1, 0xd1, 0xc1, 0xd0, 0x08, 0x8a, 0x91, 0xc1, 0xc8, 0xd0, 0xe8, 0xf0, 0xe4, 0x98, 0xaa, 0xa4, 0xc2, 0x18, 0x20, 0x2D, 0xfe, 0xa9, 0x08, 0x8d, 0x82, 0x02, 0xa9, 0x04, 0x8d, 0x88, 0x02, 0x60 };
static uint8_t kernal_copy[DRV_c64_roms_kernal_COUNT];
static bool kernal_known, kernal_patched;

// c64.put_kernal (c64core.cc) calls this with every KERNAL image it puts into the aperture: the copy is what the patch is made against (the aperture is write
// only)
void trx64u_kernal_put(CoreCtx *, const uint8_t *data, uint32_t n)
{
    if (n != DRV_c64_roms_kernal_COUNT) {
        return;
    }
    memcpy(kernal_copy, data, n);
    kernal_known = true;
    kernal_patched = false;
    if (setting_ix("Fast Reset", 0) && !memcmp(kernal_copy + 0x1d6c, fast_org, sizeof(fast_org))) {
        c64_rom_write(DRV_OFF(c64_roms, kernal) + 0x1d6c, fast_patch, sizeof(fast_patch));
        kernal_patched = true;
    }
}

static void fast_reset(void)
{
    if (!kernal_known) {
        return;
    }
    bool want = setting_ix("Fast Reset", 0) != 0 && !memcmp(kernal_copy + 0x1d6c, fast_org, sizeof(fast_org));
    if (want && !kernal_patched) {
        c64_rom_write(DRV_OFF(c64_roms, kernal) + 0x1d6c, fast_patch, sizeof(fast_patch));
        kernal_patched = true;
    } else if (!want && kernal_patched) {
        c64_rom_write(DRV_OFF(c64_roms, kernal) + 0x1d6c, kernal_copy + 0x1d6c, sizeof(fast_patch));
        kernal_patched = false;
    }
}

static int reu_load(const char *path, char *msg, int msglen, bool at_zero = true);
static void mp3_forget(void);
// The slot's status (trx_slot, gateware/c64_basic.toml): `block` the slot block is in the bitstream, `armed` the port is armed for a cartridge. `block` is the
// bitstream's and is read once (the firmware's trx_slot_present keeps it the same way, TREX/1541ultimate:software/io/c64/trx_cart.cc); `armed` each time.
// The register is 32 bit: one read latches it at byte 0 (trx_sys.v), which byte reads of +6 and +7 alone never did.
static bool slot_armed(void)
{
    static int8_t block = -1;
    if (!g_api->io) {
        return false;
    }
    if (block < 0) {
        block = (HW_RD(trx_slot_status) & DRV_trx_slot_status_block_MASK) ? 1 : 0;
    }
    return block && (HW_RD(trx_slot_status) & DRV_trx_slot_status_armed_MASK);
}

// c64.cc:421: the expansion port is taken when a .crt runs, a cartridge was seen in the port at the last bus setup (trx_ext_cart_seen: GAME or EXROM pulled low) or the slot is armed for one.
// The REU, the Ultimate Audio mapping and the UCI are off then (the saved values stay), and the rows say "Disabled (cartridge)". The menu's states read it on
// every pass: the cartridge library's flag and one register byte, the bus setup's finding as fx_io left it.
static bool g_ext_cart_seen;
static bool port_taken(void)
{
    return cart_active() || g_ext_cart_seen || slot_armed();
}

int fx_io(CoreCtx *, const OpArgs &, OpOut &)
{
    g_ext_cart_seen = g_api->c64 && (g_api->c64->cart_detect() & 3) != 3;         // U64_CART_DETECT: GAME and EXROM both high = nothing in the port
    HW_WR(trx_slot_ctrl, DRV_trx_slot_ctrl_rom_wr_en_MASK);                          // T228 R20 29: CPU writes reach a real cartridge (TRX-Ultimate trx_cart.cc:364-406 keeps
                                                                                     // ROM_WR_EN set); one 32-bit write, which commits at byte 3 (a byte write of +0 alone never did)
    HW_WR(c64_bus_bridge, bus_mode_values[setting_ix("Bus Operation Mode", 0)]);
    bool cart = cart_active();
    if (!cart) {
        uint8_t internal = 0, external = 0;
        bool ext_cart = g_ext_cart_seen;
        if (setting_ix("GAME/EXROM", 0) == 1) {                                     // Off: the external cartridge is hidden completely
            internal = 15;
        } else {
            switch (setting_ix("Cartridge Preference", 0)) {
            case 0: internal = ext_cart ? 0 : 15; external = ext_cart ? 15 : 0; break;
            case 1: internal = 15; break;
            case 2: external = 15; break;
            default: {
                static const char *const keys[4] = { "Bus Sharing - ROMs", "Bus Sharing - Interrupts", "Bus Sharing - I/O1", "Bus Sharing - I/O2" };
                static const uint8_t bits[4] = { 0x04, 0x08, 0x01, 0x02 };
                for (int k = 0; k < 4; k++) {
                    int v = setting_ix(keys[k], 2);
                    if (v == 0 || v == 2) {
                        internal |= bits[k];
                    }
                    if (v == 1 || v == 2) {
                        external |= bits[k];
                    }
                }
            }
            }
        }
        HW_WR(c64_bus_internal, internal);
        HW_WR(c64_bus_external, external);
        // one module in the expansion port: the REU, the Ultimate Audio and the UCI are the machine's only while no cartridge runs
        bool taken = port_taken();                                                   // a cartridge in the port (seen or armed): off, whatever the settings say
        CART_WR(reu, !taken && setting_ix("RAM Expansion Unit", 0) == 1 ? DRV_cart_regs_reu_on : DRV_cart_regs_reu_off);
        CART_WR(reu_size, setting_ix("REU Size", 4));
        CART_WR(sampler, taken ? DRV_cart_regs_sampler_off : setting_ix("Map Ultimate Audio $DF20-DFFF", 0));
        uci_slot(!taken && setting_ix("Command Interface (UCI)", 0), true);        // the server itself was started by the start hook (uci.start)
    } else {
        uci_slot(false, false);                                                     // c64.cc:426-445: a running cartridge has the port, the UCI is off (the saved value stays)
    }
    fast_reset();
    static bool preloaded;                                               // "REU Preload": the image goes into the REU once, at the start
    if (!preloaded && !(ctx_state() & CTX_STARTED) && setting_ix("REU Preload", 0) && setting_ix("RAM Expansion Unit", 0) == 1) {
        preloaded = true;
        char m[100];
        const char *img = setting_text("REU Preload Image");
        if (img[0] && reu_load(img, m, sizeof(m), false)) {
            lib_logf("[reu] preload: %s", m);
        } else if (img[0]) {
            lib_logf("[reu] %s", m);
        }
    }
    return OPE_OK;
}

// ---- the REU: a file into its memory (Gideon's "Load into REU" and "REU Preload"; the memory is the DDR2 window from offset 0) -----------------------------------------
static uint32_t reu_offset(void)
{
    int i = setting_ix("REU Preload Offset", 0);
    return i == 0 ? 0 : (128u * 1024u) << (i - 1);
}

static int reu_load(const char *path, char *msg, int msglen, bool at_zero)
{
    uint64_t sz = 0;
    bool dir = false;
    if (!g_api->fs->stat(path, &sz, &dir) || dir) {
        snprintf(msg, msglen, "No file %s", path);
        return 1;
    }
    if (cart_active()) {
        snprintf(msg, msglen, "A cartridge uses the REU's memory: remove it first");
        return 1;
    }
    uint32_t off = at_zero ? 0 : reu_offset(), room = 0x1000000u - off;     // Load into REU: REU_MEMORY_BASE (FW filetype_reu.cc:125); only the start's preload takes "REU Preload Offset"
    uint32_t cap;
    uint8_t *buf = xfer_alloc(&cap);
    if (!buf) {
        snprintf(msg, msglen, "No memory to load %s", path);
        return 1;
    }
    SvcFile *f = g_api->fs->open(path, false);
    if (!f) {
        xfer_free(buf);
        snprintf(msg, msglen, "Cannot open %s", path);
        return 1;
    }
    uint32_t n = sz > room ? room : (uint32_t)sz;
    bool ok = true;
    mp3_forget();
    for (uint32_t done = 0; ok && done < n;) {
        uint32_t k = n - done < cap ? n - done : cap;
        ok = g_api->fs->read(f, done, buf, k);
        ddr_write(off + done, buf, k);
        done += k;
    }
    g_api->fs->close(f);
    xfer_free(buf);
    if (!ok) {
        snprintf(msg, msglen, "Cannot read %s (the medium is gone?)", path);
        return 1;
    }
    snprintf(msg, msglen, "%u KB loaded into the REU at %u KB%s", (unsigned)(n >> 10), (unsigned)(off >> 10), sz > room ? " (cut: the file is larger than the room)" : "");
    return 0;
}

int op_reu_load(CoreCtx *, const OpArgs &a, OpOut &o) { return reu_load(a.s(0), o.msg, o.msglen) ? OPE_FAILED : OPE_OK; }

int op_reu_preload(CoreCtx *, const OpArgs &a, OpOut &o)
{
    OpIn in[2] = { in_str("key", "REU Preload Image"), in_str("value", a.s(0)) };
    OpResult r;
    g_api->ops->call("settings.set", in, 2, &r);
    OpIn on[2] = { in_str("key", "REU Preload"), in_str("value", "Enabled") };
    g_api->ops->call("settings.set", on, 2, &r);
    snprintf(o.msg, o.msglen, "%s goes into the REU at every start", a.s(0));
    return OPE_OK;
}

// F6 "A .cfg/.usr beside a PRG, D64, G64 or CRT is loaded before it runs" (TRX-Ultimate configio.cc:460-474, filetype_prg.cc:186-190, filetype_d64.cc:150,
// filetype_g64.cc:95, filetype_crt.cc:75): the file of the same name with the extension .cfg is read like a settings file (quietly); a PRG that has none tries .usr.
void trx64u_load_assoc_cfg(CoreCtx *, const char *path, bool usr_too)
{
    static const char *const exts[2] = { ".cfg", ".usr" };
    for (int k = 0; k < (usr_too ? 2 : 1); k++) {
        char p[FS_PATH_MAX];
        snprintf(p, sizeof(p), "%s", path);
        char *slash = strrchr(p, '/');
        char *dot = strrchr(p, '.');
        if (dot && (!slash || dot > slash)) {
            *dot = 0;
        }
        size_t l = strlen(p);
        if (l + 5 > sizeof(p)) {
            return;
        }
        strcpy(p + l, exts[k]);
        uint64_t sz = 0;
        bool dir = false;
        if (g_api->fs->stat(p, &sz, &dir) && !dir) {
            OpIn in[1] = { in_str("path", p) };
            OpResult r;
            g_api->ops->call("settings.load_file", in, 1, &r);
            lib_logf("[cfg] %s: %s", p, r.msg);
            return;
        }
    }
}

// "Run Disk": the .cfg of the disk first, then mount and run (drive A, the one drive of this core)
int op_run_disk(CoreCtx *c, const OpArgs &a, OpOut &o)
{
    trx64u_load_assoc_cfg(c, a.s(1), false);
    OpIn in[2] = { in_str("slot", a.s(0)), in_str("path", a.s(1)) };
    OpResult r;
    int rc = g_api->ops->call("drive.mount_run", in, 2, &r);
    snprintf(o.msg, o.msglen, "%s", r.msg);
    return rc ? OPE_FAILED : OPE_OK;
}

// ---- the ROM files of the machine ----------------------------------------------------------------------------------------------------------------
static int load_rom(const char *key, const char *put_op, uint32_t size, char *msg, int msglen)
{
    const char *name = setting_text(key);
    if (!name[0]) {
        return 0;
    }
    char path[200];
    if (name[0] == '/') {
        snprintf(path, sizeof(path), "%s", name);
    } else {
        snprintf(path, sizeof(path), "/flash/roms/%s", name);
    }
    uint64_t fsz = 0;
    bool dir = false;
    if (!g_api->fs->stat(path, &fsz, &dir) || dir || fsz != size) {
        snprintf(msg, msglen, "%s: %s needs a file of %u bytes", key, path, (unsigned)size);
        return 1;
    }
    uint8_t *buf = (uint8_t *)g_api->mem->malloc(size);                 // while the ROM goes in (the put operation copies it)
    if (!buf) {
        snprintf(msg, msglen, "%s: no memory to read %s", key, path);
        return 1;
    }
    int got = g_api->fs->load(path, buf, size);
    if (got != (int)size) {
        g_api->mem->free(buf);
        snprintf(msg, msglen, "%s: cannot read %s", key, path);
        return 1;
    }
    OpIn in[1] = { in_blob("data", buf, size) };
    OpResult r;
    int rc = g_api->ops->call(put_op, in, 1, &r);
    g_api->mem->free(buf);
    if (rc) {
        snprintf(msg, msglen, "%s", r.msg);
        return 1;
    }
    return 0;
}

// T228 R21, FW C64::list_kernals / list_basics / list_chars (c64.cc:2353-2440) and C1541::list_roms (c1541.cc): the choices of a ROM row are the
// files in /flash/roms of that ROM's size (Kernal and Basic 8192 bytes, Char 4096, a 1541 ROM 16384 or 32768), after the built-in one; the list ends
// with "Enter manually" (menuctl). The rows of the pick list (picklist.h): value TAB label. The app ABI's entry keeps 55 characters of a name: a name of
// that length may have been cut, and a cut name is no file to offer.
static const char *rom_choices(char *t, int tlen, const char *builtin, uint64_t size, uint64_t size2)
{
    static SvcFsEntry ent[24];
    int p = snprintf(t, tlen, "%s\tBuilt-in\n", builtin);
    int n = g_api->fs->list("/flash/roms", ent, 24);
    for (int i = 0; i < n; i++) {
        bool cut = strlen(ent[i].name) >= sizeof(ent[i].name) - 1;
        if (ent[i].dir || cut || (ent[i].size != size && ent[i].size != size2) || !strcasecmp(ent[i].name, builtin) ||
            p + (int)strlen(ent[i].name) + 2 >= tlen) {
            continue;
        }
        p += snprintf(t + p, tlen - p, "%s\n", ent[i].name);
    }
    return t;
}
const char *state_roms_kernal(CoreCtx *) { static char t[24 * 60]; return rom_choices(t, sizeof(t), "", DRV_c64_roms_kernal_COUNT, DRV_c64_roms_kernal_COUNT); }
const char *state_roms_basic(CoreCtx *)  { static char t[24 * 60]; return rom_choices(t, sizeof(t), "", DRV_c64_roms_basic_COUNT, DRV_c64_roms_basic_COUNT); }
const char *state_roms_char(CoreCtx *)   { static char t[24 * 60]; return rom_choices(t, sizeof(t), "", DRV_c64_roms_char_COUNT, DRV_c64_roms_char_COUNT); }
const char *state_roms_1541(CoreCtx *)   { static char t[24 * 60]; return rom_choices(t, sizeof(t), "1541.rom", 16384, 32768); }   // "1541.rom" is the built-in one (drive.fx_misc)

// The built-in image of a ROM role back in through the role's put operation (the base's "machine.put_builtin_rom"). 0, or the reason in msg.
static int put_builtin_rom(const char *put_op, char *msg, int msglen)
{
    OpIn in[1] = { in_str("put_op", put_op) };
    return op_call("machine.put_builtin_rom", in, 1, msg, msglen) != OPE_OK;
}

// The machine held, the reset-time settings put, released (the base's "machine.cold_reset"). 0, or the reason in msg.
static int cold_reset(char *msg, int msglen)
{
    return op_call("machine.cold_reset", 0, 0, msg, msglen) != OPE_OK;
}

// the chosen file goes in; an empty choice puts the built-in file back (not at the start: the boot has put it)
static bool roms_after_reload;
int fx_roms(CoreCtx *, const OpArgs &, OpOut &o)
{
    static const struct { const char *key, *put_op; } roles[3] = { { "Kernal ROM", "c64.put_kernal" }, { "Basic ROM", "c64.put_basic" }, { "Char ROM", "c64.put_char" } };
    for (int i = 0; i < 3 && (ctx_state() & CTX_STARTED) && !roms_after_reload; i++) {
        if (!setting_text(roles[i].key)[0] && put_builtin_rom(roles[i].put_op, o.msg, o.msglen)) {
            return OPE_FAILED;
        }
    }
    if (load_rom("Kernal ROM", "c64.put_kernal", DRV_c64_roms_kernal_COUNT, o.msg, o.msglen) ||
        load_rom("Basic ROM", "c64.put_basic", DRV_c64_roms_basic_COUNT, o.msg, o.msglen) ||
        load_rom("Char ROM", "c64.put_char", DRV_c64_roms_char_COUNT, o.msg, o.msglen)) {
        return OPE_FAILED;
    }
    return OPE_OK;
}

// ---- the power of the board: the ESP32 control module keeps it (u64cfg.cc has the off / cycle operations) ---------------------------------------------
#define ESP_CMD_SET_POWER_MODE   0x16
#define ESP_CMD_GET_POWER_MODE   0x17
#define ESP_CMD_SET_WAKE_ON_WIFI 0x18
#define ESP_CMD_GET_WAKE_ON_WIFI 0x19

// reply layout: header (4) + esp_err (4) + the field(s)
static bool esp_get(const SvcBoard *b, uint8_t cmd, int field, uint8_t *v)
{
    uint8_t r[16];
    int n = b->esp32_rpc(cmd, 0, 0, r, sizeof(r), 300);
    if (n < 9 + field || r[4] || r[5] || r[6] || r[7]) {
        return false;
    }
    *v = r[8 + field];
    return true;
}

int fx_power(CoreCtx *, const OpArgs &, OpOut &o)
{
    const SvcBoard *b = g_api->board;
    uint8_t want, have;
    want = (uint8_t)setting_ix("Power On After Power Loss", 0);
    if (!b || !esp_get(b, ESP_CMD_GET_POWER_MODE, 0, &have)) {
        snprintf(o.msg, o.msglen, "The control module does not answer: power-on behaviour not stored");
        return OPE_IO;
    }
    if (have != want) {
        b->esp32_rpc(ESP_CMD_SET_POWER_MODE, &want, 1, 0, 0, 0);
    }
    want = (uint8_t)setting_ix("Wake On Wi-Fi", 0);
    if (esp_get(b, ESP_CMD_GET_WAKE_ON_WIFI, 0, &have) && have != want) {
        b->esp32_rpc(ESP_CMD_SET_WAKE_ON_WIFI, &want, 1, 0, 0, 0);
    }
    return OPE_OK;
}

// ---- drive A: the settings of Gideon's c1541 store that the library has a place for, and the actions the menu adds -------------------------------------
#define DRIVE_ROM_BYTES 0x8000u                                      // d1541_drive.h: the largest drive ROM (a 1541 ROM is 16 or 32 KB)

int fx_drive_misc(CoreCtx *, const OpArgs &, OpOut &o)
{
    const Cbm1541Api *d = X_GET(X_DRIVE, Cbm1541Api, set_options);
    Cbm1541Options opt;
    opt.size = sizeof(opt);
    opt.track_twist = setting_ix("Track Twist", 27);
    opt.align_tracks = setting_ix("GCR Save Align Tracks", 1) != 0;
    opt.reset_with_c64 = setting_ix("Resets when C64 resets", 1) != 0;
    opt.freeze_in_menu = setting_ix("Freezes in menu", 1) != 0;         // c1541.cc:343-344: b2 of the reset register, written at the next drive reset
    opt.geos_protection = (uint8_t)setting_ix("D64 Geos Copy Protection", 0);   // c1541.cc:599: the D64 -> GCR conversion of the next mount
    if (!d || !d->set_options(&opt)) {
        return OPE_OK;                                              // no drive in this machine: nothing to set
    }
    const char *rom = setting_text("ROM for 1541 mode");
    static char last_rom[64];                                       // the choice the drive has now; a change is applied at once, the other settings leave the ROM alone
    bool changed = strncmp(last_rom, rom, sizeof(last_rom) - 1) != 0;
    snprintf(last_rom, sizeof(last_rom), "%s", rom);
    if (changed && (ctx_state() & CTX_STARTED) && (!rom[0] || !strcasecmp(rom, "1541.rom"))) {      // back to the standard ROM
        if (put_builtin_rom("drive.put_rom", o.msg, o.msglen)) {
            return OPE_FAILED;
        }
    } else if (rom[0] && strcasecmp(rom, "1541.rom") && (changed || roms_after_reload)) {      // another file than the standard one
        char path[200];
        snprintf(path, sizeof(path), "%s%s", rom[0] == '/' ? "" : "/flash/roms/", rom);
        OpIn in[2] = { in_str("slot", "iec:drive_a"), in_str("path", path) };
        OpResult r;
        if (g_api->ops->call("drive.set_rom", in, 2, &r)) {
            snprintf(o.msg, o.msglen, "%s", r.msg);
            return OPE_FAILED;
        }
    }
    return OPE_OK;
}

// F3 on a .bin / .64c of 8 KB: "Load Kernal" - the file goes into the KERNAL window for this session only, nothing is stored, and the machine resets
// (TRX-Ultimate filetype_bin.cc:95, 242-257: C64_SET_KERNAL, then MENU_C64_RESET). "Reload ROMs, reset" brings the chosen / built-in KERNAL back.
int op_load_kernal(CoreCtx *, const OpArgs &a, OpOut &o)
{
    uint8_t *buf = (uint8_t *)g_api->mem->malloc(DRV_c64_roms_kernal_COUNT);
    if (!buf) {
        snprintf(o.msg, o.msglen, "No memory to read %s", a.s(0));
        return OPE_FAILED;
    }
    int n = g_api->fs->load(a.s(0), buf, DRV_c64_roms_kernal_COUNT);
    if (n != (int)DRV_c64_roms_kernal_COUNT) {
        g_api->mem->free(buf);
        snprintf(o.msg, o.msglen, "%s: a KERNAL is %u bytes", a.s(0), (unsigned)DRV_c64_roms_kernal_COUNT);
        return OPE_FAILED;
    }
    OpIn in[1] = { in_blob("data", buf, DRV_c64_roms_kernal_COUNT) };
    OpResult r;
    int rc = g_api->ops->call("c64.put_kernal", in, 1, &r);
    g_api->mem->free(buf);
    if (rc) {
        snprintf(o.msg, o.msglen, "%s", r.msg);
        return OPE_FAILED;
    }
    if (cold_reset(o.msg, o.msglen)) {
        return OPE_FAILED;
    }
    snprintf(o.msg, o.msglen, "KERNAL from %s for this session, machine restarted", a.s(0));
    return OPE_OK;
}

// "Reload ROMs": after the built-in files went in, the files chosen in F5 > C64 and the drive ROM go over them again
int op_after_roms(CoreCtx *c, const OpArgs &, OpOut &o)
{
    roms_after_reload = true;
    OpArgs none;
    memset(&none, 0, sizeof(none));
    int rc = fx_roms(c, none, o);
    if (rc == OPE_OK) {
        rc = fx_drive_misc(c, none, o);
    }
    roms_after_reload = false;
    return rc;
}

// F3 on a .rom: "Set as 1541 ROM" - the file is copied to /flash/roms and its name stored in "ROM for 1541 mode" (TRX-Ultimate filetype_bin.cc:208-215, c1541.cc:1208-1214).
// /flash is writable for the app on the monolith profile (fsif.h fs_flash_rw, T228 R21); where it is not (the MultiCore host) the file's own path is stored and the message says so.
int op_set_rom_store(CoreCtx *, const OpArgs &a, OpOut &o)
{
    const char *src = a.s(0);
    const char *base = strrchr(src, '/');
    base = base ? base + 1 : src;
    char dst[200];
    snprintf(dst, sizeof(dst), "/flash/roms/%s", base);
    const char *store = base;
    const char *note = "";
    if (strcasecmp(src, dst)) {                                       // the file lies in /flash/roms already: no copy onto itself
        uint8_t *buf = (uint8_t *)g_api->mem->malloc(DRIVE_ROM_BYTES);   // 32 KB while the copy runs
        if (!buf) {
            snprintf(o.msg, o.msglen, "No memory to copy %s", src);
            return OPE_FAILED;
        }
        int n = g_api->fs->load(src, buf, DRIVE_ROM_BYTES);
        if (n != 16384 && n != 32768) {
            g_api->mem->free(buf);
            snprintf(o.msg, o.msglen, "%s: a drive ROM is 16 or 32 KB", src);
            return OPE_FAILED;
        }
        g_api->fs->mkdir("/flash/roms");
        if (!g_api->fs->save(dst, buf, (uint32_t)n)) {
            store = src;
            note = " (not copied to /flash/roms)";
        }
        g_api->mem->free(buf);
    }
    OpIn in[2] = { in_str("key", "ROM for 1541 mode"), in_str("value", store) };
    OpResult r;
    if (g_api->ops->call("settings.set", in, 2, &r)) {
        snprintf(o.msg, o.msglen, "%s", r.msg);
        return OPE_FAILED;
    }
    OpIn d[2] = { in_str("slot", "iec:drive_a"), in_str("path", store == base ? dst : src) };
    if (g_api->ops->call("drive.set_rom", d, 2, &r)) {
        snprintf(o.msg, o.msglen, "%s", r.msg);
        return OPE_FAILED;
    }
    snprintf(o.msg, o.msglen, "%s is the ROM for 1541 mode%s", base, note);
    return OPE_OK;
}

// "Insert Blank": a blank D64 made in a writable folder, mounted unlinked (nothing of it is written back), the file removed again
int op_insert_blank(CoreCtx *, const OpArgs &, OpOut &o)
{
    const Cbm1541Api *d = X_GET(X_DRIVE, Cbm1541Api, create);
    if (!d) {
        snprintf(o.msg, o.msglen, "This machine has no drive A");
        return OPE_FAILED;
    }
    char dir[200];
    dir[0] = 0;
    SvcFsEntry r[6];
    int k = g_api->fs->list("/", r, 6);
    for (int i = 0; i < k && !dir[0]; i++) {
        if (strcasecmp(r[i].name, "flash")) {
            snprintf(dir, sizeof(dir), "/%s", r[i].name);
        }
    }
    if (!dir[0]) {
        snprintf(o.msg, o.msglen, "A blank disk needs a medium to build it on (stick or card)");
        return OPE_FAILED;
    }
    char path[220];
    snprintf(path, sizeof(path), "%s/.blank.d64", dir);
    if (d->create(path, CBM1541_FORMAT_D64, "BLANK", o.msg, o.msglen)) {
        return OPE_FAILED;
    }
    int rc = drive_mount(path, CBM1541_MODE_UNLINKED, o.msg, o.msglen);
    g_api->fs->remove(path);
    if (rc) {
        return OPE_FAILED;
    }
    snprintf(o.msg, o.msglen, "A blank disk is in drive A (unlinked: nothing is written to a file, use Save Disk)");
    return OPE_OK;
}

// "Swap Disk" and CTRL + D (TRX-Ultimate c1541.cc:653-727, userinterface.cc:444-445): the disk's file name has a denominating character - the last digit that
// no digit follows, or the last letter between two non-letters - and the next file that has the next character there (digits 0-9, letters A-Z, round and
// round, names compared in upper case) takes the place of this one after the swap delay, in the mode of this one. The walk is the firmware's own: its
// wrap-around lands on the second character (a `continue` before the loop's c++), so after 9 comes 1 and after Z comes B, as there. The folder is listed
// through api->fs, whose entries keep 55 characters of a name: a series whose names are longer finds no next disk. `cur` and `dir` are copies of what
// drive_info gave (the eject ends the drive library's strings).
static int swap_next(const char *cur, const char *dir, int mode, int delay_ms, SvcFsEntry *ents, int cap, OpOut &o)
{
    char work[FS_NAME_MAX + 1];
    int len = (int)strlen(cur);
    if (len < 5 || len > FS_NAME_MAX) {
        snprintf(o.msg, o.msglen, "No other disk in the series of %s", cur);
        return OPE_FAILED;
    }
    for (int i = 0; i <= len; i++) {
        work[i] = (char)lc_toupper((unsigned char)cur[i]);
    }
    int index = -1;
    for (int k = len - 5; k >= 0; k--) {
        bool found = lc_isdigit((unsigned char)work[k]) && !lc_isdigit((unsigned char)work[k + 1]);
        if (k > 0) {
            found |= lc_isalpha((unsigned char)work[k]) && !lc_isalpha((unsigned char)work[k - 1]) && !lc_isalpha((unsigned char)work[k + 1]);
        } else {
            found |= lc_isalpha((unsigned char)work[k]) && !lc_isalpha((unsigned char)work[k + 1]);
        }
        if (found) {
            index = k;
            break;
        }
    }
    if (index < 0) {
        snprintf(o.msg, o.msglen, "No denominating character found in %s", cur);
        return OPE_FAILED;
    }
    int k = g_api->fs->list(dir, ents, cap);
    char current = work[index];
    char top = lc_isalpha((unsigned char)current) ? 'A' : '0';
    char bottom = lc_isalpha((unsigned char)current) ? 'Z' : '9';
    int steps = 0;                                              // the firmware's loop never ends with no match when the character is the first of its range (A or 0); here it stops after a round
    for (char ch = (char)(current + 1); ch != current && steps < 40; ch++, steps++) {
        if (ch > bottom) {
            ch = top;
            continue;
        }
        work[index] = ch;
        for (int i = 0; i < k; i++) {
            if (ents[i].dir || strcasecmp(ents[i].name, work)) {
                continue;
            }
            char next[FS_PATH_MAX];
            snprintf(next, sizeof(next), "%s/%s", dir, ents[i].name);
            char name[sizeof(ents[i].name)];
            snprintf(name, sizeof(name), "%s", ents[i].name);
            const Cbm1541Api *d = X_GET(X_DRIVE, Cbm1541Api, slot_eject);
            if (!d) {
                snprintf(o.msg, o.msglen, "This machine has no drive A");
                return OPE_FAILED;
            }
            int rc = d->slot_eject(o.msg, o.msglen);                // c1541.cc:712: what the head wrote goes back to the file first
            if (rc) {
                return OPE_FAILED;
            }
            wait_ms((uint32_t)(delay_ms > 0 ? delay_ms : 0));
            if (drive_mount(next, mode, o.msg, o.msglen)) {
                return OPE_FAILED;
            }
            snprintf(o.msg, o.msglen, "Swapped to %s", name);
            return OPE_OK;
        }
    }
    snprintf(o.msg, o.msglen, "No matching image found for %s", cur);
    return OPE_FAILED;
}

// The disk in drive A: its full path, its file name and folder copied, its mode and the swap delay. False: no drive or no disk.
struct DriveDisk {
    char path[FS_PATH_MAX], name[FS_NAME_MAX + 1], dir[FS_PATH_MAX];
    int mode, delay_ms;
};
static bool drive_disk(DriveDisk *k)
{
    Cbm1541Info info;
    if (!drive_info(&info) || !info.image_path || !info.image_name || !info.last_dir || !info.last_dir[0]) {
        return false;
    }
    snprintf(k->path, sizeof(k->path), "%s", info.image_path);
    snprintf(k->name, sizeof(k->name), "%s", info.image_name);
    snprintf(k->dir, sizeof(k->dir), "%s", info.last_dir);
    k->mode = info.mode;
    k->delay_ms = info.swap_delay_ms;
    return true;
}

int op_swap(CoreCtx *, const OpArgs &, OpOut &o)
{
    DriveDisk *k = (DriveDisk *)g_api->mem->malloc(sizeof(DriveDisk));                // 1.3 KB off the stack while the swap runs
    if (!k || !drive_disk(k)) {
        g_api->mem->free(k);
        snprintf(o.msg, o.msglen, k ? "There is no disk in the drive to swap" : "No memory to swap");
        return OPE_FAILED;
    }
    SvcFsEntry *ents = (SvcFsEntry *)g_api->mem->malloc(256 * sizeof(SvcFsEntry));     // 18 KB while the swap runs, not for good
    if (!ents) {
        g_api->mem->free(k);
        snprintf(o.msg, o.msglen, "No memory to list the folder");
        return OPE_FAILED;
    }
    int rc = swap_next(k->name, k->dir, k->mode, k->delay_ms, ents, 256, o);
    g_api->mem->free(ents);
    g_api->mem->free(k);
    return rc;
}

// "Unlink": the disk stays in the drive, the file is let go (what the machine writes is not saved any more)
int op_unlink(CoreCtx *, const OpArgs &, OpOut &o)
{
    DriveDisk *k = (DriveDisk *)g_api->mem->malloc(sizeof(DriveDisk));
    if (!k || !drive_disk(k)) {
        g_api->mem->free(k);
        snprintf(o.msg, o.msglen, k ? "There is no disk in the drive" : "No memory to unlink");
        return OPE_FAILED;
    }
    int rc = OPE_OK;
    if (k->mode == CBM1541_MODE_UNLINKED) {
        snprintf(o.msg, o.msglen, "The disk is unlinked already");
    } else if (drive_mount(k->path, CBM1541_MODE_UNLINKED, o.msg, o.msglen)) {
        rc = OPE_FAILED;
    } else {
        snprintf(o.msg, o.msglen, "Unlinked: %s is no longer written", k->name);
    }
    g_api->mem->free(k);
    return rc;
}

// ---- the machine: pause, clear memory, save memory ------------------------------------------------------------------------------------------------
// Pause keeps C64_STOP asserted after the menu closes (run.cc close_menu, Freeze::leave(keep_stopped)); Resume takes the flag away and the close releases it.
int op_pause_toggle(CoreCtx *, const OpArgs &, OpOut &o)
{
    const SvcCtx *c = g_api->ctx;
    uint32_t st = ctx_state();
    if (!c || !(st & CTX_STARTED)) {
        snprintf(o.msg, o.msglen, "The machine is not running yet");
        return OPE_FAILED;
    }
    bool pause = !(st & CTX_PAUSED);
    c->set_paused(pause);
    snprintf(o.msg, o.msglen, pause ? "Paused: the machine stands still, F7 or the menu key brings the menu back" : "Resumed");
    return OPE_OK;
}

const char *state_pause_label(CoreCtx *) { return (ctx_state() & CTX_PAUSED) ? "Resume" : "Pause"; }
const char *state_machine(CoreCtx *)
{
    uint32_t st = ctx_state();
    return !(st & CTX_STARTED) ? "Waiting" : (st & CTX_PAUSED) ? "Paused" : (st & CTX_MENU_OPEN) ? "Frozen" : "Running";
}

// "Reboot (Clr Mem)": the RAM is zeroed through the second port while the machine is held, then the cold reset (Gideon MENU_C64_CLEARMEM: clear_ram, start_cartridge)
int op_clear_reboot(CoreCtx *, const OpArgs &, OpOut &o)
{
    static const uint8_t zero[512] = { 0 };
    if (!c64_window()) {
        snprintf(o.msg, o.msglen, "The C64's memory window is not there");
        return OPE_FAILED;
    }
    c64_mode_reset(true);
    c64_ram_memonly(true);
    for (uint32_t a = 0; a < 0x10000; a += sizeof(zero)) {
        ram_write(a, zero, sizeof(zero));
    }
    c64_ram_memonly(false);
    if (cold_reset(o.msg, o.msglen)) {
        return OPE_FAILED;
    }
    snprintf(o.msg, o.msglen, "Memory cleared, machine restarted");
    return OPE_OK;
}

// where a file of the menu goes (ctx.target_path): the folder the menu was on, else the first medium of the root but /flash; `ext` is added when the name has none
static bool target_path(const char *name, const char *ext, char *out, int n)
{
    const SvcCtx *c = g_api->ctx;
    return c && c->target_path(name, ext, out, n);
}

static int write_ddr(const char *path, uint32_t off, uint32_t len, bool ram, char *msg, int msglen)
{
    if (ram && !c64_window()) {
        snprintf(msg, msglen, "The C64's memory window is not there");
        return 1;
    }
    uint32_t cap;
    uint8_t *buf = xfer_alloc(&cap);
    if (!buf) {
        snprintf(msg, msglen, "No memory to save %s", path);
        return 1;
    }
    if (!g_api->fs->save(path, buf, 0)) {
        xfer_free(buf);
        snprintf(msg, msglen, "cannot write %s", path);
        return 1;
    }
    SvcFile *f = g_api->fs->open(path, true);
    if (!f || !g_api->fs->writable(f)) {
        if (f) {
            g_api->fs->close(f);
        }
        xfer_free(buf);
        snprintf(msg, msglen, "%s: the medium cannot be written", path);
        return 1;
    }
    bool ok = true;
    for (uint32_t done = 0; ok && done < len;) {
        uint32_t n = len - done < cap ? len - done : cap;
        if (ram) {
            c64_ram_memonly(true);
            ram_read(off + done, buf, n);
            c64_ram_memonly(false);
        } else {
            ddr_read(off + done, buf, n);
        }
        ok = g_api->fs->write(f, done, buf, n);
        done += n;
    }
    ok = ok && g_api->fs->sync(f);
    g_api->fs->close(f);
    xfer_free(buf);
    if (!ok) {
        g_api->fs->remove(path);
        snprintf(msg, msglen, "%s: writing failed (medium full or gone)", path);
        return 1;
    }
    snprintf(msg, msglen, "Saved %s (%u KB)", path, (unsigned)((len + 1023) >> 10));
    return 0;
}

int op_save_ram(CoreCtx *, const OpArgs &a, OpOut &o)
{
    char p[260];
    if (!target_path(a.s(0), ".bin", p, sizeof(p))) {
        snprintf(o.msg, o.msglen, "No medium to save to");
        return OPE_FAILED;
    }
    return write_ddr(p, 0, 0x10000, true, o.msg, o.msglen) ? OPE_FAILED : OPE_OK;
}

int op_save_reu(CoreCtx *, const OpArgs &a, OpOut &o)
{
    char p[260];
    if (setting_ix("RAM Expansion Unit", 0) != 1) {
        snprintf(o.msg, o.msglen, "The REU is not enabled (F5 > 4 I/O)");
        return OPE_FAILED;
    }
    if (!target_path(a.s(0), ".reu", p, sizeof(p))) {
        snprintf(o.msg, o.msglen, "No medium to save to");
        return OPE_FAILED;
    }
    return write_ddr(p, 0, 128u * 1024u << setting_ix("REU Size", 4), false, o.msg, o.msglen) ? OPE_FAILED : OPE_OK;
}

// ---- F7 #4 the GEOS RAM drives in the REU (TRX-Ultimate c64.cc:2354-2391, c64_subsys.cc:852-912, filetype_d64.cc:121-124, 161-240) -------------------------------------
// MegaPatch 3 keeps up to four RAM drives in the REU: the type byte of drive n is at $BB0E + n (GEOS 64: at $798E + n, one table further, $81 .. $84), its first
// bank at $7DC7 + n; a native one (type $84 / $A4) is as large as its track count says (the drive's table at the address kept at $BAFE + n / $BB02 + n). The REU's
// memory is the DDR2 window from offset 0; with a cartridge in the port it holds the cartridge's image and there is no drive.
#define MP3_DNP 31
static uint8_t reu_byte(uint32_t at)
{
    uint8_t b = 0;
    ddr_read(at, &b, 1);
    return b;
}

// The type of the four drives (0 none, 1541, 1581, MP3_DNP) from at most three reads of four bytes: the type bytes at $BB0E (which are also the GEOS 64
// mark), GEOS 64's at $798E, the first banks at $7DC7.
static void mp3_types_now(int t[4])
{
    uint8_t real[4], bank[4];
    if (cart_active()) {
        t[0] = t[1] = t[2] = t[3] = 0;
        return;
    }
    ddr_read(0xBB0E, real, 4);
    bool geos64 = real[0] == 0x03 && real[1] == 0xA9 && real[2] == 0x06 && real[3] == 0x8D;
    if (geos64) {
        ddr_read(0x798E, real, 4);
    }
    ddr_read(0x7DC7, bank, 4);
    for (int n = 0; n < 4; n++) {
        uint8_t r = geos64 && real[n] > 0x83 ? 0 : real[n];
        int k = 0;
        k = r == 0x81 ? 1541 : k;
        // 0x82, a 1571 RAM drive, counts as none: .d71 is out of trx64u (Alex 2026-11-18: ".d71 goes out completely")
        k = r == 0x83 ? 1581 : k;
        k = r == 0x84 || r == 0xA4 ? MP3_DNP : k;
        t[n] = bank[n] > 0x40 ? 0 : k;
    }
}

// for an operation: the drive as the REU holds it now
static int mp3_type(int n)
{
    int t[4];
    mp3_types_now(t);
    return t[n & 3];
}

// For the menu's states: they all run in one pass (at the open, after an operation, every 2 s), and the eight of the RAM drives (and the show_state
// conditions over them) share one look at the REU per pass instead of three reads each. The REU changes outside this library (the C64, a state load), so the
// look is not kept past the pass (MP3_LOOK_MS); an operation of this library that writes the REU drops it (mp3_forget).
#define MP3_LOOK_MS 100
static struct {
    bool valid;
    uint32_t at;
    int t[4];
} g_mp3;

static void mp3_forget(void)
{
    g_mp3.valid = false;
}

static int mp3_seen(int n)
{
    uint32_t now = now_ms();
    if (!g_mp3.valid || now - g_mp3.at > MP3_LOOK_MS) {
        mp3_types_now(g_mp3.t);
        g_mp3.at = now;
        g_mp3.valid = true;
    }
    return g_mp3.t[n & 3];
}

static uint32_t mp3_size(int n, int type)
{
    if (type == 1541) {
        return 174848;
    }
    if (type == 1581) {
        return 819200;
    }
    uint32_t base = reu_byte(0xBAFE + (uint32_t)n) | ((uint32_t)reu_byte(0xBAFE + 4 + (uint32_t)n) << 8);
    return (uint32_t)reu_byte(base + 0x84) << 16;
}

// The command interface's view of a RAM drive (CONTROL 0x40, DOS 0x41 / 0x42): its type, its size (a native one by its track count) and where its first byte lies in the REU.
static void uci_ramdrive(int n, int *type, uint32_t *size, uint32_t *base)
{
    *type = mp3_type(n);
    *size = *type ? mp3_size(n, *type) : 0;
    *base = (uint32_t)reu_byte(0x7DC7 + (uint32_t)n) << 16;
}

static const char *mp3_name(int n)
{
    switch (mp3_seen(n)) {
    case 1541: return "1541";
    case 1581: return "1581";
    case MP3_DNP: return "DNP";
    default: return 0;
    }
}
const char *state_mp3_a(CoreCtx *) { return mp3_name(0); }
const char *state_mp3_b(CoreCtx *) { return mp3_name(1); }
const char *state_mp3_c(CoreCtx *) { return mp3_name(2); }
const char *state_mp3_d(CoreCtx *) { return mp3_name(3); }
const char *state_nomp3_a(CoreCtx *) { return mp3_seen(0) ? 0 : "none"; }
const char *state_nomp3_b(CoreCtx *) { return mp3_seen(1) ? 0 : "none"; }
const char *state_nomp3_c(CoreCtx *) { return mp3_seen(2) ? 0 : "none"; }
const char *state_nomp3_d(CoreCtx *) { return mp3_seen(3) ? 0 : "none"; }

// "Save MP3 Drv A..D": the drive's bytes into a file named by the person
int op_save_mp3(CoreCtx *, const OpArgs &a, OpOut &o)
{
    int n = a.i(0), type = mp3_type(n);
    char p[260];
    if (!type) {
        snprintf(o.msg, o.msglen, "No GEOS RAM drive %c in the REU", 'A' + n);
        return OPE_FAILED;
    }
    if (!target_path(a.s(1), type == 1541 ? ".d64" : type == 1581 ? ".d81" : ".dnp", p, sizeof(p))) {
        snprintf(o.msg, o.msglen, "No medium to save to");
        return OPE_FAILED;
    }
    uint32_t len = mp3_size(n, type), at = (uint32_t)reu_byte(0x7DC7 + (uint32_t)n) << 16;
    return write_ddr(p, at, len, false, o.msg, o.msglen) ? OPE_FAILED : OPE_OK;
}

// "Load into MP3 Drv A..D" on a .d64 / .d81 / .dnp: the drive must be of the file's type (a native one: of the file's size, at most 12 MB)
int op_load_mp3(CoreCtx *, const OpArgs &a, OpOut &o)
{
    const char *path = a.s(0);
    int n = a.i(1), type = mp3_type(n);
    const char *dot = strrchr(path, '.');
    int want = !dot ? 0 : !strcasecmp(dot, ".d64") ? 1541 : !strcasecmp(dot, ".d81") ? 1581 : !strcasecmp(dot, ".dnp") ? MP3_DNP : 0;
    if (!type || type != want) {
        snprintf(o.msg, o.msglen, "Invalid operation: RAM drive %c is not of this type", 'A' + n);
        return OPE_FAILED;
    }
    uint32_t len = mp3_size(n, type), at = (uint32_t)reu_byte(0x7DC7 + (uint32_t)n) << 16;
    uint64_t sz = 0;
    bool dir = false;
    if (!g_api->fs->stat(path, &sz, &dir) || dir) {
        snprintf(o.msg, o.msglen, "No file %s", path);
        return OPE_FAILED;
    }
    if (type == MP3_DNP) {
        if (sz != len) {
            snprintf(o.msg, o.msglen, "Size does not match");
            return OPE_FAILED;
        }
        if (len > 12u * 1024u * 1024u) {
            snprintf(o.msg, o.msglen, "DNP file too large");
            return OPE_FAILED;
        }
    }
    uint32_t cap;
    uint8_t *buf = xfer_alloc(&cap);
    if (!buf) {
        snprintf(o.msg, o.msglen, "No memory to load %s", path);
        return OPE_FAILED;
    }
    SvcFile *f = g_api->fs->open(path, false);
    if (!f) {
        xfer_free(buf);
        snprintf(o.msg, o.msglen, "Cannot open %s", path);
        return OPE_FAILED;
    }
    uint32_t half = len, total = 0;
    bool ok = true;
    mp3_forget();
    {
        uint32_t from = 0, dst = at;
        for (uint32_t done = 0; ok && done < half && from + done < sz;) {
            uint32_t k = half - done < cap ? half - done : cap;
            if ((uint64_t)from + done + k > sz) {
                k = (uint32_t)(sz - from - done);
            }
            ok = g_api->fs->read(f, from + done, buf, k);
            ddr_write(dst + done, buf, k);
            done += k;
            total += k;
        }
    }
    g_api->fs->close(f);
    xfer_free(buf);
    if (!ok) {
        snprintf(o.msg, o.msglen, "Cannot read %s (the medium is gone?)", path);
        return OPE_FAILED;
    }
    snprintf(o.msg, o.msglen, "Bytes loaded: %u ($%08x)", (unsigned)total, (unsigned)total);
    return OPE_OK;
}

int op_save_cart(CoreCtx *, const OpArgs &a, OpOut &o)
{
    char p[260];
    if (!target_path(a.s(0), ".crt", p, sizeof(p))) {
        snprintf(o.msg, o.msglen, "No medium to save to");
        return OPE_FAILED;
    }
    const C64CartApi *k = X_GET(X_CART, C64CartApi, save);
    if (!k) {
        snprintf(o.msg, o.msglen, "No cartridge library");
        return OPE_FAILED;
    }
    return k->save(p, o.msg, o.msglen) ? OPE_FAILED : OPE_OK;
}

const char *state_suggest_ram(CoreCtx *) { return "c64ram"; }
const char *state_suggest_reu(CoreCtx *) { return "reu"; }
const char *state_suggest_cart(CoreCtx *) { return "cartridge"; }
const char *state_suggest_tw(CoreCtx *) { return "timewarp"; }
const char *state_suggest_ring(CoreCtx *) { return "ring"; }

// ---- states ----------------------------------------------------------------------------------------------------------------------------------------
// the rows of the REU (size, preload) are listed while the expansion is not Disabled
const char *state_reu_on(CoreCtx *) { return setting_ix("RAM Expansion Unit", 0) == 1 && !port_taken() ? "yes" : 0; }   // T228 R20 2: the REU is there only as the REU (not GeoRAM) and while no cartridge has the port
// the three rows of the port's devices (REU, Ultimate audio, UCI) are listed while the port is free; while it is taken a row of the same name says why they are off (c64.cc:424-428)
const char *state_port_taken(CoreCtx *) { return port_taken() ? "Disabled (cartridge)" : 0; }
const char *state_cartridge(CoreCtx *) { return cart_active() ? cart_name() : 0; }
const char *state_cartridge_path(CoreCtx *) { return cart_active() ? cart_path() : 0; }

// ---- the command interface's machine side (UciHooks of the export "uci"; every callback runs on the UCI's own task): the REU and drive A -----------------
static int uci_reu(uint32_t *size, uint32_t *offset, char *img, int imgn, char *busy, int busyn)
{
    if (setting_ix("RAM Expansion Unit", 0) != 1) {
        return -2;
    }
    if (cart_active()) {
        snprintf(busy, busyn, "A cartridge uses the REU's memory: remove it first");
        return -4;
    }
    *size = 0x20000u << setting_ix("REU Size", 4);
    *offset = reu_offset();
    snprintf(img, imgn, "%s", setting_text("REU Preload Image"));
    return 0;
}

static bool uci_drive_info(int idx, uint8_t *type, uint8_t *addr, bool *power)
{
    Cbm1541Info info;
    if (idx != 0 || !drive_info(&info)) {
        return false;
    }
    *type = (uint8_t)(info.type == CBM1541_TYPE_UNSET ? (int32_t)CBM1541_TYPE_1541 : info.type);
    *addr = info.unit;
    *power = info.powered != 0;
    return true;
}

static int uci_drive_do(int idx, int what, const char *path, char *msg, int msglen)
{
    static const char *const names[] = { "drive.mount", "drive.eject", "drive.swap", "drive.set_power", "drive.set_power" };
    OpIn in[3] = { in_str("slot", "iec:drive_a"), in_str("path", path ? path : ""), in_str("mode", "rw") };
    OpIn pw[1] = { in_int("value", what == 4 ? 1 : 0) };
    OpResult *r = (OpResult *)g_api->mem->malloc(sizeof(OpResult));         // 500 bytes off the UCI task's stack
    if (!r) {
        snprintf(msg, msglen, "No memory");
        return 1;
    }
    int rc = OPE_FAILED;
    r->msg[0] = 0;
    if (idx == 0) {
        rc = g_api->ops->call(names[what], what == 0 ? in : what == 1 ? in : what == 2 ? in + 3 : pw, what == 0 ? 3 : what == 1 ? 1 : what == 2 ? 0 : 1, r);
    }
    snprintf(msg, msglen, "%s", r->msg);
    g_api->mem->free(r);
    return rc == OPE_OK ? 0 : 1;
}

static bool uci_ef_clear(uint8_t bank, bool high)
{
    const C64CartApi *c = X_GET(X_CART, C64CartApi, ef_clear);
    return c && c->ef_clear(bank, high);
}

static const UciHooks uci_hooks = { sizeof(UciHooks), uci_reu, uci_drive_info, uci_drive_do, u64cfg_sid_map, "/temp/c64_memory.bin", uci_ef_clear,
                                    uci_ramdrive, u64cfg_palette };
const UciHooks *trx64u_uci_hooks(void) { return &uci_hooks; }

// ---- the Ultimate DMA service (dmasvc.cc, FW network/socket_dma.cc:129-232): what a command of port 64 does here, by the names the server calls (dma.*). The server stages
// a PRG, a disk image or a cartridge image as a file in /Temp (FW's save_temp_file) and hands the path; the rest is the operations the menu uses. ------------------------
// SOCKET_CMD_LOADSIDCRT (0xFF71) has no operation here: the SID player is gone (U20 D6), so the server's call of dma.sidcrt finds none and is logged and dropped.
static int dma_call(const char *op, const OpIn *in, int n, OpOut &o)
{
    OpResult r;
    int rc = g_api->ops->call(op, in, n, &r);
    snprintf(o.msg, o.msglen, "%s", r.msg);
    return rc ? OPE_FAILED : OPE_OK;
}

// SOCKET_CMD_DMA / DMARUN / DMAJUMP (FW :134-145: RUNCODE_DMALOAD, _RUN, _JUMP): 0 = into RAM at the load address, nothing started (c64.dma_prg); 1 = and run it (prg.run: the machine
// is reset and the program started at READY, the menu's Run); 2 = and jump to the load address (prg.load with the file's own first two bytes, FW c64_subsys.cc:1056-1060).
int op_dma_prg(CoreCtx *, const OpArgs &a, OpOut &o)
{
    OpIn p[2] = { in_str("path", a.s(0)), in_int("start", 0) };
    if (a.i(1) == 0) {
        return dma_call("c64.dma_prg", p, 1, o);
    }
    if (a.i(1) == 1) {
        return dma_call("prg.run", p, 1, o);
    }
    uint8_t b[2];
    SvcFile *f = g_api->fs->open(a.s(0), false);
    bool ok = f && g_api->fs->fsize(f) >= 2 && g_api->fs->read(f, 0, b, 2);
    if (f) {
        g_api->fs->close(f);
    }
    if (!ok) {
        snprintf(o.msg, o.msglen, "No load address in %s", a.s(0));
        return OPE_FAILED;
    }
    p[1] = in_int("start", b[0] | (b[1] << 8));
    return dma_call("prg.load", p, 2, o);
}
// SOCKET_CMD_MOUNT_IMG / RUN_IMG (FW :178-196): the image in drive A read-write, and for RUN_IMG the real LOAD"*",8,1 and RUN (RUNCODE_MOUNT_LOAD_RUN: drive.mount_run).
int op_dma_mount(CoreCtx *, const OpArgs &a, OpOut &o)
{
    int rc;
    if (a.i(1)) {
        OpIn in[2] = { in_str("slot", "iec:drive_a"), in_str("path", a.s(0)) };
        rc = dma_call("drive.mount_run", in, 2, o);
    } else {
        OpIn in[3] = { in_str("slot", "iec:drive_a"), in_str("path", a.s(0)), in_str("mode", "rw") };
        rc = dma_call("drive.mount", in, 3, o);
    }
    return rc;
}
// SOCKET_CMD_RUN_CRT (FW :197-210, FileTypeCRT::execute_st): the cartridge is placed and the machine reset into it, as the Library's Run does.
int op_dma_crt(CoreCtx *, const OpArgs &a, OpOut &o)
{
    OpIn in[1] = { in_str("path", a.s(0)) };
    return dma_call("c64.run_crt", in, 1, o);
}
// SOCKET_CMD_REUWRITE (FW :165-173): the bytes into the REU's memory at the 24 bit offset, wrapping at 16 MB; dropped while a cartridge image is in that memory (FW reu_memory_usable).
int op_dma_reu(CoreCtx *, const OpArgs &a, OpOut &o)
{
    if (cart_active()) {
        snprintf(o.msg, o.msglen, "REU write dropped: a cartridge uses the REU's memory");
        return OPE_FAILED;
    }
    uint32_t off = (uint32_t)a.i(0), n = a.len(1);
    const uint8_t *p = a.p(1);
    mp3_forget();
    while (n) {
        uint32_t k = 0x1000000u - off < n ? 0x1000000u - off : n;
        ddr_write(off, p, k);
        p += k;
        n -= k;
        off = 0;
    }
    snprintf(o.msg, o.msglen, "%u bytes written into the REU", (unsigned)a.len(1));
    return OPE_OK;
}
// SOCKET_CMD_POWEROFF (FW :161-163, MENU_C64_POWEROFF): the menu's Power off.
int op_dma_power_off(CoreCtx *, const OpArgs &, OpOut &o)
{
    return dma_call("u64.power_off", 0, 0, o);
}
