// The monitor of F4: trxmon's engine in the monitor library (monitor.h). PROVENANCE: monitor.cc of MultiCore's core application (TRX64U-MultiCore 3b8e2955,
// coreapp/src); the session is ui_session() of trxmon's main.c (ui_set_size, ui_set_keys, ui_open, ui_map_key, ui_key, ui_tick, ui_render) without the
// overlay; the RPC server is monsrv.cc, the platform calls are lib_monitor.cc.
// This file reads the firmware's application ABI (u2p_app.h, app_api.h), as trxmon does, and no header of the SDK: svctab.h defines u2p_video_t and the ring
// records again. The base's tables reach it through monlib.h.
#include "monitor.h"
#include "monlib.h"
#include "monsrv.h"
#include "ring_steps.h"
#include <stddef.h>
#include <string.h>

extern "C" {
#include "u2p_app.h"
#include "trxmon/hw.h"
#include "trxmon/mach.h"
#include "trxmon/mon.h"
#include "trxmon/ring.h"
#include "trxmon/rpc.h"
#include "trxmon/ui.h"
}

static_assert(sizeof(u2p_video_t) == 20, "u2p_video_t: the layout lib_monitor.cc hands over from svctab.h");
static_assert(int(RING_JOB_FRAME) == int(RING_STEPS_FRAME) && int(RING_JOB_RING) == int(RING_STEPS_RING),
              "trxmon's kinds of a dump in steps are the ring library's");

// ---- trxmon's trx table ------------------------------------------------------------------------------------------------------------------------------------
// What the firmware's app loader gave trxmon (app_api.h u2p_trx_api_t), made of the base's trx table (api->trx) and the ring library's table. detach,
// stop_requested and ui_request stay null: the library has no task of its own and no overlay, and their callers in trxmon's main.c are not built here.
static u2p_trx_api_t s_trx;

static int trx_video(u2p_video_t *v) { return monlib_trx_video(v); }

// The ring library's dumps in steps (ring_steps.h), right after its ring table at the offset that table's size states, handed to trxmon (ring.h): the
// screen's file verbs then run from the poll (mon_serve), one step per pass under the core lock. n: the bytes of the export.
static ring_steps_t s_steps;

static void steps_table(const u2p_ring_api_t *r, uint32_t n)
{
    uint32_t at = r->size;
    if ((at & 3) || n < at + sizeof(uint32_t)) {
        return;
    }
    const RingSteps *x = (const RingSteps *)((const char *)r + at);
    if (x->size < offsetof(RingSteps, drop) + sizeof(x->drop) || n - at < x->size) {
        return;
    }
    s_steps.begin = x->begin;
    s_steps.step = x->step;
    s_steps.pct = x->pct;
    s_steps.bytes = x->bytes;
    s_steps.entries = x->entries;
    s_steps.drop = x->drop;
    ring_set_steps(&s_steps);
}

static void trx_table(void)
{
    memset(&s_trx, 0, sizeof(s_trx));
    ring_set_steps(0);
    s_trx.size = sizeof(s_trx);
    MonTrxBase b;
    if (!monlib_trx(&b)) {
        return;                                 // regs stay null: hw_init() refuses
    }
    s_trx.regs = b.regs;
    s_trx.caps = b.caps;
    s_trx.acquire = b.acquire;
    s_trx.release = b.release;
    s_trx.announce = b.announce;
    s_trx.video = trx_video;
    // The ring as trxmon's hw_trx.c reads it: null without the engine. The table must reach `load`: hw_trx.c calls status ... load without asking for them.
    uint32_t n = 0;
    const u2p_ring_api_t *r = (const u2p_ring_api_t *)monlib_ring(&n);
    if (r && n >= offsetof(u2p_ring_api_t, load) + sizeof(r->load) && U2P_HAS(r, load) && r->status) {
        u2p_ring_status_t st;
        memset(&st, 0, sizeof(st));
        r->status(&st);
        if (st.present) {
            s_trx.ring = r;
            steps_table(r, n);
        }
    }
}

// ---- the session ---------------------------------------------------------------------------------------------------------------------------------------------
static ui_t s_ui;
static ui_screen_t s_scr;
static bool g_open;                             // a session stands (the engine is initialised, the debug block may be owned)
static bool g_ok;                               // the debug block is there
static bool g_leave, g_diag;
static bool g_menu;                             // the menu's monitor tab uses the session (an RPC client may use it too: monsrv_has_client())
static uint32_t g_hash, g_last;
static uint32_t g_try;                          // the last time mon_serve() tried to listen

static void session_open(void)
{
    if (g_open) {
        return;
    }
    g_open = true;
    trx_table();
    g_ok = hw_init(&s_trx);                     // 0 without regs or without the debug page (CAPS b3)
    ui_init(&s_ui);
    ui_set_size(&s_ui, UI_WIDE_COLS, UI_WIDE_ROWS);
    int fkey[9] = { 0, KEY_PAGEUP, 0, KEY_HELP, 0, KEY_TASKS, 0, KEY_PAGEDOWN, 0 };       // the Ultimate interface: F1 F3 F5 F7 (ui_set_keys names them in the hint row)
    ui_set_keys(&s_ui, fkey);
    if (!g_ok) {
        ui_open(&s_ui);
        ui_print(&s_ui, "The debug block is not in this bitstream (CAPS b3): the monitor has nothing to talk to.", UA_ERR);
        return;
    }
    mach_init();
    mon_init();
    rpc_init();
    ui_open(&s_ui);
    rpc_take_user_stop();
    ui_print(&s_ui, "TRX64 Monitor (trxmon). F3 help, F1/F7 scroll, x resume+leave.", UA_NORMAL);
    if (monsrv_status()[0]) {
        ui_print(&s_ui, monsrv_status(), UA_NORMAL);
    }
}

void mon_open(void)
{
    g_menu = true;
    session_open();
}

bool mon_claim(void)
{
    session_open();
    return g_ok;
}

static void session_end(void)
{
    if (g_ok && s_trx.release) {
        s_trx.release();                        // the base's release does nothing when the debug block was not acquired
    }
    g_open = false;
}

void mon_serve(void)
{
    if (!monsrv_listening() && (!g_try || plat_ms() - g_try >= 5000)) {
        g_try = plat_ms() | 1;
        monsrv_start();
    }
    // The screen's ring file job (ring.h) runs below, one step per pass. Between two of its steps the machine stays as it is: the RPC's requests wait in
    // their socket while it is queued or running.
    if (ring_job_state() != 1) {
        monsrv_poll();
    }
    if (g_open && g_ok) {
        rpc_poll();
        if (rpc_quit_requested()) {                 // /quit over the RPC: the holder goes, as the application's end took it
            monsrv_drop_client();
            if (g_menu) {
                g_leave = true;
            }
        }
        ring_job_poll();
    }
    if (g_open && !g_menu && !monsrv_has_client()) {
        session_end();
    }
}

void mon_release(void)
{
    if (!g_open) {
        return;
    }
    ring_job_drop();                            // the screen's ring file job goes with the screen (a dump's partial file is removed)
    g_menu = false;
    if (monsrv_has_client()) {                  // an RPC client still holds the session (breakpoints, observers)
        return;
    }
    session_end();
}

// RUN/STOP pauses the running machine, and cancels the screen's ring file job while one is on (ui.c)
bool mon_wants_stop(void) { return g_open && g_ok && (M.running || ring_job_state() != 0); }

bool mon_key(int k)
{
    g_menu = true;
    session_open();
    switch (k) {                                // the keyboards deliver F1 F3 F5 F7 as function keys; trxmon names them PAGEUP HELP TASKS PAGEDOWN
    case KEY_F1: k = KEY_PAGEUP; break;
    case KEY_F3: k = KEY_HELP; break;
    case KEY_F5: k = KEY_TASKS; break;
    case KEY_F7: k = KEY_PAGEDOWN; break;
    default: break;
    }
    if (!g_ok) {
        return true;
    }
    if (k == KEY_RETURN && !strcmp(s_ui.cmd, "diag")) {               // the way to Diagnostics: no key of the monitor's leaves to another tab
        s_ui.clen = s_ui.cpos = 0;
        s_ui.cmd[0] = 0;
        g_diag = true;
        return true;
    }
    int m = ui_map_key(k);
    if (m) {
        int r = ui_key(&s_ui, m);
        if (r != UI_STAY) {
            g_leave = true;
        }
    }
    return true;
}

bool mon_leave_requested(void)
{
    bool r = g_leave;
    g_leave = false;
    return r;
}

bool mon_diag_requested(void)
{
    bool r = g_diag;
    g_diag = false;
    return r;
}

bool mon_tick(void)
{
    if (!g_open || !g_ok) {
        return false;
    }
    rpc_poll();
    bool d = ui_tick(&s_ui) != 0;
    uint32_t now = plat_ms();
    if (now - g_last >= 250) {                  // the status rows follow the machine, the command row a ring file job's progress
        g_last = now;
        uint32_t was = g_hash;                  // the screen as last built (mon_sig after the base's render, or this tick's)
        mon_render();
        d |= mon_sig() != was;
    }
    return d;
}

void mon_run(const char *line)
{
    g_menu = true;
    session_open();
    if (!g_ok) {
        return;
    }
    for (const char *p = line; *p; p++) {
        ui_key(&s_ui, (unsigned char)*p);
    }
    ui_key(&s_ui, UI_K_RETURN);
}

// ---- the rows ---------------------------------------------------------------------------------------------------------------------------------------------------
void mon_render(void)
{
    if (!g_open) {                              // no session: no rows, and nothing of the last session's screen
        s_scr.cols = UI_WIDE_COLS;
        s_scr.rows = 0;
        s_scr.blank_rows = 0;
        s_scr.cursor_x = s_scr.cursor_y = 0;
        return;
    }
    ui_render(&s_ui, &s_scr);
}

int mon_cols(void) { return s_scr.cols; }
int mon_rows(void) { return s_scr.rows; }
const char *mon_line(int r) { return r >= 0 && r < s_scr.rows ? s_scr.row[r] : ""; }
int mon_attr(int r) { return r >= 0 && r < s_scr.rows ? s_scr.attr[r] : 0; }
int mon_blank_rows(void) { return s_scr.blank_rows; }
void mon_cursor(int *x, int *y)
{
    *x = s_scr.cursor_x;
    *y = s_scr.cursor_y;
}

uint32_t mon_sig(void)
{
    uint32_t h = 2166136261u;
    for (int r = 0; r < s_scr.rows; r++) {
        for (const char *p = s_scr.row[r]; *p; p++) {
            h = (h ^ (uint8_t)*p) * 16777619u;
        }
        h = (h ^ (uint32_t)s_scr.attr[r] ^ 0x100u) * 16777619u;
    }
    h = (h ^ (uint32_t)s_scr.cursor_x ^ ((uint32_t)s_scr.cursor_y << 8)) * 16777619u;
    g_hash = h;
    return h;
}
