/*
 * ui_test_main.c — trxmon's own app_main (src/main.c) with the screen: the firmware's tables are
 * stood in for by a scripted keyboard, a window kept as text (80 x 40 through screen_open_wide, or
 * the 40 x 23 screen_open_full where the firmware has no wide screen), a net table with no
 * clients and the fake debug block, whose machine runs a little loop. The detached task is a
 * thread and trx->ui_request hands the callback to the test, which runs it as the firmware's UI
 * task would. Checks T21-monitor.md §8.4 and §8.5: leaving, the resident watch, the screen coming
 * back on a stop, headless, ending.
 */
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#include "u2p_app.h"
#include "fake_hw.h"
#include "../src/mach.h"

static int checks, fails;
#define CHECK(c)                                                                                                       \
    do {                                                                                                               \
        checks++;                                                                                                      \
        if (!(c)) {                                                                                                    \
            fails++;                                                                                                   \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                                        \
        }                                                                                                              \
    } while (0)

/* ---- the fake firmware -------------------------------------------------- */

static uint32_t h_tick_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, 0);
    return (uint32_t)(tv.tv_sec * 1000 + tv.tv_usec / 1000);
}
/* the smallest non-zero wait the app asked for: the firmware rounds delay_ms down to ticks (5 ms), so
 * anything shorter than PLAT_SLEEP_MIN_MS is a bare yield there, and a bare yield never lets the serve
 * loop run under the UI task */
static volatile uint32_t g_min_sleep;
static volatile int g_close_menu;
static void h_close_menu(void) { g_close_menu++; }
static void h_delay(uint32_t ms)
{
    if (ms && (!g_min_sleep || ms < g_min_sleep)) {
        g_min_sleep = ms;
    }
    usleep(ms ? ms * 1000 : 100);
}
static int h_log(const char *fmt, ...) { (void)fmt; return 0; }
static int h_listen(uint16_t port, int backlog) { (void)port; (void)backlog; return 3; }
static int h_accept(int l) { (void)l; return U2P_ERR_BUSY; }
static int h_recv(int h, void *b, int n) { (void)h; (void)b; (void)n; return U2P_ERR_BUSY; }
static int h_send(int h, const void *b, int n) { (void)h; (void)b; return n; }
static volatile int g_listener_closed;
static int h_close(int h)
{
    if (h == 3) {
        g_listener_closed = 1;
    }
    return 0;
}
static int h_wait(const int *handles, int n, uint32_t timeout_ms)
{
    (void)handles; (void)n; (void)timeout_ms;
    usleep(1000);
    fake_tick(300); /* the machine runs between two looks at the sockets */
    return 0;
}
static int h_source(int role, char *buf, int len) { (void)role; (void)buf; (void)len; return 0; }
static int h_acquire(uint32_t f) { (void)f; return 0; }
static void h_release(void) {}
static void h_announce(uint16_t p) { F.announced = p; }
static void h_popup(const char *t, const char *m) { printf("popup: %s: %s\n", t, m); }
static int h_reply(const char *t) { (void)t; return 0; }

/* the window: a grid of text, the state of the last frame closed. screen_open_wide gives 80 x 40 and
 * counts what is open (the overlay is wide for as long as it is), screen_open_full 40 x 23 */
#define GR 40
#define GC 81
static char g_grid[GR][GC];
static char g_closed[GR][GC];
static int g_cur_x = -1, g_cur_y = -1;
static volatile int g_opened, g_closed_n;
static volatile int g_wide_opened, g_wide_open; /* opens of the wide screen, and how many are open now */
static int g_wide_ok = 1;                       /* the firmware has screen_open_wide */
static int g_wide_rows_asked;
static int g_rev;
static int g_win;
static int g_cols = 40, g_rows = 23;
static int g_open_wide_now;

static void grid_clear(int cols, int rows)
{
    g_cols = cols;
    g_rows = rows;
    memset(g_grid, ' ', sizeof(g_grid));
    for (int y = 0; y < GR; y++) {
        g_grid[y][cols] = 0;
    }
}
static void *h_open_full(void)
{
    g_opened++;
    g_open_wide_now = 0;
    grid_clear(40, 23);
    return &g_win;
}
/* A Freeze firmware (menu on the C64's screen): while the wide window is open the overlay is shown and the
 * freeze's text layer and pixel layer are off; every close puts them back. g_fz_nolayers: no TRX layers
 * (Gideon's bitstream), the firmware refuses the wide screen. */
static int g_fz, g_fz_nolayers;
static int g_fz_tx = 1, g_fz_px = 1, g_fz_ovl = 0;
static int fz_menu(void) { return g_fz_tx == 1 && g_fz_px == 1 && g_fz_ovl == 0; }
static int g_wide_refuse; /* the call exists but there is no overlay screen (the menu is on the C64's own) */
static void *h_open_wide(int rows)
{
    g_wide_rows_asked = rows;
    if (g_wide_refuse || (g_fz && g_fz_nolayers)) {
        return 0;
    }
    if (g_fz) {
        g_fz_tx = 0;
        g_fz_px = 0;
        g_fz_ovl = 1;
    }
    g_opened++;
    g_wide_opened++;
    g_wide_open++;
    g_open_wide_now = 1;
    grid_clear(80, rows);
    return &g_win;
}
static int h_size_x(void *w) { (void)w; return g_cols; }
static int h_size_y(void *w) { (void)w; return g_rows; }
static void h_close_win(void *w)
{
    (void)w;
    memcpy(g_closed, g_grid, sizeof(g_grid));
    if (g_open_wide_now) {
        g_wide_open--; /* the overlay is 40 x 25 again */
        g_open_wide_now = 0;
        if (g_fz) {
            g_fz_tx = 1;
            g_fz_px = 1;
            g_fz_ovl = 0;
        }
    }
    g_closed_n++;
}
static void h_puts(void *w, int x, int y, const char *s, int color)
{
    (void)w; (void)color;
    for (int i = 0; s[i] && (x + i < g_cols); i++) {
        g_grid[y][x + i] = s[i];
    }
}
static void h_bg(void *w, int c) { (void)w; (void)c; }
static void h_rev(void *w, int on) { (void)w; g_rev = on; }
static void h_setchar(void *w, int x, int y, char c)
{
    (void)w; (void)c;
    if (g_rev) {
        g_cur_x = x;
        g_cur_y = y;
    }
}
static int g_commodore; /* the Commodore interface: F1 Tasks, F3 PageUp, F5 PageDown, F7 Help */
static int h_fkey(int n)
{
    static const int ult[9] = { 0, 0x92, 0x1FA, 0x1FB, 0x1F9, 0x1FC, 0x1FD, 0x95, 0x1FE };
    static const int com[9] = { 0, 0x1FC, 0x1FA, 0x92, 0x1F9, 0x95, 0x1FD, 0x1FB, 0x1FE };
    return (n >= 1 && n <= 8) ? (g_commodore ? com[n] : ult[n]) : -1;
}
static int h_color(int n) { return n; }

/* the keyboard: a queue the test fills; \n is RETURN. g_wasd: "Navigation Style: WASD Cursors", the default
 * keymap turns w a s d into cursor keys; g_gone_after: how many empty polls before the screen is gone (-2) */
static unsigned char g_keys[256];
static volatile int g_kw, g_kr;
static int g_wasd, g_gone_after, g_mon_polls, g_def_polls, g_idle;
static void push_keys(const char *k)
{
    g_kw = 0;
    g_kr = 0;
    g_idle = 0;
    for (; *k; k++) {
        g_keys[g_kw++] = (*k == '\n') ? 0x0D : (unsigned char)*k;
    }
}
static char g_first[GR][GC]; /* the window when the session first asks for a key: its opening frame */
static int next_raw(void)
{
    if (g_kr == 0) {
        memcpy(g_first, g_grid, sizeof(g_grid));
    }
    if (g_kr < g_kw) {
        return g_keys[g_kr++];
    }
    if (g_gone_after && (++g_idle > g_gone_after)) {
        return -2;
    }
    return -1;
}
/* the default keymap: WASD cursors when on (the interface's function keys give the codes the test pushes) */
static int h_getch(void)
{
    g_def_polls++;
    int k = next_raw();
    if (k < 0) {
        return k;
    }
    if (g_wasd) {
        if (k == 'w') return 0x91;
        if (k == 'a') return 0x9D;
        if (k == 's') return 0x12;
        if (k == 'd') return 0x1D;
    }
    return k;
}
/* the monitor's keymap: w a s d stay letters */
static int g_have_mon_keys = 1;
static int h_getch_monitor(void)
{
    g_mon_polls++;
    return next_raw();
}

/* trx->detach: a thread; trx->ui_request: the callback is kept for the test */
static pthread_t g_th;
static volatile int g_detached;
static volatile int g_stop_req;
static void (*g_task)(void *);
static int g_defer;
static void start_resident(void) { pthread_create(&g_th, 0, (void *(*)(void *))(void *)g_task, 0); }
int hw_detach(void (*fn)(void *), void *arg)
{
    (void)arg;
    g_detached++;
    g_task = fn;
    if (g_defer) {
        return 0; /* the test starts it once the machine is set up */
    }
    start_resident();
    return 0;
}
int hw_stop_requested(void) { return g_stop_req; }
static volatile int g_ui_requests;
static void (*volatile g_ui_fn)(void *);
static int g_ui_result;
int hw_ui_request(void (*fn)(void *), void *arg)
{
    (void)arg;
    if (g_ui_result == 0) {
        g_ui_fn = fn;
    }
    g_ui_requests++;
    return g_ui_result;
}

static u2p_api_t api;
static u2p_app_context_t ctx;

static void setup(int has_ui)
{
    fake_reset();
    static const uint8_t prg[] = { 0xA9, 0x01, 0x20, 0x10, 0xC0, 0x4C, 0x00, 0xC0 };
    memcpy(F.ram + 0xC000, prg, sizeof(prg));
    F.ram[0xC010] = 0xE8;
    F.ram[0xC011] = 0x60;
    g_listener_closed = 0;
    g_opened = g_closed_n = 0;
    g_detached = 0;
    g_ui_requests = 0;
    g_ui_fn = 0;
    g_ui_result = 0;
    g_defer = 0;
    g_stop_req = 0;
    g_cur_x = g_cur_y = -1;
    memset(g_closed, ' ', sizeof(g_closed));
    g_wide_opened = g_wide_open = 0;
    g_open_wide_now = 0;
    g_fz_tx = g_fz_px = 1;
    g_fz_ovl = 0;
    g_wasd = g_gone_after = g_mon_polls = g_def_polls = 0;
    g_wide_rows_asked = 0;
    static u2p_sys_api_t sys;
    sys.size = sizeof(sys);
    sys.log = h_log;
    sys.tick_ms = h_tick_ms;
    sys.delay_ms = h_delay;
    static u2p_ui_api_t ui;
    memset(&ui, 0, sizeof(ui));
    ui.size = sizeof(ui);
    ui.popup = h_popup;
    ui.getch = h_getch;
    ui.fkey = h_fkey;
    ui.color = h_color;
    ui.screen_open_full = h_open_full;
    ui.screen_open_wide = g_wide_ok ? h_open_wide : 0;
    ui.win_get_size_x = h_size_x;
    ui.win_get_size_y = h_size_y;
    ui.getch_monitor = g_have_mon_keys ? h_getch_monitor : 0;
    ui.screen_close = h_close_win;
    ui.screen_puts = h_puts;
    ui.win_set_background = h_bg;
    ui.win_reverse_mode = h_rev;
    ui.win_set_char_at = h_setchar;
    ui.close_menu = h_close_menu;
    g_close_menu = 0;
    g_min_sleep = 0;
    static u2p_media_api_t media;
    media.size = sizeof(media);
    media.source = h_source;
    static u2p_net_api_t net = { sizeof(u2p_net_api_t), h_listen, h_accept, h_recv, h_send, h_close, h_wait, 0 };
    static u2p_trx_api_t trx;
    trx.size = sizeof(trx);
    trx.regs = (volatile uint8_t *)1;
    trx.caps = 0xFF;
    trx.acquire = h_acquire;
    trx.release = h_release;
    trx.announce = h_announce;
    api.abi_major = U2P_ABI_MAJOR;
    api.abi_minor = U2P_ABI_MINOR;
    api.size = sizeof(api);
    api.sys = &sys;
    api.ui = &ui;
    api.media = &media;
    api.net = &net;
    api.trx = &trx;
    ctx.size = sizeof(ctx);
    ctx.app_path = "host";
    ctx.has_ui = has_ui;
    ctx.reply = h_reply;
}

static void on_alarm(int s)
{
    (void)s;
    fprintf(stderr, "FAIL: the test hung\n");
    _exit(2);
}

static int wait_for(volatile int *v, int want)
{
    for (int i = 0; i < 5000 && (*v < want); i++) {
        usleep(1000);
    }
    return *v >= want;
}

static int closed_has(const char *needle)
{
    for (int y = 0; y < GR; y++) {
        if (strstr(g_closed[y], needle)) {
            return 1;
        }
    }
    return 0;
}

static int grid_has(const char *needle)
{
    for (int y = 0; y < GR; y++) {
        if (strstr(g_first[y], needle)) {
            return 1;
        }
    }
    return 0;
}

/* runs the queued ui callback as the firmware's UI task would */
static void run_ui_request(const char *keys)
{
    void (*fn)(void *) = g_ui_fn;
    CHECK(fn != 0);
    g_ui_fn = 0;
    push_keys(keys);
    if (fn) {
        fn(0);
    }
}

int main(void)
{
    signal(SIGALRM, on_alarm);
    alarm(60);

    setvbuf(stdout, 0, _IOLBF, 0);
    /* 1. menu start, stops set, x: the screen closes, trxmon stays; a breakpoint brings it back, twice;
     *    then the person clears the stops and x ends trxmon completely. */
    setup(1);
    push_keys("bk c010\nx\n");
    int r = app_main(&api, &ctx);
    CHECK(r == 0);
    CHECK(g_opened == 1 && g_closed_n == 1);
    CHECK(g_wide_opened == 1 && g_wide_open == 0 && g_wide_rows_asked == 40); /* the wide screen came and went */
    CHECK(closed_has("device c64"));                                       /* the 80 column layout was drawn */
    CHECK(g_detached == 1);
    CHECK(!g_listener_closed);        /* it stays on */
    CHECK(F.announced == 4312);
    CHECK(closed_has("RUNNING"));
    CHECK(g_close_menu == 1);           /* x: the whole overlay goes, not back to the file browser */
    CHECK(wait_for(&g_ui_requests, 1)); /* the bp at $c010 fired: the screen is asked for */
    CHECK(g_ui_fn != 0);
    CHECK(g_opened == 1);               /* the loader runs it, not the task */
    run_ui_request("r\nx\n");           /* stops still armed: the screen goes again, trxmon stays */
    CHECK(g_opened == 2 && g_closed_n == 2);
    CHECK(g_wide_opened == 2 && g_wide_open == 0);
    CHECK(g_close_menu == 1);           /* the firmware's own request closes the overlay itself */
    CHECK(grid_has(".C:c010"));
    CHECK(!g_listener_closed);
    CHECK(wait_for(&g_ui_requests, 2));
    CHECK(g_ui_fn != 0);
    run_ui_request("del\nx\n");         /* nothing armed: x ends it */
    CHECK(grid_has("breakpoint 1 hit at .C:c010"));
    pthread_join(g_th, 0);
    CHECK(g_listener_closed && F.announced == 0);
    CHECK(g_ui_requests == 2);
    CHECK(g_min_sleep >= PLAT_SLEEP_MIN_MS); /* the callback waited with real sleeps only */
    CHECK(g_wide_opened == 3 && g_wide_open == 0);

    /* 1b. a JAM while trxmon watches in the background is a stop like a breakpoint: the screen is asked for and
     *     says why it came (T21 8.5). The opcode $02 where the loop jumps back stands the CPU still. */
    setup(1);
    F.ram[0xC005] = 0x02;
    push_keys("bk c800\nx\n");
    r = app_main(&api, &ctx);
    CHECK(r == 0 && g_detached == 1);
    CHECK(wait_for(&g_ui_requests, 1)); /* no breakpoint was hit: the jam asked */
    CHECK(g_ui_fn != 0);
    run_ui_request("del\nx\n");
    CHECK(grid_has("JAM at .C:c005"));
    pthread_join(g_th, 0);

    /* 2. menu start, no stop armed, x: ends completely (no task) */
    setup(1);
    push_keys("x\n");
    r = app_main(&api, &ctx);
    CHECK(r == 0 && g_detached == 0);
    CHECK(g_listener_closed && F.announced == 0);
    CHECK(F.running);
    CHECK(g_opened == 1 && g_closed_n == 1);
    CHECK(g_close_menu == 1);           /* x with nothing armed leaves the overlay as well */
    CHECK(g_wide_opened == 1 && g_wide_open == 0);

    /* 3. /quit with stops armed: disarmed by the release, trxmon ends, the machine as it was */
    setup(1);
    push_keys("bk c010\n/quit\n");
    r = app_main(&api, &ctx);
    CHECK(r == 0 && g_detached == 0 && g_listener_closed);
    CHECK(!F.running);
    CHECK(g_close_menu == 0);           /* /quit goes back to the file browser */
    CHECK(g_wide_opened == 1 && g_wide_open == 0);

    /* 4. the screen is gone (getch -2) or the loader wants the app to end: it ends */
    setup(1);
    g_stop_req = 1;
    push_keys("");
    r = app_main(&api, &ctx);
    CHECK(r == 0 && g_detached == 0 && g_listener_closed);
    CHECK(g_close_menu == 0);
    CHECK(g_wide_open == 0);
    g_stop_req = 0;

    /* 5. headless (REST serve): a task from the start, no screen; a stop set over RPC brings the
     *    screen up for the person, and x with nothing armed ends it */
    setup(0);
    g_defer = 1;
    r = app_main(&api, &ctx);
    CHECK(r == 0 && g_detached == 1 && g_opened == 0);
    mach_bp_add(0xC010); /* what a client's break_add and debug/run do */
    mach_run();
    start_resident();
    CHECK(wait_for(&g_ui_requests, 1));
    CHECK(g_opened == 0);
    run_ui_request("del\nx\n");
    CHECK(g_opened == 1);
    CHECK(grid_has("breakpoint 1 hit at .C:c010"));
    pthread_join(g_th, 0);
    CHECK(g_listener_closed);

    /* 6. no ui_request in the firmware: the stop is only notified, trxmon goes on, no flood */
    setup(0);
    g_ui_result = -6;
    g_defer = 1;
    app_main(&api, &ctx);
    mach_bp_add(0xC010);
    mach_run();
    start_resident();
    CHECK(wait_for(&g_ui_requests, 1));
    usleep(100000);
    CHECK(g_ui_requests == 1 && g_opened == 0 && !g_listener_closed);
    g_stop_req = 1;
    pthread_join(g_th, 0);
    CHECK(g_listener_closed);
    g_stop_req = 0;

    /* 7. a request already queued (BUSY): not repeated while it waits */
    setup(0);
    g_ui_result = -5;
    g_defer = 1;
    app_main(&api, &ctx);
    mach_bp_add(0xC010);
    mach_run();
    start_resident();
    CHECK(wait_for(&g_ui_requests, 1));
    usleep(50000);
    CHECK(g_ui_requests == 1);
    g_stop_req = 1;
    pthread_join(g_th, 0);
    g_stop_req = 0;

    /* 8. the monitor's own keymap: with "WASD Cursors" the default keymap would turn w a s d into
     *    cursor keys; getch_monitor leaves them as letters */
    setup(1);
    g_wasd = 1;
    push_keys("wasd\n/quit\n");
    app_main(&api, &ctx);
    CHECK(closed_has(".wasd") && g_mon_polls > 0 && g_def_polls == 0);
    CHECK(g_wide_open == 0);

    /* 9. a firmware without getch_monitor: the default keymap (w a s d are cursor keys there, which is the
     *    limit the old firmware has), the function keys through ui->fkey */
    g_have_mon_keys = 0;
    setup(1);
    g_wasd = 1;
    push_keys("wasd\n/quit\n");
    app_main(&api, &ctx);
    CHECK(!closed_has(".wasd") && g_mon_polls == 0 && g_def_polls > 0);
    g_have_mon_keys = 1;

    /* 10. PageUp and PageDown (F1 and F7 in the Ultimate interface) scroll the history; the screen ends when it is gone (-2) */
    setup(1);
    g_gone_after = 30;
    push_keys("help\nhelp\n\x92");
    app_main(&api, &ctx);
    CHECK(closed_has("history -33 lines"));
    CHECK(g_closed_n == 1 && g_wide_open == 0);
    setup(1);
    g_gone_after = 30;
    push_keys("help\nhelp\n\x92\x92\x95");
    app_main(&api, &ctx);
    CHECK(closed_has("history -33 lines") && !closed_has("history -66 lines"));
    setup(1);
    g_gone_after = 30;
    push_keys("help\nhelp\n\x92\x95");
    app_main(&api, &ctx);
    CHECK(!closed_has("history -")); /* back at the newest */
    CHECK(g_wide_open == 0);

    /* 11. the firmware has no wide screen, or refuses it (the menu is on the C64's own screen): the 40
     *     column window, nothing wide is opened */
    g_wide_ok = 0;
    setup(1);
    push_keys("/quit\n");
    app_main(&api, &ctx);
    CHECK(g_wide_opened == 0 && g_opened == 1 && g_closed_n == 1 && g_cols == 40);
    CHECK(closed_has("PAUSED IB") && !closed_has("device c64"));
    g_wide_ok = 1;
    g_wide_refuse = 1;
    setup(1);
    push_keys("/quit\n");
    app_main(&api, &ctx);
    CHECK(g_wide_opened == 0 && g_opened == 1 && g_closed_n == 1 && g_cols == 40 && g_wide_open == 0);
    g_wide_refuse = 0;

    /* 12. the screen goes away under the session (getch -2): the wide screen is closed, the app ends */
    setup(1);
    g_gone_after = 5;
    push_keys("");
    r = app_main(&api, &ctx);
    CHECK(r == 0 && g_wide_opened == 1 && g_wide_open == 0 && g_closed_n == 1 && g_listener_closed);

    /* 13. the Commodore interface: the screen names the keys it made PageUp, PageDown and Help, and those scroll */
    g_commodore = 1;
    setup(1);
    g_gone_after = 30;
    push_keys("help\nhelp\n\x92");
    app_main(&api, &ctx);
    CHECK(closed_has("F3/F5 scroll  F7 help") && closed_has("history -33 lines"));
    g_commodore = 0;

    /* 14. Freeze mode: the monitor is 80 x 40 there too; the layers are away while it is open and every way
     *     out gives the freeze menu back exactly (text layer on, pixel layer on, overlay hidden) */
    g_fz = 1;
    setup(1);
    push_keys("help\n/quit\n");
    app_main(&api, &ctx);
    CHECK(g_wide_opened == 1 && g_wide_open == 0 && g_cols == 80 && fz_menu());
    setup(1);
    push_keys("x\n");
    app_main(&api, &ctx);
    CHECK(g_wide_opened == 1 && g_wide_open == 0 && fz_menu() && g_close_menu == 1);
    setup(1);
    g_gone_after = 5;
    push_keys("");
    app_main(&api, &ctx);
    CHECK(g_wide_opened == 1 && g_wide_open == 0 && fz_menu());
    setup(1);
    g_stop_req = 1;
    push_keys("");
    app_main(&api, &ctx);
    CHECK(g_wide_open == 0 && fz_menu());
    g_stop_req = 0;
    /* a firmware without the TRX layers refuses: 40 columns, the freeze untouched */
    g_fz_nolayers = 1;
    setup(1);
    push_keys("/quit\n");
    app_main(&api, &ctx);
    CHECK(g_wide_opened == 0 && g_opened == 1 && g_cols == 40 && fz_menu());
    g_fz_nolayers = 0;
    g_fz = 0;

    printf("ui session: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
