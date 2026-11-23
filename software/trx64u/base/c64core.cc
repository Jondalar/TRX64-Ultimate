// The C64 descriptor's code (U18): MultiCore's coreapp/cores/c64.cc (t239.3 beee9b09), the TRX2 descriptor of the trx64u core, moved into this library
// (their boundary map 2.11, 12.1). Its data (identity, ROMs, freeze, hotkeys, mount slot names, report registers, the mouse) is desc.toml; this file is
// what the data names: the ROM put operations, the freeze mode, the program loads (run, load and start, DMA, T64, run with app, the typed script), the
// cartridge run and remove, the service poll, the machine's memory as machine.read_mem / write_mem, and the hooks init, release, media_flush,
// media_pending and the mount slots. The program loader (relink, pointers, typed keys) is MultiCore's export "cbmprg"; the T64 reader is their
// coreapp/src/t64.cc cut to what this core uses. Every handler and hook runs with the core lock (svc_io.h: the register and RAM window calls need it).
#include "c64core.h"
#include "c64_go.h"
#include "trx64u_ops.h"
#include "u64cfg.h"
#include "trx64u_lib.h"
#include "c64u_regs.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>

// ---- the shared libraries' calls of this file, each answering as "none there" when its library is not loaded ------------------------------------------
static bool cart_active(void)
{
    const C64CartApi *c = X_GET(X_CART, C64CartApi, active);
    return c && c->active();
}

// The machine's cold reset: the base's operation, which holds the machine, calls the init hook, applies the settings and releases it.
static int cold_reset(char *msg, int msglen)
{
    return op_call("machine.cold_reset", 0, 0, msg, msglen) != OPE_OK;
}

// ---- the C64's memory through the RAM window -------------------------------------------------------------------------------------------------------
// In the memory-only mode every byte goes to the RAM's second port, $D000-$DFFF included, never a bus cycle into the chips; never used with the machine
// stopped by C64_STOP, where a byte is a bus cycle through the PLA (S15). Without the io group nothing is written and a read gives zeros.
static void ram_put(uint16_t addr, const void *p, uint32_t n)
{
    const SvcIo *io = g_api->io;
    if (io) {
        io->ram_write(addr, p, n);
    }
}
static void ram_get(uint16_t addr, void *p, uint32_t n)
{
    const SvcIo *io = g_api->io;
    if (io) {
        io->ram_read(addr, p, n);
    } else {
        memset(p, 0, n);
    }
}

// machine.read_mem / machine.write_mem (MultiCore's memops.h: every core registers the pair): the 64 KB RAM, through the second port.
int c64_mem_read(CoreCtx *, const OpArgs &a, OpOut &o)
{
    uint32_t addr = (uint32_t)a.i(0), n = (uint32_t)a.i(1);
    if (n > 0x10000u - addr) {
        snprintf(o.msg, o.msglen, "%u bytes at $%X cross the end of RAM ($FFFF)", (unsigned)n, (unsigned)addr);
        return OPE_ARG;
    }
    if (n > (uint32_t)o.datacap) {
        snprintf(o.msg, o.msglen, "at most %d bytes at a time", o.datacap);
        return OPE_ARG;
    }
    c64_ram_memonly(true);
    ram_get((uint16_t)addr, o.data, n);
    c64_ram_memonly(false);
    o.ndata = (int)n;
    snprintf(o.msg, o.msglen, "%u bytes at $%X (RAM)", (unsigned)n, (unsigned)addr);
    return OPE_OK;
}

static int hexv(char ch)
{
    return ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : -1;
}

int c64_mem_write(CoreCtx *, const OpArgs &a, OpOut &o)
{
    uint32_t addr = (uint32_t)a.i(0);
    const char *hex = a.s(1);
    size_t len = strlen(hex);
    if (len == 0 || (len & 1) || len / 2 > 127) {
        snprintf(o.msg, o.msglen, "data is hex text: two digits a byte, 1 to 127 bytes");
        return OPE_ARG;
    }
    uint8_t buf[127];
    uint32_t n = (uint32_t)(len / 2);
    for (uint32_t i = 0; i < n; i++) {
        int h = hexv(hex[2 * i]), l = hexv(hex[2 * i + 1]);
        if (h < 0 || l < 0) {
            snprintf(o.msg, o.msglen, "data has a character that is not a hex digit");
            return OPE_ARG;
        }
        buf[i] = (uint8_t)(h * 16 + l);
    }
    if (n > 0x10000u - addr) {
        snprintf(o.msg, o.msglen, "%u bytes at $%X cross the end of RAM ($FFFF)", (unsigned)n, (unsigned)addr);
        return OPE_ARG;
    }
    c64_ram_memonly(true);
    ram_put((uint16_t)addr, buf, n);
    c64_ram_memonly(false);
    snprintf(o.msg, o.msglen, "%u bytes written at $%X (RAM)", (unsigned)n, (unsigned)addr);
    return OPE_OK;
}

// ---- settings ----------------------------------------------------------------------------------------------------------------------------------
// GATE_CTRL b0 (trx_sys.v): with it the application's C64_STOP pauses the whole machine - CPU, VIC, CIAs and SID; the C64 picture is black while it
// stands (top.v vic_scaler .blank(c64_halted)), the state bus is writable - without it only the CPU, and the VIC keeps drawing.
int c64_freeze_mode(CoreCtx *, const OpArgs &a, OpOut &)
{
    const SvcIo *io = g_api->io;
    if (io) {
        uint32_t v = a.i(0) == 0 ? C64U_GATE_FRZ : 0;
        for (int i = 0; i < 4; i++) {
            io->wr(C64U_GATE_CTRL + i, (uint8_t)(v >> (8 * i)));
        }
    }
    return OPE_OK;
}

// ---- ROMs (desc.toml [[rom]] names these as put_op) ---------------------------------------------------------------------------------------------
// Plain copies at the aperture's offsets. No read-back: the KERNAL array is write only on Gideon's boards, and the other two are the same register file.
static int put_rom(const OpArgs &a, uint32_t ofs, uint32_t size, const char *role, OpOut &o)
{
    if (a.len(0) != size) {
        snprintf(o.msg, o.msglen, "%s: %u bytes, the machine takes %u", role, (unsigned)a.len(0), (unsigned)size);
        return OPE_FAILED;
    }
    c64_rom_write(ofs, a.p(0), size);
    return OPE_OK;
}
int c64_put_basic(CoreCtx *c, const OpArgs &a, OpOut &o)
{
    int rc = put_rom(a, C64_ROM_BASIC, C64_BASIC_BYTES, "BASIC", o);
    if (rc == OPE_OK) {
        trx64u_basic_put(c, a.p(0), a.len(0));                     // the SID player's cartridge carries the BASIC the machine runs with
    }
    return rc;
}
// also dma.kernal (the DMA service's KERNAL write, FW socket_dma.cc SOCKET_CMD_KERNALWRITE): the 8 KB image and its fast reset patch in place, no reset
int c64_put_kernal(CoreCtx *c, const OpArgs &a, OpOut &o)
{
    int rc = put_rom(a, C64_ROM_KERNAL, C64_KERNAL_BYTES, "KERNAL", o);
    if (rc == OPE_OK) {
        trx64u_kernal_put(c, a.p(0), a.len(0));                    // the copy the fast reset patch is made against
    }
    return rc;
}
int c64_put_char(CoreCtx *, const OpArgs &a, OpOut &o)
{
    return put_rom(a, C64_ROM_CHAR, C64_CHAR_BYTES, "CHAR", o);
}

// ---- the program loader: the export "cbmprg" (MultiCore's app/loaders/cbm_prg.cc) with the C64's numbers (c64.cc run_params, load_params) ------------
// Gideon's addresses (c64_subsys.cc, bootcrt.tas): BASIC at $0801, the pointer pair $2B/$2C and the three that follow, the keyboard buffer at $0277 with its
// count at $C6. A file runs from $0801 to $FFFF: the bytes go through the RAM's second port, under BASIC, the I/O area and the KERNAL alike. PRG_READY_MS is
// the least the machine runs after the reset before the first look at READY; not measured on a board (T104.2 5).
#define PRG_P00_HEADER  0x1A
#define PRG_MIN_SIZE    3
#define BASIC_START     0x0801
#define TXTTAB          0x2B
#define PRG_READY_MS    1000
#define READY_WAIT_MAX_MS 4000          // READY is when BASIC's cold start has set the pointer pair; the banner and the editor loop need a moment more
#define READY_SETTLE_MS   300

// "Run PRG": a load address of $0801 is BASIC (relinked to the machine's start of BASIC, RUN), anything else SYS; $0801-$FFFF behind a .P00 header.
static CbmPrgMachine run_machine(void)
{
    CbmPrgMachine m;
    memset(&m, 0, sizeof(m));
    m.size = sizeof(m);
    m.basic_starts[0] = BASIC_START;
    m.nbasic_starts = 1;
    m.txttab = TXTTAB;
    m.txttab_min = BASIC_START;
    m.txttab_max = 0x9FFF;
    m.vartab = 0x2D;
    m.arytab = 0x2F;
    m.strend = 0x31;
    m.kbd_buffer = 0x0277;
    m.kbd_count = 0xC6;
    m.ready_ms = PRG_READY_MS;
    m.p00_header = PRG_P00_HEADER;
    m.min_size = PRG_MIN_SIZE;
    m.max_size = PRG_P00_HEADER + 2 + (0x10000 - BASIC_START);
    return m;
}
// "Load PRG and start from": the file goes where it says, anywhere in $0000-$FFFF, nothing is relinked.
static CbmPrgMachine load_machine(void)
{
    CbmPrgMachine m = run_machine();
    m.nbasic_starts = 0;
    m.max_size = PRG_P00_HEADER + 2 + 0x10000;
    return m;
}

// The loader's table with the four members this file calls; null with the reason in msg when no library exports it.
static const CbmPrgApi *prg_lib(char *msg, int msglen)
{
    const CbmPrgApi *x = X_GET(X_PRG, CbmPrgApi, script_step);
    if (x && x->place && x->type_start && x->script_check) {
        return x;
    }
    snprintf(msg, msglen, "No program loader: no library exports \"" CBMPRG_API_NAME "\"");
    return 0;
}

// ---- the program job -----------------------------------------------------------------------------------------------------------------------------
// A program is started in two steps, because the menu holds the machine (C64_STOP) while an operation of it runs, and a machine that is held never reaches
// READY. The operation validates the file, resets the machine and queues the job; the service (c64.service, polled while the machine runs) waits for READY
// and then places the bytes and types RUN or SYS, or points the CPU. The time it waits is the time the machine has run, not the menu's.
enum { J_NONE = 0, J_RUN = 1, J_LOAD = 2, J_TYPE = 3 };

static struct {
    char program[56];                    // the last program started
    int job;
    char name[56];
    char path[FS_PATH_MAX];              // the file the job came from
    char program_path[FS_PATH_MAX];      // the last program started (the Library's [P] mark)
    uint16_t start;
    bool p00, go;
    uint32_t size;
    uint32_t ran_ms, last_ms, ready_at, ready_ms;
    char script[64];                     // J_TYPE: the pieces to type, '|' between them
    int script_pos;
} g_st;

// The file of the job, from its queue to its start; static because the job outlives the operation that queued it.
static uint8_t prgbuf[PRG_P00_HEADER + 2 + 0x10000];

static void forget_program(void)
{
    g_st.program[0] = 0;
    g_st.program_path[0] = 0;
}

const char *c64_prg_name(CoreCtx *)
{
    return g_st.program[0] ? g_st.program : 0;
}
const char *c64_prg_path(CoreCtx *)
{
    return g_st.program_path[0] ? g_st.program_path : 0;
}
const char *c64_prg_pending(CoreCtx *)
{
    return g_st.job == J_RUN ? "run" : (g_st.job == J_LOAD ? "load" : (g_st.job == J_TYPE ? "type" : 0));
}

static bool is_p00(const char *path)
{
    size_t l = strlen(path);
    return l > 4 && !strcasecmp(path + l - 4, ".p00");
}

static const char *base_name(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

// The whole file, at most m.max_size bytes, into prgbuf. 0 or a reason.
static int read_prg(const char *path, const CbmPrgMachine &m, uint32_t *size, char *msg, int msglen)
{
    SvcFile *f = g_api->fs->open(path, false);
    if (!f) {
        snprintf(msg, msglen, "cannot open %s", path);
        return 1;
    }
    uint64_t n = g_api->fs->fsize(f);
    uint32_t skip = is_p00(path) ? m.p00_header : 0;
    if (n < skip + m.min_size || n > m.max_size) {
        g_api->fs->close(f);
        snprintf(msg, msglen, "%s: %u bytes is not a program", path, (unsigned)fs_size32(n));
        return 1;
    }
    bool ok = g_api->fs->read(f, 0, prgbuf, (uint32_t)n);
    g_api->fs->close(f);
    if (!ok) {
        snprintf(msg, msglen, "cannot read %s (the medium is gone?)", path);
        return 1;
    }
    uint32_t load = (uint32_t)(prgbuf[skip] | (prgbuf[skip + 1] << 8));
    uint32_t len = (uint32_t)n - skip - 2;
    if (load + len > 0x10000u) {                               // a file that would wrap past $FFFF is refused before the machine is touched
        snprintf(msg, msglen, "%s: $%04X + %u bytes runs past $FFFF", path, (unsigned)load, (unsigned)len);
        return 1;
    }
    *size = (uint32_t)n;
    return 0;
}

// The program in prgbuf into the machine, through the RAM's second port with the memory-only mode on, so that $D000-$DFFF is RAM too and not a bus cycle
// into the chips (c64.cc place). 0 or a reason.
static int place(const CbmPrgApi *x, uint32_t size, const CbmPrgMachine &m, bool p00, CbmPrgPlaced *r, char *msg, int msglen)
{
    c64_ram_memonly(true);
    int rc = x->place(prgbuf, size, &m, p00, r, msg, msglen);
    c64_ram_memonly(false);
    return rc;
}

// F4 "DMA load mimics ID": after a DMA load that starts a program the device number is in $BA (TRX-Ultimate c64_subsys.cc:1070, CFG_C64_DMA_ID 8 .. 31),
// so a program that loads its next file from "the last device" finds it there.
static void poke_dma_id(void)
{
    uint8_t id = (uint8_t)setting_num("DMA Load Mimics ID", 8);
    ram_put(0x00BA, &id, 1);
}

static void remember(void)
{
    snprintf(g_st.program, sizeof(g_st.program), "%s", g_st.name);
    snprintf(g_st.program_path, sizeof(g_st.program_path), "%s", g_st.path);
}

static void start_job(int kind, const char *path)
{
    g_st.job = kind;
    g_st.ran_ms = g_st.last_ms = g_st.ready_at = 0;
    g_st.ready_ms = PRG_READY_MS;
    if (path) {
        fs_short_name(g_st.name, sizeof(g_st.name), base_name(path));
        snprintf(g_st.path, sizeof(g_st.path), "%s", path);
    }
}

// The operation's half: the file is read and checked, its .cfg loaded, the machine reset, the job queued. The machine is not touched when the file is no
// program or there is no program loader. The reset clears a job that is still waiting (the init hook), so the queue comes after it.
static int queue_prg(CoreCtx *c, int kind, const char *path, uint16_t start, char *msg, int msglen)
{
    if (!prg_lib(msg, msglen)) {
        return 1;
    }
    CbmPrgMachine m = kind == J_RUN ? run_machine() : load_machine();
    uint32_t size;
    if (read_prg(path, m, &size, msg, msglen)) {
        return 1;
    }
    trx64u_load_assoc_cfg(c, path, true);                      // filetype_prg.cc:186-190: the .cfg (or .usr) beside the file goes in before the program runs
    bool p00 = is_p00(path);
    bool go = kind == J_LOAD && c64_go_available();
    if (kind == J_LOAD && !go) {
        uint32_t skip = p00 ? m.p00_header : 0;
        uint16_t load = (uint16_t)(prgbuf[skip] | (prgbuf[skip + 1] << 8));
        if (load < 0x0400) {
            snprintf(msg, msglen, "$%04X is in the zero page, stack or KERNAL work area: a typed SYS cannot start it, and this bitstream "
                     "cannot set the CPU's address", load);
            return 1;
        }
    }
    if (cold_reset(msg, msglen)) {
        return 1;
    }
    start_job(kind, path);
    g_st.start = start;
    g_st.p00 = p00;
    g_st.go = go;
    g_st.size = size;
    lib_logf("[c64] %s: reset, %s queued for READY", path, kind == J_RUN ? "run" : "load and start");
    return 0;
}

struct GoPlace {
    const CbmPrgApi *x;
    const CbmPrgMachine *m;
    CbmPrgPlaced *r;
};
static int go_place(void *user, char *msg, int msglen)
{
    GoPlace *a = (GoPlace *)user;
    int rc = place(a->x, g_st.size, *a->m, g_st.p00, a->r, msg, msglen);
    if (!rc) {
        poke_dma_id();
    }
    return rc;
}

// The service's half, with the machine at READY.
static int fire(char *msg, int msglen)
{
    const CbmPrgApi *x = prg_lib(msg, msglen);
    if (!x) {
        return 1;
    }
    CbmPrgMachine m = g_st.job == J_RUN ? run_machine() : load_machine();
    CbmPrgPlaced r;
    memset(&r, 0, sizeof(r));
    if (g_st.job == J_RUN) {
        if (place(x, g_st.size, m, g_st.p00, &r, msg, msglen)) {
            return 1;
        }
        poke_dma_id();
        x->type_start(&m, &r);
        lib_logf("[c64] %s: $%04X-$%04X, %s", g_st.name, r.load, (unsigned)(r.end - 1) & 0xFFFF, r.basic ? "RUN" : "SYS");
        return 0;
    }
    if (g_st.go) {
        GoPlace pa = { x, &m, &r };
        if (c64_go(g_st.start, go_place, &pa, msg, msglen)) {
            return 1;
        }
    } else {
        if (place(x, g_st.size, m, g_st.p00, &r, msg, msglen)) {
            return 1;
        }
        poke_dma_id();
        r.start = g_st.start;
        x->type_start(&m, &r);
    }
    lib_logf("[c64] %s: $%04X-$%04X, start $%04X (%s)", g_st.name, r.load, (unsigned)(r.end - 1) & 0xFFFF, g_st.start, g_st.go ? "CPU address set" : "SYS typed");
    return 0;
}

// A notice for the person; it replaces one that waits (c64.cc wrote the context's notice unconditionally).
static void tell(const char *what)
{
    lib_logf("[c64] %s", what);
    const SvcCtx *x = g_api->ctx;
    if (x) {
        x->notice(what, false);
    }
}

static void service_job(void)
{
    if (!g_st.job) {
        return;
    }
    uint32_t now = now_ms();
    if (g_st.last_ms) {
        uint32_t dt = now - g_st.last_ms;
        g_st.ran_ms += dt > 200 ? 100 : dt;                    // a gap is the menu's: the machine did not run in it
    }
    g_st.last_ms = now;
    if (g_st.ran_ms < g_st.ready_ms) {
        return;
    }
    uint8_t b[2] = { 0, 0 };
    ram_get(TXTTAB, b, 2);
    if ((uint16_t)(b[0] | (b[1] << 8)) != BASIC_START && !(g_st.job == J_TYPE && g_st.ready_at)) {
        if (g_st.ran_ms > g_st.ready_ms + READY_WAIT_MAX_MS) {
            g_st.job = J_NONE;
            tell("The machine did not reach READY: the program was not started (are the ROMs the right ones?)");
        }
        return;
    }
    if (!g_st.ready_at) {
        g_st.ready_at = g_st.ran_ms;
    }
    if (g_st.ran_ms - g_st.ready_at < READY_SETTLE_MS) {
        return;
    }
    char msg[200];
    msg[0] = 0;
    if (g_st.job == J_TYPE) {
        const CbmPrgApi *x = prg_lib(msg, sizeof(msg));
        CbmPrgMachine m = run_machine();
        if (!x) {
            g_st.job = J_NONE;
            tell(msg);
        } else if (g_st.ran_ms - g_st.ready_at > 60000) {
            g_st.job = J_NONE;
            tell("The machine did not take the typed start (is the editor stuck?)");
        } else if (x->script_step(&m, g_st.script, &g_st.script_pos)) {
            g_st.job = J_NONE;
        }
        return;
    }
    if (!fire(msg, sizeof(msg))) {
        remember();
    } else {
        tell(msg);
    }
    g_st.job = J_NONE;
}

// c64.service (desc.toml [core] service_op, every 20 ms while the machine runs): the SID player's handshake (T228 R21 item 7), the program job, then the
// drive's write-back on the same task (T104.1 D15).
int c64_service(CoreCtx *c, const OpArgs &, OpOut &o)
{
    trx64u_sid_service(c);
    service_job();
    const Cbm1541Api *d = X_GET(X_DRIVE, Cbm1541Api, service);
    o.more = d && d->service();
    return OPE_OK;
}

// ---- the operations of the programs ----------------------------------------------------------------------------------------------------------------
int c64_run(CoreCtx *c, const OpArgs &a, OpOut &o)
{
    return queue_prg(c, J_RUN, a.s(0), 0, o.msg, o.msglen) ? OPE_FAILED : OPE_OK;
}
int c64_load(CoreCtx *c, const OpArgs &a, OpOut &o)
{
    return queue_prg(c, J_LOAD, a.s(0), (uint16_t)a.i(1), o.msg, o.msglen) ? OPE_FAILED : OPE_OK;
}

// "DMA": the file goes into RAM where its load address says; nothing is reset, nothing is started (Gideon's DMA load without the run).
int c64_dma(CoreCtx *, const OpArgs &a, OpOut &o)
{
    const CbmPrgApi *x = prg_lib(o.msg, o.msglen);
    CbmPrgMachine m = load_machine();
    uint32_t size;
    if (!x || read_prg(a.s(0), m, &size, o.msg, o.msglen)) {
        return OPE_FAILED;
    }
    CbmPrgPlaced r;
    if (place(x, size, m, is_p00(a.s(0)), &r, o.msg, o.msglen)) {
        return OPE_FAILED;
    }
    snprintf(o.msg, o.msglen, "Loaded $%04X-$%04X into RAM", r.load, (unsigned)(r.end - 1) & 0xFFFF);
    return OPE_OK;
}

// The .t64 container (the C64S layout): a 64 byte header with "C64" at the start, the used entries at +0x24; 32 byte entries from +0x40: type (1 a normal
// tape file), 1541 type, start, end (little endian), two reserved bytes, the offset in the file at +8. Entry `index` counts the entries of type 1 (at most
// 64, MultiCore t64.cc t64_list). Its image (load address, then the data) goes into prgbuf as if the file were a .prg. 0 or a reason.
static int load_t64(const char *path, int index, uint32_t *size, char *msg, int msglen)
{
    SvcFile *f = g_api->fs->open(path, false);
    if (!f) {
        snprintf(msg, msglen, "%s: entry %d cannot be read", path, index + 1);
        return 1;
    }
    uint64_t fsz = g_api->fs->fsize(f);
    uint8_t h[0x40];
    bool found = false;
    uint16_t start = 0, end = 0;
    uint32_t offset = 0;
    if (fsz >= 0x40 && g_api->fs->read(f, 0, h, 0x40) && !memcmp(h, "C64", 3)) {
        int used = h[0x24] | (h[0x25] << 8), n = 0;
        for (int i = 0; i < used && i < 256 && n < 64 && !found; i++) {
            uint8_t e[32];
            if (!g_api->fs->read(f, 0x40 + 32u * i, e, 32)) {
                break;
            }
            if (e[0] != 1) {
                continue;
            }
            if (n++ == index) {
                found = true;
                start = (uint16_t)(e[2] | (e[3] << 8));
                end = (uint16_t)(e[4] | (e[5] << 8));
                offset = (uint32_t)e[8] | ((uint32_t)e[9] << 8) | ((uint32_t)e[10] << 16) | ((uint32_t)e[11] << 24);
            }
        }
    }
    uint32_t len = end > start ? (uint32_t)(end - start) : 0;  // some files carry a wrong end address: then the rest of the file is the program
    if (found && offset < fsz && (len == 0 || offset + len > fsz)) {
        len = (uint32_t)(fsz - offset);
    }
    bool ok = found && offset < fsz && len + 2 <= sizeof(prgbuf) && len + 2 >= PRG_MIN_SIZE;
    if (ok) {
        prgbuf[0] = (uint8_t)start;
        prgbuf[1] = (uint8_t)(start >> 8);
        ok = g_api->fs->read(f, offset, prgbuf + 2, len);
    }
    g_api->fs->close(f);
    if (!ok) {
        snprintf(msg, msglen, "%s: entry %d cannot be read", path, index + 1);
        return 1;
    }
    *size = len + 2;
    return 0;
}

int c64_run_t64(CoreCtx *, const OpArgs &a, OpOut &o)
{
    uint32_t size;
    if (!prg_lib(o.msg, o.msglen) || load_t64(a.s(0), a.i(1), &size, o.msg, o.msglen)) {
        return OPE_FAILED;
    }
    if (cold_reset(o.msg, o.msglen)) {
        return OPE_FAILED;
    }
    start_job(J_RUN, a.s(0));
    g_st.p00 = false;
    g_st.go = false;
    g_st.size = size;
    return OPE_OK;
}

int c64_dma_t64(CoreCtx *, const OpArgs &a, OpOut &o)
{
    const CbmPrgApi *x = prg_lib(o.msg, o.msglen);
    uint32_t size;
    if (!x || load_t64(a.s(0), a.i(1), &size, o.msg, o.msglen)) {
        return OPE_FAILED;
    }
    CbmPrgPlaced r;
    if (place(x, size, load_machine(), false, &r, o.msg, o.msglen)) {
        return OPE_FAILED;
    }
    snprintf(o.msg, o.msglen, "Loaded $%04X-$%04X into RAM", r.load, (unsigned)(r.end - 1) & 0xFFFF);
    return OPE_OK;
}

// T228 R21 #14, FW user_file_interaction.cc:211-296 (S_runApp): the file goes into the REU's memory behind a 32 bit little-endian length (the length at 0,
// the bytes from 4), then /Flash/apps/<ext>.prg - ext the first three characters of the file's extension - is run as "Run PRG" runs a file. The REU memory
// is the DDR2 window from 0; a cartridge image in it refuses (FW :215). The registry name is MultiCore's PRG_APP_OP: the base offers "Run with App" only
// for a core that registers it.
int c64_run_with_app(CoreCtx *c, const OpArgs &a, OpOut &o)
{
    const char *path = a.s(0);
    const char *dot = strrchr(base_name(path), '.');
    uint64_t sz = 0;
    bool dir = false;
    if (!dot || !dot[1] || !g_api->fs->stat(path, &sz, &dir) || dir) {
        snprintf(o.msg, o.msglen, "No file %s", path);
        return OPE_FAILED;
    }
    if (sz > 0x1000000u - 4) {
        snprintf(o.msg, o.msglen, "The file is larger than the REU's memory");
        return OPE_FAILED;
    }
    if (cart_active()) {
        snprintf(o.msg, o.msglen, "A cartridge uses the REU's memory: remove it first");
        return OPE_FAILED;
    }
    char app[64], ext[4];
    int k = 0;
    for (; k < 3 && dot[1 + k]; k++) {                         // the app is named by the first three letters of the extension
        ext[k] = dot[1 + k];
    }
    ext[k] = 0;
    app[0] = 0;
    SvcFsEntry r[6];
    int nr = g_api->fs->list("/", r, 6);
    for (int i = 0; i < nr && !app[0]; i++) {
        if (!strcasecmp(r[i].name, "flash")) {
            snprintf(app, sizeof(app), "/%s/apps/%s.prg", r[i].name, ext);
        }
    }
    uint64_t asz = 0;
    if (!app[0] || !g_api->fs->stat(app, &asz, &dir) || dir) {
        snprintf(o.msg, o.msglen, "There is no app for .%s files in the flash's apps folder", ext);
        return OPE_FAILED;
    }
    uint32_t cap = 0;
    uint8_t *buf = xfer_alloc(&cap);
    if (!buf) {
        snprintf(o.msg, o.msglen, "No memory for the transfer");
        return OPE_FAILED;
    }
    SvcFile *f = g_api->fs->open(path, false);
    if (!f) {
        xfer_free(buf);
        snprintf(o.msg, o.msglen, "Cannot open %s", path);
        return OPE_FAILED;
    }
    uint32_t n = (uint32_t)sz;
    bool ok = true;
    for (uint32_t done = 0; ok && done < n;) {
        uint32_t take = n - done < cap ? n - done : cap;
        ok = g_api->fs->read(f, done, buf, take);
        ddr_write(4 + done, buf, take);
        done += take;
    }
    g_api->fs->close(f);
    xfer_free(buf);
    if (!ok) {
        snprintf(o.msg, o.msglen, "Cannot read %s (the medium is gone?)", path);
        return OPE_FAILED;
    }
    uint8_t len[4] = { (uint8_t)n, (uint8_t)(n >> 8), (uint8_t)(n >> 16), (uint8_t)(n >> 24) };
    ddr_write(0, len, 4);
    return queue_prg(c, J_RUN, app, 0, o.msg, o.msglen) ? OPE_FAILED : OPE_OK;
}

// c64.type_script (desc.toml [core] type_op; T104.1 D15, Mount & Run): reset, and type the script at READY in pieces of at most ten keys
int c64_type(CoreCtx *, const OpArgs &a, OpOut &o)
{
    const CbmPrgApi *x = prg_lib(o.msg, o.msglen);
    if (!x || x->script_check(a.s(0), o.msg, o.msglen)) {
        return OPE_FAILED;
    }
    if (cold_reset(o.msg, o.msglen)) {
        return OPE_FAILED;
    }
    start_job(J_TYPE, 0);
    snprintf(g_st.script, sizeof(g_st.script), "%s", a.s(0));
    g_st.script_pos = 0;
    lib_logf("[c64] reset, typing queued for READY");
    return OPE_OK;
}

// The Ultimate DMA service's raw write (FW socket_dma.cc:146-149, c64_subsys.cc:819-821 dma_load_raw_buffer): the bytes go into the 64 KB the machine sees,
// through the RAM's second port, at the 16 bit address, with no reset and no start. A transfer past $FFFF is refused (nothing wraps).
int c64_dma_write(CoreCtx *, const OpArgs &a, OpOut &o)
{
    uint32_t addr = (uint32_t)a.i(0), n = a.len(1);
    if (!n || n > 0x10000u - addr) {
        snprintf(o.msg, o.msglen, "%u bytes at $%X do not lie in the 64 KB", (unsigned)n, (unsigned)addr);
        return OPE_ARG;
    }
    c64_ram_memonly(true);
    ram_put((uint16_t)addr, a.p(1), n);
    c64_ram_memonly(false);
    snprintf(o.msg, o.msglen, "%u bytes written at $%X", (unsigned)n, (unsigned)addr);
    return OPE_OK;
}

// "Set as Kernal / BASIC / Char ROM": the file becomes the setting (so every start takes it) and goes into the machine at once
int c64_set_rom(CoreCtx *, const OpArgs &a, OpOut &o)
{
    static const char *const keys[3] = { "Kernal ROM", "Basic ROM", "Char ROM" };
    int w = a.i(1);
    OpIn in[2] = { in_str("key", keys[w]), in_str("value", a.s(0)) };
    if (op_call("settings.set", in, 2, o.msg, o.msglen) != OPE_OK) {
        return OPE_FAILED;
    }
    OpIn v[1] = { in_int("value", 0) };
    if (op_call("c64.fx_roms", v, 1, o.msg, o.msglen) != OPE_OK) {
        return OPE_FAILED;
    }
    snprintf(o.msg, o.msglen, "%s is now %s (the machine restarts on RESET)", keys[w], a.s(0));
    return OPE_OK;
}

// ---- the cartridge (the export "c64cart") --------------------------------------------------------------------------------------------------------
// The .crt at `path` placed and the machine reset into it (c64cart.run); the program that ran is gone with it. 0, or the reason.
static int cart_run(const char *path, char *msg, int msglen)
{
    const C64CartApi *k = X_GET(X_CART, C64CartApi, run);
    if (!k) {
        snprintf(msg, msglen, "No cartridge library: the cartridge cannot be started");
        return 1;
    }
    if (k->run(path, msg, msglen)) {
        return 1;
    }
    forget_program();
    return 0;
}

// The cartridge taken away and the machine cold reset (c64cart.remove); the program's name goes, its path stays (c64.cc). 0, or the reason; -1 with the
// reason when there is no cartridge library.
static int cart_remove(char *msg, int msglen)
{
    const C64CartApi *k = X_GET(X_CART, C64CartApi, remove);
    if (!k) {
        snprintf(msg, msglen, "No cartridge library");
        return -1;
    }
    if (k->remove(msg, msglen)) {
        return 1;
    }
    g_st.program[0] = 0;
    return 0;
}

// The Library's .crt: its .cfg first (filetype_crt.cc:75), then the cartridge. The slot is told changed whatever the answer: a run that fails after the
// machine was held leaves the port empty.
int c64_run_crt(CoreCtx *c, const OpArgs &a, OpOut &o)
{
    trx64u_load_assoc_cfg(c, a.s(0), false);
    int rc = cart_run(a.s(0), o.msg, o.msglen);
    return rc ? OPE_FAILED : OPE_OK;
}

int c64_remove_cart(CoreCtx *, const OpArgs &, OpOut &o)
{
    int rc = cart_remove(o.msg, o.msglen);
    return rc ? OPE_FAILED : OPE_OK;
}

// ---- the mount slots (core_hooks.h slot_*: MountSlots' meaning; c64.cc slot_spec, slot_mount, slot_eject) --------------------------------------------
// SLOT_DRIVE_A is the drive library's slot. SLOT_CART's spec is the path of the .crt in the port, and its mount is the Library's run without the .cfg:
// the program that was running is gone with it. No mount here releases the machine: the drive's has no reset, and the cartridge's reset is the base's
// machine.cold_reset, which while ctx.state has CTX_MEDIA_RESTORING leaves the machine held for the base to release. A mount or an eject asked for here
// is the base's doing. The shared libraries behind both slots tell the base of every change they make to their slot (ctx.mount_notify, by the slot's
// name), so this library calls mount_notify nowhere.

// `s` into buf: its length; 0 for none (buf ""); -1 when it does not fit in n - 1 bytes
static int put_spec(const char *s, char *buf, int n)
{
    if (n > 0) {
        buf[0] = 0;
    }
    if (!s || !s[0]) {
        return 0;
    }
    size_t len = strlen(s);
    if (n <= 0 || len > (size_t)(n - 1)) {
        return -1;
    }
    memcpy(buf, s, len + 1);
    return (int)len;
}

int c64_hook_slot_spec(int slot, char *buf, int n)
{
    const char *s = 0;
    if (slot == SLOT_DRIVE_A) {
        const Cbm1541Api *d = X_GET(X_DRIVE, Cbm1541Api, slot_spec);
        s = d ? d->slot_spec() : 0;
    } else if (slot == SLOT_CART && cart_active()) {
        const C64CartApi *k = X_GET(X_CART, C64CartApi, path);
        s = k ? k->path() : 0;
    }
    return put_spec(s, buf, n);
}

int c64_hook_slot_mount(int slot, const char *spec, char *msg, int msglen)
{
    if (slot == SLOT_DRIVE_A) {
        const Cbm1541Api *d = X_GET(X_DRIVE, Cbm1541Api, slot_mount);
        if (!d) {
            snprintf(msg, msglen, "This machine has no drive A");
            return 1;
        }
        return d->slot_mount(spec, msg, msglen);
    }
    if (slot == SLOT_CART) {
        return cart_run(spec, msg, msglen);
    }
    snprintf(msg, msglen, "No mount slot %d", slot);
    return 1;
}

int c64_hook_slot_eject(int slot, char *msg, int msglen)
{
    if (slot == SLOT_DRIVE_A) {
        const Cbm1541Api *d = X_GET(X_DRIVE, Cbm1541Api, slot_eject);
        return d ? d->slot_eject(msg, msglen) : 0;                   // no drive library: the slot is empty
    }
    if (slot == SLOT_CART) {
        int rc = cart_remove(msg, msglen);
        return rc < 0 ? 0 : rc;                                      // no cartridge library: the port is empty
    }
    snprintf(msg, msglen, "No mount slot %d", slot);
    return 1;
}

// ---- the hooks (core_hooks.h; lib_trx64u.cc exports them) ------------------------------------------------------------------------------------------
// init: CTRL+P / CTRL+N at power-on (once, before the settings are applied, T228 R21), the machine held in reset (C64_MODE), drive A reset with it when
// "Resets when C64 resets" is on, and what the reset ends: a program that was still waiting for READY, a SID tune that was waiting for its player
// cartridge, the SID player's cartridge itself (the one before it comes back: the cartridge slot changes), the program the machine had run (T228 R20 1/7).
// The drive reset is cbm1541.reset, which makes no drive (-1: none there); its result is not the reset's (c64.cc ignored it), a failure is logged.
int c64_hook_init(char *, int)
{
    u64cfg_power_on_keys();
    c64_mode_reset(true);
    if (setting_ix("Resets when C64 resets", 1)) {
        const Cbm1541Api *d = X_GET(X_DRIVE, Cbm1541Api, reset);
        char m[OP_MSG_MAX];
        m[0] = 0;
        int rc = d ? d->reset(m, sizeof(m)) : -1;
        if (rc && rc != -1) {
            lib_logf("[c64] drive A reset: %s", m);
        }
    }
    g_st.job = J_NONE;
    trx64u_sid_cancel();
    const C64CartApi *k = X_GET(X_CART, C64CartApi, player_end);
    if (k) {
        k->player_end();
    }
    forget_program();
    return 0;
}

// release: the application's reset let go; the only place its hold is taken away
int c64_hook_release(char *, int)
{
    c64_mode_reset(false);
    return 0;
}

// media_flush / media_pending (T180 D4): drive A's write-back, the drive library's; no drive library: nothing to write
int c64_hook_media_flush(char *msg, int msglen)
{
    const Cbm1541Api *d = X_GET(X_DRIVE, Cbm1541Api, media_flush);
    return d ? d->media_flush(msg, msglen) : 0;
}

bool c64_hook_media_pending(void)
{
    const Cbm1541Api *d = X_GET(X_DRIVE, Cbm1541Api, media_pending);
    return d && d->media_pending();
}
