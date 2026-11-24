// Timewarp as operations and state values (T104.2 UI, Alex 2026-10-28: everything declarative). The engine is ring.cc (TRX64-Ultimate's frame ring driver); this file is what
// the menu says to it: start and stop, scrub a frame or a second, back to live, mark, resume here, write and load a .c64re. menu.toml's page F1/timewarp is the base's
// layout "timeline" (screens.cc build_timeline, which draws the bar from the states tw.pos, tw.info, tw.glyph, tw.track), its [[key]] rows call these operations.
// U18 (the ring library on the SDK): the registrations are the static tables of ringops_record() (lib_ring.cc hands them out), settings are read
// through api->ops->state("set.<Key>"), the dumps are operations the menu steps (trx_dump_*), the menu's write protocol and the target path are the ctx
// group's; ring.init starts the engine (the core's start hook calls it), ring.poll is the library's poll and ring.unfreeze the menu's rewind at its close
// (the core hook menu_closed calls it); the last two were calls of the core application's main loop. The machine a file describes is ringmachine.cc's,
// for these operations and the exported table alike.
#include "ringops.h"
#include "trx_ring.h"
#include "trx_c64re.h"
#include "trx_trxring.h"
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
    // The scrub's run writes a frame into the log areas, and ring.cc's trim() then drops the oldest entries it made unreachable, also when the run
    // fails: every index moves down by that many. The frames are found again by their seq (the firmware's Timewarp counts from the newest for the
    // same reason).
    bool same = k == shown();
    TrxFrame f, held;
    bool known = trx_ring_frame(k, &f);
    bool was = g_sel >= 0 && trx_ring_frame(g_sel, &held);
    TrxRestoreStats st;
    int err = trx_ring_scrub_show(k, &st);
    if (err) {
        int i = was ? trx_ring_find(held.seq) : -1;
        if (i >= 0) {
            g_sel = i;
            trx_ring_set_pending(i);
        }
        r_snprintf(msg, msglen, "%s", trx_ring_error(err));
        return OPE_FAILED;
    }
    int moved = known ? trx_ring_find(f.seq) : -1;
    if (moved >= 0) {
        k = moved;
    }
    n = trx_ring_count();
    g_glyph = same ? g_glyph : (dir < 0 ? 1 : 2);
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
    const SvcCtx *c = g_api->ctx;
    return c && c->job_step();
}

static bool job_cancelled(void)
{
    const SvcCtx *c = g_api->ctx;
    return c && c->job_cancelled();
}

static void job_progress(int pct, const char *path)
{
    if (const SvcCtx *c = g_api->ctx) {
        c->job_progress(pct, path);
    }
}

static bool target_path(const char *name, const char *ext, char *out, int n)
{
    const SvcCtx *c = g_api->ctx;
    return c && c->target_path(name, ext, out, n);
}

// ---- the dumps (trx_c64re.h trx_dump_*) -------------------------------------------------------------------------------------------------------------
// FW showed a progress box while its write ran on a task (trx_timewarp_ui.cc:361-376). Here the menu steps the operation (the node attribute `job`,
// ctx.job_step): each call does one step of the dump under the core lock and answers `more` until the file is complete; the menu stays open meanwhile, so
// the machine stays frozen between the steps. A call the menu does not step (REST, a hotkey, a task) does every step in that one call. The stepped dump
// lives here between two calls of its operation; one at a time, and only calls on the menu task (the stepped ones, ring.unfreeze) touch it.
static TrxDump *g_dump;
static int g_dump_kind;
static char g_dump_name[56];                    // the name it was started with (name_arg: 55 characters)

// Drops the stepped dump, removing its file when that is not complete.
static void dump_drop(void)
{
    trx_dump_drop(g_dump);
    g_dump = NULL;
}

static int dump_said(const TrxDump *d, int kind, int rc, const char *why, OpOut &o)
{
    if (rc < 0) {
        r_snprintf(o.msg, o.msglen, "%s", why ? why : "The file could not be written");
        return OPE_FAILED;
    }
    unsigned kb = (unsigned)((trx_dump_bytes(d) + 1023) >> 10);
    if (kind == TRX_DUMP_RING) {
        r_snprintf(o.msg, o.msglen, "Saved %s: %d entries, %u KB", trx_dump_path(d), trx_dump_entries(d), kb);
    } else {
        r_snprintf(o.msg, o.msglen, "Saved %s (%u KB)", trx_dump_path(d), kb);
    }
    return OPE_OK;
}

static int dump_op(const OpArgs &a, OpOut &o, int kind, const char *ext)
{
    o.more = false;
    const bool step = job_step();
    const char *why = NULL;
    if (step && g_dump && g_dump_kind == kind && !strcmp(g_dump_name, a.s(0))) {
        if (job_cancelled()) {
            dump_drop();
            r_snprintf(o.msg, o.msglen, "Cancelled: nothing was written.");
            return OPE_OK;
        }
        int rc = trx_dump_step(g_dump, &why);
        if (rc > 0) {
            job_progress(trx_dump_pct(g_dump), NULL);
            o.more = true;
            return OPE_OK;
        }
        int res = dump_said(g_dump, kind, rc, why, o);
        dump_drop();
        return res;
    }
    if (step) {
        dump_drop();                                // one the menu stopped stepping
    }
    char path[260];
    if (!target_path(a.s(0), ext, path, sizeof(path))) {
        r_snprintf(o.msg, o.msglen, "No medium to save to");
        return OPE_FAILED;
    }
    int arg = 0;
    if (kind == TRX_DUMP_FRAME) {
        arg = shown();
    } else if (kind == TRX_DUMP_RING) {
        int fps = trx_ring_fps() ? trx_ring_fps() : 50;
        arg = clampi(trx_ring_count() / fps, 1, 30);
    }
    TrxDump *d = trx_dump_begin(kind, path, arg, 5, &why);
    if (!d) {
        r_snprintf(o.msg, o.msglen, "%s", why);
        return OPE_FAILED;
    }
    if (step) {                                     // this call set it up; the menu's next calls do the steps
        g_dump = d;
        g_dump_kind = kind;
        r_snprintf(g_dump_name, sizeof(g_dump_name), "%s", a.s(0));
        job_progress(0, path);
        o.more = true;
        return OPE_OK;
    }
    int rc;
    while ((rc = trx_dump_step(d, &why)) > 0) {
    }
    int res = dump_said(d, kind, rc, why, o);
    trx_dump_drop(d);
    return res;
}

static int op_dump(CoreCtx *, const OpArgs &a, OpOut &o) { return dump_op(a, o, TRX_DUMP_FRAME, ".c64re"); }

// "Save .C64RE Dump" (FW apps/c64re/manifest.json, category "C64 Machine"): the machine as it stands, no ring needed; the media that are in it go inside
static int op_c64re_dump(CoreCtx *, const OpArgs &a, OpOut &o) { return dump_op(a, o, TRX_DUMP_MACHINE, ".c64re"); }

static const char *st_suggest_c64re(CoreCtx *) { return "c64re"; }

static int op_ringdump(CoreCtx *, const OpArgs &a, OpOut &o) { return dump_op(a, o, TRX_DUMP_RING, ".c64rering"); }

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
// in here as well. A dump the menu was still stepping is dropped with its partial file: the machine runs on from here, and its next step would describe
// another machine (no register is touched by the drop).
static int op_unfreeze(CoreCtx *, const OpArgs &, OpOut &)
{
    dump_drop();
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
