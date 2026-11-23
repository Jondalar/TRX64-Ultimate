// The machine a state file describes (ringmachine.h).
#include "ringmachine.h"
#include "trx_ring.h"
#include "ringsys.h"
#include <string.h>

// The path in a slot's spec (ctx.media_source: "ro /usb0/x.d64", "unlinked /usb0/x.d64", "/usb0/x.crt"): the text from the first '/'. mode, when
// asked: 1 read only, 2 unlinked, 0 read/write. "" when the slot is empty, unknown or the base has no ctx group.
static void slot_path(const char *slot, char *path, int n, int *mode)
{
    path[0] = 0;
    if (mode) {
        *mode = 0;
    }
    const SvcCtx *ctx = g_api->ctx;
    char spec[256];
    if (!ctx || !ctx->media_source(slot, spec, sizeof(spec))) {
        return;
    }
    const char *p = strchr(spec, '/');
    if (!p) {
        return;
    }
    if (mode) {
        *mode = !strncmp(spec, "ro ", 3) ? 1 : !strncmp(spec, "unlinked ", 9) ? 2 : 0;
    }
    r_snprintf(path, n, "%s", p);
}

// The System Mode that runs, from C64_VIDEOFORMAT (u64_config.cc writes the colour timing's mode bits there): PAL 0, NTSC 1, PAL-60 2, NTSC-50 3. The /L
// timings (4, 5) have the mode bits of PAL-60 and NTSC-50 and read as those; the trx64u menu offers the first four only. -1 without the io group.
static int machine_now(void)
{
    if (!g_api->io) {
        return -1;
    }
    uint8_t v = io_bus_rd(C64U_BUS_VIDEOFORMAT);
    bool ntsc = (v & C64U_VIDEO_NTSC_ENCODING) != 0, hz60 = (v & C64U_VIDEO_60_HZ) != 0;
    return ntsc ? (hz60 ? 1 : 3) : (hz60 ? 2 : 0);
}

// ---- the hooks of the source (ctx unused: the export knows its cartridge and drive) ----------------------------------------------------------------------
static void cart_rom_read(void *, uint32_t off, uint8_t *dst, uint32_t n)
{
    if (const C64CartApi *cc = R_TABLE(C64CartApi, RX_C64CART, image_read)) {
        cc->image_read(off, dst, n);
    }
}

static void cart_rom_write(void *, uint32_t off, const uint8_t *src, uint32_t n)
{
    if (const C64CartApi *cc = R_TABLE(C64CartApi, RX_C64CART, image_write)) {
        cc->image_write(off, src, n);
    }
}

static int cart_for_state(void *, const char *path)
{
    const C64CartApi *cc = R_TABLE(C64CartApi, RX_C64CART, for_state);
    if (!cc) {
        return 1;
    }
    char m[96];
    m[0] = 0;
    int rc = cc->for_state(path, m, sizeof(m));
    if (rc) {
        r_log("[C64RE] the state's cartridge: %s\n", m);
    }
    return rc;
}

// The GMod2's 93C86: the 2048 bytes written, its dirty flag cleared (c64cart.eeprom_put), so only what the C64 writes from here on is a change.
static void cart_eeprom_put(void *, const uint8_t *data)
{
    if (const C64CartApi *cc = R_TABLE(C64CartApi, RX_C64CART, eeprom_put)) {
        cc->eeprom_put(data);
    }
}

// FW build_machine_mode: the file's System Mode for this run only (settings.override_value: nothing reaches the .cfg), then applied. 0 when the setting
// has that value already or now has it; 1 when there is no such setting or the menu does not offer that mode.
static int machine_set(void *, int machine)
{
    const SvcSettings *s = g_api->settings;
    int ix = s ? s->find("System Mode") : -1;
    if (ix < 0) {
        return 1;
    }
    if (s->value(ix) == machine) {
        return 0;
    }
    int nvalues = 0, base = 0;
    if (machine < 0 || !s->info(ix, &nvalues, &base) || machine >= nvalues) {
        return 1;
    }
    s->override_value(ix, machine);
    s->apply(ix);
    return s->value(ix) == machine ? 0 : 1;
}

// FW build_drive: drive A's image goes in before the drive's own state (a mount resets the drive); spec is cbm1541's own, "ro <path>" / "unlinked <path>" / "<path>"
static int drive_mount(void *, const char *spec)
{
    const Cbm1541Api *d = R_TABLE(Cbm1541Api, RX_CBM1541, slot_mount);
    if (!d) {
        return 1;
    }
    char m[96];
    return d->slot_mount(spec, m, sizeof(m));
}

void ring_machine_build(RingMachine *m, bool load)
{
    memset(m, 0, sizeof(*m));
    TrxC64reSrc *s = &m->src;
    s->machine = machine_now();

    // drive A: type, power and unit from the drive, the image from its slot
    const Cbm1541Api *drv = R_TABLE(Cbm1541Api, RX_CBM1541, drive_info);
    Cbm1541Info info;
    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    if (drv && drv->drive_info(0, &info)) {
        s->drive_type = info.type == CBM1541_TYPE_1581 ? "1581" : "1541";
        s->drive_power = info.powered != 0;
        s->drive_unit = info.unit;
        slot_path(RING_SLOT_DRIVE_A, m->disk_path, sizeof(m->disk_path), &s->disk_mode);
        s->disk_path = m->disk_path[0] ? m->disk_path : NULL;
    }

    // the cartridge in the port: its file from the slot, its image and flags from c64cart
    const C64CartApi *cc = R_TABLE(C64CartApi, RX_C64CART, active);
    if (cc && cc->active()) {
        slot_path(RING_SLOT_CARTRIDGE, m->cart_path, sizeof(m->cart_path), NULL);
        s->cart_src = m->cart_path[0] ? m->cart_path : NULL;
        if (const C64CartApi *c = R_TABLE(C64CartApi, RX_C64CART, image_bytes)) {
            s->rom_bytes = c->image_bytes();
        }
        if (const C64CartApi *c = R_TABLE(C64CartApi, RX_C64CART, require)) {
            s->cart_require = c->require();
        }
        if (const C64CartApi *c = R_TABLE(C64CartApi, RX_C64CART, prohibit)) {
            s->cart_prohibit = c->prohibit();
        }
        if (const C64CartApi *c = R_TABLE(C64CartApi, RX_C64CART, writable)) {
            s->cart_writable = c->writable();
        }
        if (!load) {
            if (R_TABLE(C64CartApi, RX_C64CART, image_read)) {
                s->rom_read = cart_rom_read;
            }
            // U9 D3a: a GMod2's saved game is in the 93C86, outside the ROM window (FW trx_c64re_dump_ex: the same test on C64_CARTRIDGE_TYPE)
            const C64CartApi *e = R_TABLE(C64CartApi, RX_C64CART, eeprom_get);
            if (e && trx_ring_cart_has_eeprom(io_cart_rd(C64U_CART_TYPE))) {
                e->eeprom_get(m->eeprom);
                s->eeprom = m->eeprom;
            }
        }
    }

    if (load) {
        if (R_TABLE(C64CartApi, RX_C64CART, image_write)) {
            s->rom_write = cart_rom_write;
        }
        if (R_TABLE(C64CartApi, RX_C64CART, for_state)) {
            s->cart_for_state = cart_for_state;
        }
        if (R_TABLE(C64CartApi, RX_C64CART, eeprom_put)) {
            s->eeprom_put = cart_eeprom_put;
        }
        if (g_api->settings) {
            s->machine_set = machine_set;
        }
        if (R_TABLE(Cbm1541Api, RX_CBM1541, slot_mount)) {
            s->drive_mount = drive_mount;
        }
    }
}
