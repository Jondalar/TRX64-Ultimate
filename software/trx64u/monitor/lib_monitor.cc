// The entry of the monitor library (U18 D5) and its platform. lib_register hands the base the monitor's operations, its state value, its poll and its
// export "textgrid" (the F4 screen); the platform calls trxmon's engine makes (mach.h, rpc.h: what trxmon's main.c gives the firmware's trxmon) go to the
// base's tables (api->sys, api->mem, api->fs, api->ops) and to the base 3.3 groups (base33.h: sync, ctx, the members appended to SvcSys).
// PROVENANCE: monitor.cc of MultiCore's core application (TRX64U-MultiCore 3b8e2955, coreapp/src): its operations, monitor_register() and the plat_*
// calls moved here.
#include "libapi.h"
#include "libfmt.h"
#include "base33.h"
#include "textgrid.h"
#include "monitor.h"
#include "monlib.h"

extern "C" {
#include "trxmon/mach.h"
#include "trxmon/rpc.h"
}

const LibApi *g_api;

static_assert(sizeof(u2p_video_t) == 20, "u2p_video_t: the layout monitor.cc reads through app_api.h");

// ---- what monitor.cc takes of the base (monlib.h) ---------------------------------------------------------------------------------------------------------
bool monlib_trx(MonTrxBase *t)
{
    const SvcTrx *x = g_api->trx;
    if (!x) {
        return false;
    }
    t->regs = x->regs;
    t->caps = x->caps;
    t->acquire = x->acquire;
    t->release = x->release;
    t->announce = x->announce;
    return true;
}

int monlib_trx_video(void *v)
{
    const SvcTrx *x = g_api->trx;
    return x && x->video ? x->video((u2p_video_t *)v) : OPE_UNSUPPORTED;
}

const void *monlib_find(const char *name, uint32_t major, uint32_t *size)
{
    *size = 0;
    return g_api->lib && g_api->lib->find ? g_api->lib->find(name, U2A3_VERSION(major, 0), size) : 0;
}

void monlib_log(const char *text)
{
    char t[240];
    sbuf_t b;
    sb_init(&b, t, sizeof(t));
    sb_put(&b, "monitor: ");
    sb_put(&b, text);
    g_api->log(sb_str(&b));
}

// ---- what trxmon's main.c gave the engine (mach.h, rpc.h) ------------------------------------------------------------------------------------------------------
extern "C" {
uint32_t plat_ms(void) { return g_api->sys->ms(); }

// sys.yield (base 3.3). A base without it gets nothing here: sleep_ms(0) sleeps a whole 5 ms tick, and the waits that yield (mach.c wait_status) sleep
// one PLAT_SLEEP_MIN_MS every 20 ms of waiting on their own (breathe).
void plat_yield(void)
{
    const SvcSysAdd *s = b33_sys();
    if (s && s->yield) {
        s->yield();
    }
}
void plat_sleep(uint32_t ms) { g_api->sys->sleep_ms(ms < PLAT_SLEEP_MIN_MS ? PLAT_SLEEP_MIN_MS : ms); }

// A file through the base's file table: reads and writes at the position the calls before left. "w" makes the file new and empty first: SvcFs opens only
// files that are there, and opens them as they are.
struct PlatFile {
    SvcFile *f;
    uint64_t pos, size;
};

void *plat_fopen(const char *path, const char *mode)
{
    const SvcFs *fs = g_api->fs;
    if (!fs) {
        return 0;
    }
    bool w = mode[0] == 'w';
    if (w) {
        uint8_t none = 0;
        if (!fs->save(path, &none, 0)) {
            return 0;
        }
    }
    SvcFile *sf = fs->open(path, w);
    if (!sf) {
        return 0;
    }
    PlatFile *f = (PlatFile *)g_api->mem->malloc(sizeof(PlatFile));
    if (!f || (w && !fs->writable(sf))) {           // a medium that is read only gives a read only file
        if (f) {
            g_api->mem->free(f);
        }
        fs->close(sf);
        return 0;
    }
    f->f = sf;
    f->pos = 0;
    f->size = w ? 0 : fs->fsize(sf);
    return f;
}

int plat_fread(void *buf, int n, void *fp)
{
    PlatFile *f = (PlatFile *)fp;
    uint64_t left = f->size > f->pos ? f->size - f->pos : 0;
    uint32_t k = n <= 0 ? 0 : (left < (uint64_t)n ? (uint32_t)left : (uint32_t)n);
    if (!k) {
        return 0;
    }
    if (!g_api->fs->read(f->f, f->pos, buf, k)) {
        return -1;
    }
    f->pos += k;
    return (int)k;
}

int plat_fwrite(const void *buf, int n, void *fp)
{
    PlatFile *f = (PlatFile *)fp;
    if (n <= 0) {
        return n < 0 ? -1 : 0;
    }
    if (!g_api->fs->write(f->f, f->pos, buf, (uint32_t)n)) {
        return -1;
    }
    f->pos += (uint32_t)n;
    return n;
}

void plat_fclose(void *fp)
{
    PlatFile *f = (PlatFile *)fp;
    g_api->fs->close(f->f);
    g_api->mem->free(f);
}

int plat_mkdir(const char *path) { return g_api->fs && g_api->fs->mkdir(path) ? 0 : -1; }

int plat_exists(const char *dir, const char *name)
{
    char p[FS_PATH_MAX];
    sbuf_t b;
    sb_init(&b, p, sizeof(p));
    sb_put(&b, dir);
    sb_putc(&b, '/');
    sb_put(&b, name);
    uint64_t sz;
    bool d;
    return g_api->fs && !b.full && g_api->fs->stat(sb_str(&b), &sz, &d) ? 1 : 0;
}

// The first PLAT_DIR_MAX entries of a directory (the base lists into a buffer of that many): the monitor prints the names, and the 55 characters of a
// SvcFsEntry are enough there. The buffer is taken for the call.
#define PLAT_DIR_MAX 64
int plat_dir(const char *path, plat_dir_cb cb, void *ctx)
{
    if (!g_api->fs) {
        return -1;
    }
    SvcFsEntry *e = (SvcFsEntry *)g_api->mem->malloc(PLAT_DIR_MAX * sizeof(SvcFsEntry));
    if (!e) {
        return -1;
    }
    int n = g_api->fs->list(path, e, PLAT_DIR_MAX);
    for (int i = 0; i < n; i++) {
        if (cb(ctx, e[i].name, fs_size32(e[i].size), e[i].dir ? 1 : 0)) {
            break;                                  // the callback stops the listing (as fs->dir of the firmware)
        }
    }
    g_api->mem->free(e);
    return n < 0 ? -1 : 0;
}

int plat_rmdir(const char *path) { return g_api->fs && g_api->fs->remove(path) ? 0 : -1; }

// The firmware's configuration items the verbs read: here the settings of the core, the state value "set.<item>" (its display text).
int plat_config(const char *, const char *item, char *buf, int len)
{
    if (!g_api->ops || !g_api->ops->state || len <= 0) {
        return -1;
    }
    char key[96];
    sbuf_t k;
    sb_init(&k, key, sizeof(key));
    sb_put(&k, "set.");
    sb_put(&k, item);
    const char *v = k.full ? 0 : g_api->ops->state(sb_str(&k), 0);
    if (!v) {
        return -1;
    }
    sbuf_t b;
    sb_init(&b, buf, len);
    sb_put(&b, v);
    sb_str(&b);
    return b.len;
}

// The file behind a medium (rpc.h: role 0 cartridge, 1 drive A, 2 drive B; the full path and its length, 0 when the slot holds nothing). The base
// answers by the slot's name in the core's descriptor (ctx.media_source). The names are the mount slots of MultiCore's C64 descriptor (c64.cc,
// slot_names: "Drive A", "Cartridge", T239.3 boundary d6afb393); it has no drive B, so role 2 answers 0. media_source gives the engine's spec, which may
// carry a word before the path ("ro /usb0/x.d64"): the path starts at the first '/'.
static const char *const s_media_slot[] = { "Cartridge", "Drive A" };

int plat_media(int role, char *buf, int len)
{
    const SvcCtx *c = b33_ctx();
    if (role < 0 || role >= (int)(sizeof(s_media_slot) / sizeof(s_media_slot[0])) || !c || !c->media_source || len <= 0) {
        return 0;
    }
    char spec[FS_PATH_MAX + 16];
    if (!c->media_source(s_media_slot[role], spec, (int)sizeof(spec))) {
        return 0;
    }
    const char *p = spec;
    while (*p && *p != '/') {
        p++;
    }
    int n = 0;
    while (p[n] && n < len - 1) {
        buf[n] = p[n];
        n++;
    }
    buf[n] = 0;
    return n;
}

// The application image's version and the short hash of its commit (sys.app_version, sys.app_git; base 3.3). "" on a base without them (rpc.h).
const char *plat_firmware_git(void)
{
    const SvcSysAdd *s = b33_sys();
    return s && s->app_git ? s->app_git() : "";
}

const char *plat_firmware_version(void)
{
    const SvcSysAdd *s = b33_sys();
    return s && s->app_version ? s->app_version() : "";
}
}

// ---- the engine, one task at a time -----------------------------------------------------------------------------------------------------------------------------
// trxmon's state, the session and the RPC holder are used from two tasks: the library's poll (monitor.serve), which the base runs on a task of its own
// under the core lock, with the menu open too (when_frozen), and the menu task, which calls the monitor's operations (monitor.key runs there without the
// core lock, KF_DIRECT; monitor.release from the base library's menu_closed hook, also without it) and every call of the export "textgrid" (textgrid.h:
// enter, tick and the drawing calls, one after the other). The poll and the textgrid's calls skip their pass while the other task has the engine, so the
// menu never waits in them; the operations sleep until it is free. sync.enter / sync.leave guard only the flag; a base without the group (before 3.3)
// has no critical section to offer, and the flag is then tested and set unguarded. The ring's file verbs typed on the screen (dump, ringdump, ringsave,
// undump, ringload) do not run in monitor.key: it queues them and the poll runs them, a dump one step per pass (trxmon's ring.h, monitor.cc mon_serve).
static volatile bool s_engine;
// The textgrid's enter found the engine taken: the session opens at the next tick or render that gets it. Read and written on the menu task only.
static bool s_open_due;

static bool engine_try(void)
{
    const SvcSync *s = b33_sync();
    if (s) {
        s->enter();
    }
    bool got = !s_engine;
    s_engine = true;
    if (s) {
        s->leave();
    }
    return got;
}

static void engine_take(void)
{
    while (!engine_try()) {
        plat_sleep(PLAT_SLEEP_MIN_MS);
    }
}

static void engine_give(void) { s_engine = false; }

// ---- operations ------------------------------------------------------------------------------------------------------------------------------------------------
static void put_msg(OpOut &o, const char *s)
{
    sbuf_t b;
    sb_init(&b, o.msg, o.msglen);
    sb_put(&b, s);
    sb_str(&b);
}

// The Help node of the tab runs a command in the monitor.
static int op_run(CoreCtx *, const OpArgs &a, OpOut &o)
{
    engine_take();
    mon_run(a.s(0));
    engine_give();
    put_msg(o, a.s(0));
    return OPE_OK;
}
static const ArgSpec line_arg[] = { ARG_STRING("line", 60) };

// T192.3 leak 2: the monitor's keys are an operation, not a branch of the menu controller. The key table (menukeytab.cc) sends every key of the monitor's
// screen to it; the answer is what the controller does next: "goto F8" (the line `diag`: the System tab, F8; the session is released first), "close" (`x`:
// resume and leave), else nothing.
static int op_key(CoreCtx *, const OpArgs &a, OpOut &o)
{
    engine_take();
    mon_key(a.i(0));
    o.msg[0] = 0;
    if (mon_diag_requested()) {
        mon_release();
        put_msg(o, "goto F8");
    } else if (mon_leave_requested()) {
        put_msg(o, "close");
    }
    engine_give();
    return OPE_OK;
}
static const ArgSpec key_arg[] = { ARG_INT("key", 0, 0x1FFF) };

// The menu closed (any way): the base library's core hook menu_closed calls this, where MultiCore's run.cc called mon_release. It runs on the menu task
// without the core lock while the poll (when_frozen) may be in a pass: it waits for the engine like the other operations.
static int op_release(CoreCtx *, const OpArgs &, OpOut &o)
{
    o.msg[0] = 0;
    s_open_due = false;                             // the screen is gone: an open still due is not wanted any more
    engine_take();
    mon_release();
    engine_give();
    return OPE_OK;
}

// The library's poll (LibPollDecl): the RPC on TCP 4312, meant to answer whatever the menu does (MultiCore's main loop called mon_serve() on every pass),
// and the steps of a ring file verb the screen queued. It is declared when_frozen, so a base that honours the flag (3.3) runs it with the menu open too;
// a base before 3.3 (SDK 0.2.0's 3.2 too) does not read the flag and runs it only while the machine is not frozen, and a queued verb then waits until the
// menu closes, which drops it.
static int op_serve(CoreCtx *, const OpArgs &, OpOut &o)
{
    o.msg[0] = 0;
    if (engine_try()) {
        mon_serve();
        engine_give();
    }
    return OPE_OK;
}

static const char *st_wants_stop(CoreCtx *) { return mon_wants_stop() ? "yes" : 0; }

static const LibOpDecl s_ops[] = {
    { "mon.run", line_arg, 1, op_run },
    { "monitor.key", key_arg, 1, op_key },
    { "monitor.serve", 0, 0, op_serve },
    { "monitor.release", 0, 0, op_release },
};
static const LibStateDecl s_states[] = {
    { "monitor.wants_stop", st_wants_stop },
};

// ---- the export "textgrid" (major 1, textgrid.h): the F4 screen ----------------------------------------------------------------------------------------------
// Every call runs on the base's menu task, one after the other: enter and tick drive the session, render builds the rows from the engine and the other calls
// read what it built (s_scr of monitor.cc, which only render and tick write). None of them may block: while the poll has the engine, enter leaves the open
// due (s_open_due), tick reports no change and render builds nothing (the rows and sig stay those of the last build); the next pass of the menu loop tries
// again. The row attributes are trxmon's UA_* (ui.h), the same numbers as TG_*.
static void open_due(void)
{
    if (s_open_due) {
        s_open_due = false;
        mon_open();
    }
}

// The node was entered (what the operation monitor.open did): the session starts, once until mon_release().
static void tg_enter(void)
{
    s_open_due = true;
    if (engine_try()) {
        open_due();
        engine_give();
    }
}

// A pass of the menu loop while the screen is shown (what the operation monitor.tick did): stops, finished run-tos, the live log. True: redraw.
static bool tg_tick(void)
{
    if (!engine_try()) {
        return false;
    }
    bool opened = s_open_due;
    open_due();
    bool d = mon_tick() || opened;
    engine_give();
    return d;
}

static void tg_render(void)
{
    if (engine_try()) {
        open_due();
        mon_render();
        engine_give();
    }
}

static const TextGrid s_grid = {
    sizeof(TextGrid), tg_render, mon_cols, mon_rows, mon_line, mon_attr, mon_blank_rows, mon_cursor, mon_sig, tg_enter, tg_tick,
};

// The library's named exports (base 3.2, LibRecord::export_list); the Makefile's LIB_EXPORTS lists the same names and majors, and the loader refuses the
// library when lib_register does not return each of them.
static const LibExportDecl s_exports[] = {
    { "textgrid", 1, &s_grid, sizeof(s_grid) },
};

// lib.ld puts this section first, so the entry is at offset 0 of the image
#ifdef __riscv
#define LIB_ENTRY_ATTR __attribute__((visibility("default"), section(".text.lib_register")))
#else
#define LIB_ENTRY_ATTR
#endif
extern "C" LIB_ENTRY_ATTR int lib_register(const LibApi *api, LibRecord *out)
{
    if (api->abi_major != U2A_ABI_MAJOR || !api->sys || !api->mem) {
        return 1;
    }
    g_api = api;
    out->ops = s_ops;
    out->nops = (int)(sizeof(s_ops) / sizeof(s_ops[0]));
    out->states = s_states;
    out->nstates = (int)(sizeof(s_states) / sizeof(s_states[0]));
    out->poll.op = "monitor.serve";
    out->poll.period_ms = 5;
    out->poll.when_frozen = true;                   // the RPC answers while the menu holds the machine (base 3.3)
    out->export_list = s_exports;
    out->nexport_list = (int)(sizeof(s_exports) / sizeof(s_exports[0]));
    return 0;
}
