// update.run_record, the `.cfw` run (MultiCore T239.3 P2-1 and P2-5: the base's update.cc left it, it is this library's). A `.cfw` is loaded to the DDR2
// addresses its records name and the CPU jumps to the start address of the last record that has one: the "Run Update" of Gideon's file browser
// (filetype_u2p.cc), with the checks of his format 1 first. Gideon's own `.cfw` and ours (`trx64_update_*.cfw`) are the same container: a pre-header
// {fingerprint 0x4115ADDE read little endian, product family[16], product name[16]} and then records {load, length, start} with the bytes behind each.
// Writing the flash is the updater's work, never this file's: the library stages and jumps. Ported from TRX64U-MultiCore 75da955da^ coreapp/src/update.cc
// (update_run_record) onto the base API: the DDR2 window is io.ddr_write/ddr_read (trx64u_lib.h), the jump is sys.machine_handover (the same steps as
// the base's former CoreIo::run_at: the pixel layer given back, the rocker quiet, the interrupts off, the network's receive stopped; appsvc.cc
// appsvc_handover) and then the start address.
#include "trx64u_lib.h"
#include "trx64u_ops.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>

// The updaters' window: they load at 0x03000000 and keep .data/.bss/heap above their image and the stack below 0x03FFFFFC (host-app
// trx_time_handover.h). A record outside [UPD_WIN_LO, UPD_WIN_HI) is refused: it could overwrite the application that reads it (0x30000-0xE90000),
// the REU window or the cartridge ROM area from 0x03C00000, and nothing but an updater has a reason to be there.
#define UPD_WIN_LO 0x03000000u
#define UPD_WIN_HI 0x03C00000u
#define UPD_DDR_WINDOW 0x01000000u    // io.ddr_write(off, ...) is relative to this (svc_io.h)
#define UPD_TIME_ADDR 0x03F00000u     // the four words the updater reads its clock from (trx_time_handover.h)
#define UPD_FINGERPRINT 0x4115ADDEu   // the bytes DE AD 15 41 of the file, as a little endian word
#define UPD_RECORDS_MAX 8
#define UPD_CHUNK 32768u
// The product family of the board this library runs on. trx64u runs only on c64_basic, the bitstream of the C64 Ultimate board; the `.cfw` files that
// update that board name this family in their pre-header and no product (the update line's trx_pkg_check.py checks exactly that; it is on the Gitea
// branch gideon-app since U20 A5), and Gideon's update check on the board compares the field with getProductFamily(). The base API gives no product
// family (SvcBoard has none); the product name is the base's state "diag.board" (getProductString).
#define UPD_BOARD_FAMILY "U64MK2"

namespace {

struct Rec {
    uint32_t load, len, start;
    uint64_t at;                      // file offset of the data
};

bool le32(SvcFile *f, uint64_t off, uint32_t *v)
{
    uint8_t b[4];
    if (!g_api->fs->read(f, off, b, 4)) {
        return false;
    }
    *v = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
    return true;
}

bool is_cfw(const char *path)
{
    size_t n = strlen(path);
    return n > 4 && !strcasecmp(path + n - 4, ".cfw");
}

// The four words of trx_time_handover.h: the updater has no clock of its own on our bitstream. No valid time: all zero, so that nothing left over
// from an earlier run can pass for one.
void hand_time(void)
{
    uint32_t w[4] = { 0, 0, 0, 0 };
    uint8_t t[6];
    if (SVC_HAS(SvcSys, g_api->sys, wall_time) && g_api->sys->wall_time(t)) {
        uint32_t d = ((uint32_t)(2000 + t[0]) << 16) | ((uint32_t)t[1] << 8) | t[2];
        uint32_t tm = ((uint32_t)t[3] << 16) | ((uint32_t)t[4] << 8) | t[5];
        w[0] = 0x54585254u;                       // "TRXT"
        w[1] = d;
        w[2] = tm;
        w[3] = w[0] ^ d ^ tm ^ 0x5A17C0DEu;
    }
    ddr_write(UPD_TIME_ADDR - UPD_DDR_WINDOW, w, sizeof(w));
}

uint32_t sum32(const uint8_t *p, uint32_t n, uint32_t s)
{
    for (uint32_t i = 0; i < n; i++) {
        s = (s << 5) + (s >> 27) + p[i];
    }
    return s;
}

// 0 and the jump done (on the board it does not return); otherwise 1 and the reason in msg.
int run_record(const char *path, char *msg, int msglen)
{
    // the base's board profile: under the MultiCore host the host's menu runs updates (diag.cc state_host_multicore: board_detect's MultiCore
    // profile, api.md 6.17.5.5: the ICAP loader answers 0x7A and the partition control is linked or the hand-over page is valid)
    const char *host = g_api->ops->state("host.multicore", 0);
    if (host && host[0]) {
        snprintf(msg, msglen, "Updates run from the host's menu on this board.");
        return 1;
    }
    if (!g_api->io || !SVC_HAS(SvcSys, g_api->sys, machine_handover)) {
        snprintf(msg, msglen, "This application cannot start an update (no DDR2 window or no hand-over).");
        return 1;
    }
    if (!is_cfw(path)) {
        snprintf(msg, msglen, "%s is not a .cfw update file.", path);
        return 1;
    }
    SvcFile *f = g_api->fs->open(path, false);
    if (!f) {
        snprintf(msg, msglen, "Cannot open %s.", path);
        return 1;
    }
    int rc = 1;
    uint8_t *buf = 0, *back = 0;
    uint8_t pre[36];
    Rec rec[UPD_RECORDS_MAX];
    int nrec = 0;
    uint32_t start = 0;
    uint64_t size = g_api->fs->fsize(f), off = 36;
    uint32_t fp = 0;
    const char *product = 0;

    if (size < 36 + 12 || !g_api->fs->read(f, 0, pre, 36) || !le32(f, 0, &fp) || fp != UPD_FINGERPRINT) {
        snprintf(msg, msglen, "Invalid Update Image");
        goto done;
    }
    // His format 1: a family or a product that is named has to be this board's
    if (pre[4] && strncmp((const char *)pre + 4, UPD_BOARD_FAMILY, 16) != 0) {
        snprintf(msg, msglen, "Wrong Product Family");
        goto done;
    }
    product = pre[20] ? g_api->ops->state("diag.board", 0) : 0;
    if (pre[20] && (!product || strncmp((const char *)pre + 20, product, 16) != 0)) {
        snprintf(msg, msglen, "Update for Wrong Product");
        goto done;
    }
    // First pass, nothing is written: every record inside the file and inside the updaters' window, a start address inside its record
    while (off < size) {
        if (nrec >= UPD_RECORDS_MAX) {
            snprintf(msg, msglen, "Update image: more than %d records.", UPD_RECORDS_MAX);
            goto done;
        }
        Rec &r = rec[nrec];
        if (size - off < 12 || !le32(f, off, &r.load) || !le32(f, off + 4, &r.len) || !le32(f, off + 8, &r.start)) {
            snprintf(msg, msglen, "Update image: a record header is cut off.");
            goto done;
        }
        r.at = off + 12;
        if (r.len > size - r.at) {
            snprintf(msg, msglen, "Update image: record %d is cut off (the file ends %u bytes early).", nrec, (unsigned)(r.len - (size - r.at)));
            goto done;
        }
        if (r.load < UPD_WIN_LO || r.load >= UPD_WIN_HI || r.len > UPD_WIN_HI - r.load) {
            snprintf(msg, msglen, "Update image: record %d lies at $%08X, outside $%08X-$%08X.", nrec, (unsigned)r.load, (unsigned)UPD_WIN_LO, (unsigned)UPD_WIN_HI);
            goto done;
        }
        if (r.start) {
            if (r.start < r.load || r.start >= r.load + r.len) {
                snprintf(msg, msglen, "Update image: the start $%08X is not inside its record.", (unsigned)r.start);
                goto done;
            }
            start = r.start;
        }
        off = r.at + r.len;
        nrec++;
    }
    if (!nrec || !start) {
        snprintf(msg, msglen, "Update image: no start address.");
        goto done;
    }
    buf = (uint8_t *)g_api->mem->malloc(2 * UPD_CHUNK);       // one block for both halves: within the library's heap_peak (Makefile)
    if (!buf) {
        snprintf(msg, msglen, "Out of memory for the update (2 x %u bytes).", (unsigned)UPD_CHUNK);
        goto done;
    }
    back = buf + UPD_CHUNK;
    // Second pass: into DDR2 in chunks, then read back and compare, so that a stage that does not hold never gets the jump
    for (int k = 0; k < nrec; k++) {
        uint32_t sent = 0, got = 0;
        for (uint32_t d = 0; d < rec[k].len;) {
            uint32_t n = rec[k].len - d < UPD_CHUNK ? rec[k].len - d : UPD_CHUNK;
            if (!g_api->fs->read(f, rec[k].at + d, buf, n)) {
                snprintf(msg, msglen, "Cannot read %s at %u (the medium).", path, (unsigned)(rec[k].at + d));
                goto done;
            }
            ddr_write(rec[k].load + d - UPD_DDR_WINDOW, buf, n);
            sent = sum32(buf, n, sent);
            d += n;
        }
        for (uint32_t d = 0; d < rec[k].len;) {
            uint32_t n = rec[k].len - d < UPD_CHUNK ? rec[k].len - d : UPD_CHUNK;
            ddr_read(rec[k].load + d - UPD_DDR_WINDOW, back, n);
            got = sum32(back, n, got);
            d += n;
        }
        if (sent != got) {
            snprintf(msg, msglen, "The update did not stay in memory (record %d reads back different); not started.", k);
            goto done;
        }
    }
    hand_time();
    snprintf(msg, msglen, "Update started at $%08X.", (unsigned)start);
    lib_logf("[trx64u] update: %s, jump to 0x%08x", path, (unsigned)start);
    g_api->fs->close(f);
    g_api->mem->free(buf);
    g_api->sys->machine_handover();                          // there is no way back (svctab.h)
    ((void (*)(void))(uintptr_t)start)();
    for (;;) {
    }
done:
    if (buf) {
        g_api->mem->free(buf);
    }
    g_api->fs->close(f);
    return rc;
}

}  // namespace

int op_run_record(CoreCtx *, const OpArgs &a, OpOut &o)
{
    return run_record(a.s(0), o.msg, o.msglen) ? OPE_FAILED : OPE_OK;
}
