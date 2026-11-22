// Timewarp as operations and state values (T104.2 UI, Alex 2026-10-28: everything declarative). The engine is ring.cc (TRX64-Ultimate's frame ring driver); this file is what
// the menu says to it: start and stop, scrub a frame or a second, back to live, mark, resume here, write and load a .c64re. menu.toml F2 draws it (screens.cc layout
// "timeline"), the keys of that layout call these operations (menuctl.cc).
// U18 (the ring library on the SDK): the registrations are the static tables of ringops_record() (lib_ring.cc hands them out), settings are read
// through api->ops->state("set.<Key>"), the dump's task is a job (api->job), the menu's write protocol and the target path are the ctx group's; ring.start
// starts the engine (the core's start hook calls it), ring.poll is the library's poll and ring.unfreeze the menu's rewind at its close (the core hook
// menu_closed calls it); the last two were calls of the core application's main loop. The machine a file describes is ringmachine.cc's, for these
// operations and the exported table alike.
#include "ringops.h"
#include "trx_ring.h"
#include "trx_c64re.h"
#include "trx_trxring.h"
#include "ringmachine.h"
#include "ringsys.h"
#include <string.h>

// A setting's value as the index of its label (api->ops->state("set.<Key>"): the label of the value, spaces in the key written as '_'); def when the core
// has no such setting or the label is not one of these.
static int cfg_index(const char *state, const char *const *labels, int n, int def)
{
    const char *v = g_api->ops ? g_api->ops->state(state, 0) : 0;
    for (int i = 0; v && i < n; i++) {
        if (!strcmp(v, labels[i])) {
            return i;
        }
    }
    return def;
}
// the label list of base/menu.toml: onoff
static const char *const l_onoff[] = { "Off", "On" };
#define NLAB(l) int(sizeof(l) / sizeof((l)[0]))

static int g_glyph;                             // the bar's glyph after the last move: 0 pause, 1 back, 2 forward, 3 oldest, 4 newest
static int g_sel = -1;                          // the timeline entry the paused machine shows; -1 the live end
#define MARKS 16
static uint32_t g_mark[MARKS];                  // the seq of a marked frame
static int g_nmark;

static int newest(void) { return trx_ring_count() - 1; }
static int shown(void) { return g_sel < 0 ? newest() : g_sel; }

static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

static int go_to(int idx, char *msg, int msglen)
{
    int n = trx_ring_count();
    if (n <= 0) {
        r_snprintf(msg, msglen, "The ring holds no frame yet");
        return OPE_FAILED;
    }
    idx = clampi(idx, 0, n - 1);
    int dir = idx >= shown() ? 1 : -1;
    int k = idx;
    while (k >= 0 && k < n && !trx_ring_reachable(k)) {          // the nearest frame a restore can go to, in the direction asked
        k += dir;
    }
    if (k < 0 || k >= n) {
        k = idx;
        while (k >= 0 && k < n && !trx_ring_reachable(k)) {
            k -= dir;
        }
        if (k < 0 || k >= n) {
            r_snprintf(msg, msglen, "No frame the ring can go back to");
            return OPE_FAILED;
        }
    }
    TrxRestoreStats st;
    int err = trx_ring_scrub_show(k, &st);
    if (err) {
        r_snprintf(msg, msglen, "%s", trx_ring_error(err));
        return OPE_FAILED;
    }
    g_glyph = k == shown() ? g_glyph : (dir < 0 ? 1 : 2);
    if (k >= n - 1) {
        g_glyph = 4;
    } else if (k == 0) {
        g_glyph = 3;
    }
    g_sel = k;
    trx_ring_set_pending(k);
    return OPE_OK;
}

static int op_step(CoreCtx *, const OpArgs &a, OpOut &o) { return go_to(shown() + a.i(0), o.msg, o.msglen); }
static int op_seconds(CoreCtx *, const OpArgs &a, OpOut &o) { return go_to(shown() + a.i(0) * trx_ring_fps(), o.msg, o.msglen); }

static int op_oldest(CoreCtx *, const OpArgs &, OpOut &o) { return go_to(0, o.msg, o.msglen); }

static int op_live(CoreCtx *, const OpArgs &, OpOut &o)
{
    if (trx_ring_count() <= 0) {
        r_snprintf(o.msg, o.msglen, "The ring holds no frame yet");
        return OPE_FAILED;
    }
    if (g_sel < 0) {
        return OPE_OK;
    }
    int rc = go_to(newest(), o.msg, o.msglen);
    g_sel = -1;
    g_glyph = 4;
    trx_ring_set_pending(-1);                                      // the unfreeze goes on from the scrub's state to the newest frame
    return rc;
}

// RUN/STOP and the left arrow: the menu closes and the machine goes on from the newest frame (a frame shown is not a place to run on from)
static int op_cancel(CoreCtx *c, const OpArgs &a, OpOut &o)
{
    if (g_sel >= 0) {
        op_live(c, a, o);
    }
    g_glyph = 0;
    return OPE_OK;
}

static int op_resume(CoreCtx *, const OpArgs &, OpOut &o)
{
    if (g_sel >= 0) {
        trx_ring_set_pending(g_sel);
        r_snprintf(o.msg, o.msglen, "Running on from %s", "the shown frame");
    } else {
        r_snprintf(o.msg, o.msglen, "Live");
    }
    return OPE_OK;
}

// C= O on the Timewarp bar: into the monitor, on the frame shown (FW io/c64/trx_timewarp_ui.cc:284-289, 341-344): the monitor puts the machine on that entry itself
// (trx_ring_land) and holds it there; no rewind is made pending, so the timeline is not cut. The menu goes on to the monitor's screen (menu.screen's "goto").
static int op_monitor(CoreCtx *, const OpArgs &, OpOut &o)
{
    if (trx_ring_count() <= 0) {
        r_snprintf(o.msg, o.msglen, "The ring holds no frame yet");
        return OPE_FAILED;
    }
    int err = trx_ring_land(shown(), NULL);
    if (err) {
        r_snprintf(o.msg, o.msglen, "%s", trx_ring_error(err));
        return OPE_FAILED;
    }
    g_sel = -1;
    r_snprintf(o.msg, o.msglen, "goto F1/monitor");
    return OPE_OK;
}

// ring.start: the log starts now (the timeline is emptied).
static int op_record(CoreCtx *, const OpArgs &, OpOut &o)
{
    if (!trx_ring_start()) {
        const char *why = trx_ring_reu_refusal();
        r_snprintf(o.msg, o.msglen, "%s", why ? why : "This bitstream has no frame ring");
        return OPE_FAILED;
    }
    g_sel = -1;
    g_nmark = 0;
    r_snprintf(o.msg, o.msglen, "Timewarp is recording");
    return OPE_OK;
}

static int op_stop(CoreCtx *, const OpArgs &, OpOut &o)
{
    trx_ring_stop();
    r_snprintf(o.msg, o.msglen, "Timewarp stopped: the frames stay until it starts again");
    return OPE_OK;
}

static int op_mark(CoreCtx *, const OpArgs &, OpOut &o)
{
    TrxFrame f;
    if (!trx_ring_frame(shown(), &f)) {
        r_snprintf(o.msg, o.msglen, "No frame to mark");
        return OPE_FAILED;
    }
    if (g_nmark >= MARKS) {
        for (int i = 1; i < MARKS; i++) {
            g_mark[i - 1] = g_mark[i];
        }
        g_nmark = MARKS - 1;
    }
    g_mark[g_nmark++] = f.seq;
    r_snprintf(o.msg, o.msglen, "Marker %d set at frame %u", g_nmark, (unsigned)f.marker);
    return OPE_OK;
}

static int op_next_mark(CoreCtx *, const OpArgs &a, OpOut &o)       // value +1: the next marker towards live, -1: the one before
{
    int here = shown(), best = -1;
    for (int i = 0; i < g_nmark; i++) {
        int ix = trx_ring_find(g_mark[i]);
        if (ix < 0) {
            continue;
        }
        if (a.i(0) > 0 && ix > here && (best < 0 || ix < best)) {
            best = ix;
        }
        if (a.i(0) < 0 && ix < here && ix > best) {
            best = ix;
        }
    }
    if (best < 0) {
        r_snprintf(o.msg, o.msglen, "No marker that way");
        return OPE_FAILED;
    }
    return go_to(best, o.msg, o.msglen);
}

static int op_clear_marks(CoreCtx *, const OpArgs &, OpOut &o)
{
    g_nmark = 0;
    r_snprintf(o.msg, o.msglen, "Markers cleared");
    return OPE_OK;
}

// ---- the menu's write protocol and target path (base 3.3 ctx group; without it every write runs on the caller and nothing is placed) ----------------
static bool job_step(void)
{
    const SvcCtx *c = b33_ctx();
    return c && c->job_step();
}

static bool job_cancelled(void)
{
    const SvcCtx *c = b33_ctx();
    return c && c->job_cancelled();
}

static void job_progress(int pct, const char *path)
{
    if (const SvcCtx *c = b33_ctx()) {
        c->job_progress(pct, path);
    }
}

static bool target_path(const char *name, const char *ext, char *out, int n)
{
    const SvcCtx *c = b33_ctx();
    return c && c->target_path(name, ext, out, n);
}

// FW shows the progress box while trx_c64re_dump_ex writes (trx_timewarp_ui.cc:361-376); the menu draws a progress only for an operation that answers `more` and is called
// again (ctx.job_step, newdisk.cc is the pattern), so with job_step the write runs on a task of its own and the operation answers `more` until it is done, reporting
// the share the task has written (ctx.job_progress, from the handler: the ctx group's rule). Without job_step (REST, a hotkey) the write runs on the caller. The task is
// a job of the library (api->job); without a job it writes on the caller. The machine the file describes is built on the handler (ringmachine.cc) and goes to the task.
struct DumpJob {
    volatile int state;                         // 0 idle, 1 the task writes, 2 it is done (result in why / bytes / entries)
    volatile int pct;                           // the share written, set by the task
    bool ring;                                  // .c64rering (the last seconds) instead of one frame
    int idx;
    int secs;
    char path[260];
    uint32_t bytes;
    int entries;
    const char *why;
    RingMachine mach;
    SvcJobH *job;
};
static DumpJob g_dj;

static void dump_progress(int done, int total, void *)
{
    int pct = total ? done * 100 / total : 0;
    g_dj.pct = pct;
    if (total && (done == 0 || done == total || (done * 10 / total) != ((done - 1) * 10 / total))) {
        r_log("[C64RE] dump %d%s\n", pct, "%");
    }
}

// the firmware's task had 12 * configMINIMAL_STACK_SIZE words (1600 each); SvcJob::start takes bytes (the base gives at least 12 KB)
#define DUMP_STACK (12 * 1600 * 4)

// The source goes with both kinds: a .c64rering entry names the cartridge and drive A as a .c64re does (FW write_checkpoint).
static void dump_run(DumpJob *j)
{
    trx_c64re_set_source(&j->mach.src);
    if (j->ring) {
        j->why = trx_c64rering_dump_ex(j->path, j->secs, 5, &j->bytes, &j->entries, dump_progress, NULL);
    } else {
        j->why = trx_c64re_dump_ex(j->path, j->idx, &j->bytes, dump_progress, NULL);
    }
    trx_c64re_set_source(NULL);
}

static int dump_task(SvcJobH *, void *arg)
{
    DumpJob *j = (DumpJob *)arg;
    dump_run(j);
    __asm__ volatile ("fence" ::: "memory");
    j->state = 2;
    return 0;
}

static int dump_op(const OpArgs &a, OpOut &o, bool ring)
{
    DumpJob *j = &g_dj;
    o.more = false;
    if (j->state == 1 && !job_step()) {
        r_snprintf(o.msg, o.msglen, "A save is still being written");
        return OPE_FAILED;
    }
    if (j->state == 0) {
        if (!target_path(a.s(0), ring ? ".c64rering" : ".c64re", j->path, sizeof(j->path))) {
            r_snprintf(o.msg, o.msglen, "No medium to save to");
            return OPE_FAILED;
        }
        j->ring = ring;
        j->idx = shown();
        j->secs = 1;
        j->bytes = 0;
        j->entries = 0;
        j->pct = 0;
        j->why = NULL;
        if (ring) {
            int fps = trx_ring_fps() ? trx_ring_fps() : 50;
            j->secs = trx_ring_count() / fps;
            j->secs = j->secs < 1 ? 1 : (j->secs > 30 ? 30 : j->secs);
        }
        // T228 R20/R21: what the file says of the machine (the System Mode, the cartridge image as it stands, the GMod2's EEPROM, drive A and its image)
        ring_machine_build(&j->mach, false);
        if (job_step() && g_api->job) {
            job_progress(0, j->path);
            j->state = 1;
            j->job = g_api->job->start(dump_task, j, DUMP_STACK);
            if (j->job) {
                o.more = true;
                return OPE_OK;
            }
            j->state = 0;                               // no task: write on the caller, without the bar
        }
        dump_run(j);
        j->state = 2;
    }
    if (j->state == 1) {                                // the task writes: the share it has written goes to the menu's progress
        job_progress(j->pct, NULL);
        o.more = true;
        return OPE_OK;
    }
    j->state = 0;
    if (j->job) {
        g_api->job->join(j->job);
        j->job = 0;
    }
    job_progress(0, "");
    if (job_cancelled()) {                              // FW's write cannot be stopped half way: it ran to its end, the file goes
        if (!j->why) {
            rf_remove(j->path);
        }
        r_snprintf(o.msg, o.msglen, "Cancelled: nothing was written.");
        return OPE_OK;
    }
    if (j->why) {
        r_snprintf(o.msg, o.msglen, "%s", j->why);
        return OPE_FAILED;
    }
    if (ring) {
        r_snprintf(o.msg, o.msglen, "Saved %s: %d entries, %u KB", j->path, j->entries, (unsigned)((j->bytes + 1023) >> 10));
    } else {
        r_snprintf(o.msg, o.msglen, "Saved %s (%u KB)", j->path, (unsigned)((j->bytes + 1023) >> 10));
    }
    return OPE_OK;
}

static int op_dump(CoreCtx *, const OpArgs &a, OpOut &o) { return dump_op(a, o, false); }

// "Save .C64RE Dump" (FW apps/c64re/manifest.json, category "C64 Machine"): the machine as it stands, no ring needed; the media that are in it go inside
static int op_c64re_dump(CoreCtx *, const OpArgs &a, OpOut &o)
{
    char p[260];
    if (!target_path(a.s(0), ".c64re", p, sizeof(p))) {
        r_snprintf(o.msg, o.msglen, "No medium to save to");
        return OPE_FAILED;
    }
    uint32_t bytes = 0;
    const char *why = trx_c64re_dump_machine(p, &bytes);
    if (why) {
        r_snprintf(o.msg, o.msglen, "%s", why);
        return OPE_FAILED;
    }
    r_snprintf(o.msg, o.msglen, "Saved %s (%u KB)", p, (unsigned)((bytes + 1023) >> 10));
    return OPE_OK;
}

static const char *st_suggest_c64re(CoreCtx *) { return "c64re"; }

static int op_ringdump(CoreCtx *, const OpArgs &a, OpOut &o) { return dump_op(a, o, true); }

// The state goes in at once: MultiCore's app puts no machine state back when the menu closes (the menu draws on its own layer), so the menu's close does not
// overwrite it (MultiCore's decision of 2026-11-22). The machine the file needs (System Mode, cartridge, drive A) is built by the load (ringmachine.cc).
static int op_load(CoreCtx *, const OpArgs &a, OpOut &o)
{
    const char *why = trx_c64re_load(a.s(0));
    if (why) {
        r_snprintf(o.msg, o.msglen, "%s", why);
        return OPE_FAILED;
    }
    g_sel = -1;
    g_nmark = 0;
    if (trx_c64re_missed()) {
        r_snprintf(o.msg, o.msglen, "State loaded from %s; not put back: %s", a.s(0), trx_c64re_missed());
    } else {
        r_snprintf(o.msg, o.msglen, "State loaded from %s", a.s(0));
    }
    return OPE_OK;
}

// the setting: Timewarp Ring Off / On, Instruction Trace Off / On
static int fx_ring(CoreCtx *, const OpArgs &, OpOut &)
{
    ring_autostart = cfg_index("set.Timewarp_Ring", l_onoff, NLAB(l_onoff), 1) != 0;
    TrxRingStatus st;
    trx_ring_status(&st);
    if (st.present) {
        if (ring_autostart && !st.on && trx_ring_engine_started()) {
            trx_ring_start();
        } else if (!ring_autostart && st.on) {
            trx_ring_stop();
        }
        trx_ring_trace_ctl(cfg_index("set.Instruction_Trace", l_onoff, NLAB(l_onoff), 0) ? 1 : 0);
    }
    return OPE_OK;
}

// ---- states ---------------------------------------------------------------------------------------------------------------------------------------------
static char b1[64], b2[64], b3[64], b4[24], b5[64];

// A08: the Timewarp page is refused while the REU is on (FW io/c64/trx_timewarp_ui.cc:323-326, the popup text of io/c64/trx_ring_reu.h:10); menu.screen's `unless`
static const char *st_reu_off(CoreCtx *) { return trx_ring_reu_refusal() ? "Timewarp is off while REU is on" : 0; }

static const char *st_chip(CoreCtx *)
{
    TrxRingStatus st;
    trx_ring_status(&st);
    return !st.present ? "N/A" : (st.reu_off ? "Off: REU on" : (trx_ring_scrubbing() ? "Scrubbing" : (st.on ? "Recording" : "Stopped")));
}
static const char *st_ring(CoreCtx *)
{
    TrxRingStatus st;
    trx_ring_status(&st);
    if (!st.present) {
        return "No frame ring in this bitstream";
    }
    if (st.reu_off) {
        return "Off while the REU is enabled";
    }
    int fps = trx_ring_fps() ? trx_ring_fps() : 50;
    r_snprintf(b1, sizeof(b1), "%s, %d.%d s", st.on ? "Recording" : "Stopped", st.frames / fps, (st.frames % fps) * 10 / fps);
    return b1;
}
static const char *st_label(CoreCtx *)
{
    int n = trx_ring_count();
    if (n <= 0) {
        return "no frames yet";
    }
    int fps = trx_ring_fps() ? trx_ring_fps() : 50;
    int back = newest() - shown();
    if (back <= 0 && g_sel < 0) {
        return "LIVE";
    }
    r_snprintf(b2, sizeof(b2), "-%d.%d s", back / fps, (back % fps) * 10 / fps);
    return b2;
}
static const char *st_left(CoreCtx *)
{
    int n = trx_ring_count();
    int fps = trx_ring_fps() ? trx_ring_fps() : 50;
    r_snprintf(b3, sizeof(b3), "-%d.%d s", n / fps, (n % fps) * 10 / fps);
    return b3;
}
static const char *st_frame(CoreCtx *)
{
    TrxFrame f;
    if (!trx_ring_frame(shown(), &f)) {
        return "FRAME -";
    }
    r_snprintf(b5, sizeof(b5), "FRAME %u", (unsigned)f.marker);
    return b5;
}
static const char *st_bar(CoreCtx *)
{
    int n = trx_ring_count();
    r_snprintf(b4, sizeof(b4), "%d", n > 1 ? shown() * 100 / (n - 1) : 100);
    return b4;
}
// the bar of the pixel layer (trx_timewarp_ui.cc draw_bar): the position large, the frame small, the span of the ring as a track
static const char *st_pos(CoreCtx *)
{
    int n = trx_ring_count();
    if (g_sel < 0 || n <= 0) {
        return "now";
    }
    int fps = trx_ring_fps() ? trx_ring_fps() : 50;
    int f = n - 1 - g_sel;
    r_snprintf(b4, sizeof(b4), "-%d.%d s", f / fps, (f % fps) * 10 / fps);
    return b4;
}
static const char *st_info(CoreCtx *)
{
    int n = trx_ring_count();
    if (n < 2) {
        return "the ring holds no frame yet";
    }
    r_snprintf(b1, sizeof(b1), "frame %d / %d", shown() + 1, n);
    return b1;
}
static const char *st_glyph(CoreCtx *)
{
    static char g[4];
    r_snprintf(g, sizeof(g), "%d", g_glyph);
    return g;
}
static const char *st_track(CoreCtx *)
{
    int n = trx_ring_count();
    r_snprintf(b5, sizeof(b5), "%d/%d", n > 0 ? shown() : 0, n);
    return b5;
}
static const char *st_marks(CoreCtx *)
{
    static char m[32];
    r_snprintf(m, sizeof(m), "%d marker%s", g_nmark, g_nmark == 1 ? "" : "s");
    return g_nmark ? m : "none";
}
static const char *st_present(CoreCtx *)
{
    TrxRingStatus st;
    trx_ring_status(&st);
    return st.present ? "yes" : 0;
}
static const char *st_running(CoreCtx *)
{
    TrxRingStatus st;
    trx_ring_status(&st);
    return st.present && st.on ? "yes" : 0;
}
static const char *st_stopped(CoreCtx *)
{
    TrxRingStatus st;
    trx_ring_status(&st);
    return st.present && !st.on ? "yes" : 0;
}

static const ArgSpec value_arg[] = { ARG_INT("value", -100000, 100000) };
static const ArgSpec name_arg[] = { ARG_STRING("name", 55) };
static const ArgSpec path_arg[] = { ARG_PATH("path") };

// ---- the engine's start, the library's poll and the menu's close (U18: calls of the core application's main loop before) ------------------------
// ring.init: the core's start hook (core_hooks.h `start`, in the base library) calls it once the api's groups exist; lib_register cannot (they are null
// there). The engine starts on the first call only; later calls answer the same and change nothing.
static int op_start(CoreCtx *, const OpArgs &, OpOut &o)
{
    const char *why = trx_ring_engine_init();
    if (why) {
        r_snprintf(o.msg, o.msglen, "%s", why);
        return OPE_FAILED;
    }
    return OPE_OK;
}

// ring.poll: LibRecord::poll (lib_ring.cc). The setting is read until the log's delayed start, which takes it (run.cc did it before the engine started).
static int op_poll(CoreCtx *, const OpArgs &, OpOut &)
{
    if (!trx_ring_engine_started()) {
        ring_autostart = cfg_index("set.Timewarp_Ring", l_onoff, NLAB(l_onoff), 1) != 0;
    }
    trx_ring_engine_poll();
    return OPE_OK;
}

// ring.unfreeze: the menu closes; the core hook menu_closed (base library) calls it on the menu task with the machine still stopped and without the core
// lock: the frame chosen in the Timewarp bar is where the machine runs on. A state file made pending (trx_c64re_set_pending, trx_trxring_set_pending) goes
// in here as well.
static int op_unfreeze(CoreCtx *, const OpArgs &, OpOut &)
{
    if (trx_ring_pending()) {
        trx_ring_unfreeze();
    }
    if (trx_c64re_pending()) {
        trx_c64re_unfreeze();
    }
    if (trx_trxring_pending()) {
        trx_trxring_unfreeze();
    }
    return OPE_OK;
}

// The registrations, in the order ringops_register() made them (contributor "timewarp"), plus ring.init (the engine), ring.poll and
// ring.unfreeze.
static const LibOpDecl ops[] = {
    { "tw.step", value_arg, 1, op_step },
    { "tw.seconds", value_arg, 1, op_seconds },
    { "tw.live", 0, 0, op_live },
    { "tw.oldest", 0, 0, op_oldest },
    { "tw.resume", 0, 0, op_resume },
    { "tw.cancel", 0, 0, op_cancel },
    { "tw.monitor", 0, 0, op_monitor },
    { "tw.mark", 0, 0, op_mark },
    { "tw.next_mark", value_arg, 1, op_next_mark },
    { "tw.clear_marks", 0, 0, op_clear_marks },
    { "tw.dump", name_arg, 1, op_dump },
    { "tw.ringdump", name_arg, 1, op_ringdump },
    { "machine.save_c64re", name_arg, 1, op_c64re_dump },
    { "tw.load", path_arg, 1, op_load },
    { "ring.start", 0, 0, op_record },
    { "ring.stop", 0, 0, op_stop },
    { "ring.fx", value_arg, 1, fx_ring },
    { "ring.init", 0, 0, op_start },
    { "ring.poll", 0, 0, op_poll },
    { "ring.unfreeze", 0, 0, op_unfreeze },
};
static const LibStateDecl states[] = {
    { "machine.suggest.c64re", st_suggest_c64re },
    { "ring.reu_off", st_reu_off },
    { "tw.chip", st_chip },
    { "tw.ring", st_ring },
    { "tw.label", st_label },
    { "tw.left", st_left },
    { "tw.frame", st_frame },
    { "tw.bar", st_bar },
    { "tw.marks", st_marks },
    { "tw.pos", st_pos },
    { "tw.info", st_info },
    { "tw.glyph", st_glyph },
    { "tw.track", st_track },
    { "tw.present", st_present },
    { "tw.running", st_running },
    { "tw.stopped", st_stopped },
};

void ringops_record(LibRecord *out)
{
    out->ops = ops;
    out->nops = int(sizeof(ops) / sizeof(ops[0]));
    out->states = states;
    out->nstates = int(sizeof(states) / sizeof(states[0]));
}
