// "Timewarp" in the freeze menu (TRX64-Ultimate docs/specs/T20-delta-ring.md
// §11, §16.7): scrubs through the frames of the ring and rewinds to one.
//
// On the pixel layer (HDMI, the TRX64 bitstream) the menu box goes and the C64
// picture shows at full brightness with one bar over the lower border
// (trx_video_tw_*); every cursor move puts the paused machine on that frame
// and lets it draw it (trx_ring_scrub_show), so the picture follows the
// cursor. Without the layer a window over the menu shows the numbers only.
// Keys (after Gideon's keymapper: F1/F7 arrive as PageUp/PageDown):
//   LEFT / RIGHT    one frame back / forward
//   UP / DOWN       one second back / forward
//   F1 / F7         ten seconds back / forward
//   HOME            the oldest frame
//   RETURN          rewind there: the menu closes and the machine runs from it
//   RUN/STOP        the menu closes; the machine goes on from the freeze point,
//                   or, once a frame was shown, from the newest frame
// The rewind itself happens when the menu closes (C64::unfreeze()).

#include "trx_ring.h"
#include "trx_ring_reu.h"
#include "trx_c64re.h"
#include "rtc.h"
#include "filemanager.h"
#include "itu.h"
#include "trx_io.h"
#include "trx_video.h"
#include "c64.h"
#include "menu.h"
#include "tasks_collection.h"
#include "action.h"
#include "subsys.h"
#include "userinterface.h"
#include "ui_elements.h"
#include "screen.h"
#include "keyboard.h"

// app_registry.cc: starts the TRX64 monitor on a ring entry (U4). Absent without the app loader, so
// a build without it simply has no monitor key here.
// trx_service.cc (U5): the monitor is the firmware's debug service; weak, because a target without
// the debug block does not build it.
extern "C" bool trx_service_present(void) __attribute__ ((weak));
extern "C" void trx_service_open_at(UserInterface *ui, int ring_index) __attribute__ ((weak));

static bool monitor_reachable(void)
{
    return trx_service_present && trx_service_present();
}
#include <stdio.h>
#include <string.h>

#if (U64 == 2) && !defined(RECOVERYAPP)

#define WIN_W          38
#define WIN_H          10

class TrxTimewarpUI : public UIObject {
    Window *win;
    Keyboard *kb;
    bool bar;       // on the pixel layer (else the window over the menu)
    int count;      // timeline entries
    int back;       // frames back from the newest
    int glyph;      // TW_GLYPH_* of the last move
    bool stepped;   // a frame was shown: the machine is no longer at the freeze point
    uint32_t shown; // seq of the frame shown (stepped)
    const char *msg;
    bool msg_warn;
public:
    int chosen;     // timeline index on RETURN, else -1

    TrxTimewarpUI(UserInterface *ui) : UIObject(ui), win(NULL), kb(NULL), bar(false), count(0), back(0),
        glyph(TW_GLYPH_PAUSE), stepped(false), shown(0), msg(NULL), msg_warn(false), chosen(-1) { }

    void init()
    {
        kb = get_ui()->get_keyboard();
        count = trx_ring_count();
        kb->wait_free();
        bar = trx_video_tw_begin();
        if (bar) {
            back = 0;   // the freeze point, as the still shows it
            draw_bar(NULL);
            return;
        }
        Screen *screen = get_ui()->get_screen();
        back = count > 1 ? trx_ring_fps() : 0;     // one second back to start with
        if (back > count - 1) {
            back = count - 1;
        }
        screen->backup();
        win = new Window(screen, (screen->get_size_x() - WIN_W) / 2, (screen->get_size_y() - WIN_H) / 2, WIN_W, WIN_H);
        win->clear();
        win->draw_border();
        win->set_color(get_ui()->color_fg);
        draw();
    }

    // ---- the bar --------------------------------------------------------------
    void draw_bar(const char *hint)
    {
        char pos[16], info[48];
        trx_tw_bar_t b;
        int target = count ? trx_ring_target(back) : -1;
        b.glyph = glyph;
        b.pos = pos;
        b.info = info;
        b.hint = hint ? hint : "RETURN jump  STOP cancel";
        b.track_pos = target < 0 ? 0 : target;
        b.track_len = count;
        b.warn = false;
        if (!stepped && back == 0) {
            strcpy(pos, "now");
        } else if (target >= 0) {
            int f = count - 1 - target;
            int fps = trx_ring_fps();
            sprintf(pos, "-%d.%d s", f / fps, (f % fps) * 10 / fps);
        } else {
            strcpy(pos, "-");
        }
        if (msg) {
            snprintf(info, sizeof(info), "%s", msg);
            b.warn = msg_warn;
        } else if (target < 0) {
            strcpy(info, count < 2 ? "the ring holds no frame yet" : "no frame to go back to");
            b.warn = true;
        } else {
            sprintf(info, "frame %d / %d", target + 1, count);
        }
        trx_video_tw_bar(&b);
    }

    // the cursor's frame on the machine, then the bar
    void show(void)
    {
        int target = trx_ring_target(back);
        TrxFrame f;
        msg = NULL;
        if (!stepped && back == 0) {
            target = -1;    // still at the freeze point: nothing to show but the still
            glyph = TW_GLYPH_PAUSE;
        }
        if (target >= 0 && trx_ring_frame(target, &f) && !(stepped && f.seq == shown)) {
            TrxRestoreStats st;
            int err = trx_ring_scrub_show(target, &st);
            stepped = trx_ring_scrubbing();
            if (!err) {
                shown = f.seq;
            } else {
                msg = trx_ring_error(err);
                msg_warn = true;
            }
            trx_video_tw_still();   // what the machine drew, also after a failed run
        }
        count = trx_ring_count();   // the run's frame may have dropped the oldest
        if (back > count - 1) {
            back = count > 0 ? count - 1 : 0;
        }
        if (stepped && back == 0) {
            glyph = TW_GLYPH_NEWEST;
        } else if (count && (back >= count - 1 || trx_ring_target(back + 1) < 0)) {
            glyph = TW_GLYPH_OLDEST;
        }
        draw_bar(NULL);
    }

    // ---- the window (no pixel layer) -----------------------------------------
    void line(int y, const char *s)
    {
        char buf[WIN_W];
        int n = strlen(s);
        if (n > WIN_W - 2) {
            n = WIN_W - 2;
        }
        memset(buf, ' ', WIN_W - 2);
        memcpy(buf, s, n);
        buf[WIN_W - 2] = 0;
        win->move_cursor(0, y);
        win->output(buf);
    }

    void draw()
    {
        char s[64];
        int head = count - 1;
        int target = trx_ring_target(back);
        int fps = trx_ring_fps();
        sprintf(s, " TIMEWARP     %d frames (%d.%d s)", count, count / fps, (count % fps) * 10 / fps);
        line(0, s);
        if (count < 2) {
            line(2, " The ring holds no frame yet.");
        } else {
            int b = head - (target < 0 ? head : target);
            sprintf(s, " frame -%d   -%d.%02d s", b, b / fps, (b % fps) * 100 / fps);
            line(2, s);
            // a bar: the ring's whole span, the cursor as a reversed cell
            char bar[WIN_W];
            int w = WIN_W - 4;
            int pos = w - 1 - (b * (w - 1)) / (head ? head : 1);
            for (int i = 0; i < w; i++) {
                bar[i] = i <= pos ? '=' : '-';
            }
            bar[w] = 0;
            win->move_cursor(1, 3);
            win->output(bar);
            TrxFrame f;
            if (target >= 0 && trx_ring_frame(target, &f)) {
                sprintf(s, " PC $%04X  snap x %d", (unsigned)(f.w[2] & 0xFFFF), f.x);
            } else {
                sprintf(s, " (no frame a rewind can reach)");
            }
            line(4, s);
        }
        line(6, " <- -> frame  UP/DN 1 s  F1/F7 10 s");
        line(7, " RETURN rewind+run   RUN/STOP back");
    }

    // ---- keys -------------------------------------------------------------------
    // Moves the cursor only; the frame is shown once the keys read so far are
    // done (a step takes a frame or more: intermediate targets are dropped).
    void move(int delta)
    {
        back += delta;
        if (back > count - 1) {
            back = count - 1;
        }
        if (back < 0) {
            back = 0;
        }
        // during a scrub the oldest entries need headroom (trx_ring.cc
        // SCRUB_MARGIN): the cursor stays on frames a step can go to
        while (bar && back > 0 && trx_ring_target(back) < 0) {
            back--;
        }
        if (delta) {
            glyph = delta > 0 ? TW_GLYPH_BACK : TW_GLYPH_FWD;
        }
    }

    // RETURN (jump) or RUN/STOP: 1, the menu closes
    bool to_monitor = false;   // U4: the way out is the monitor, not a rewind

    int leave(bool jump)
    {
        chosen = -1;
        if (jump) {
            if (bar && !stepped && back == 0) {
                return 1;   // the freeze point itself: nothing to rewind
            }
            chosen = trx_ring_target(back);
            if (chosen < 0 && !(bar && stepped)) {
                return 0;   // no frame there
            }
        }
        if (bar) {
            draw_bar(jump ? "rewinding" : (stepped ? "back to the newest frame" : "back"));
        }
        return 1;
    }

    int poll(int)
    {
        bool moved = false;
        for (;;) {
            int c = kb->getch();
            if (c == -1) {
                break;
            }
            if (c == -2) {
                return -1;
            }
            c = get_ui()->keymapper(c, e_keymap_default);
            switch (c) {
            case KEY_LEFT:      move(1); moved = true; break;
            case KEY_RIGHT:     move(-1); moved = true; break;
            case KEY_UP:        move(trx_ring_fps()); moved = true; break;
            case KEY_DOWN:      move(-trx_ring_fps()); moved = true; break;
            case KEY_PAGEUP:    move(10 * trx_ring_fps()); moved = true; break;
            case KEY_PAGEDOWN:  move(-10 * trx_ring_fps()); moved = true; break;
            case KEY_HOME:      move(count); moved = true; break;
            case KEY_RETURN:
                if (leave(true)) {
                    return 1;
                }
                break;
            case KEY_CTRL_O:
                // U4: into the monitor, on this frame. The browser opens the monitor with the same key.
                if (monitor_reachable() && leave(true)) {
                    to_monitor = (chosen >= 0);
                    return 1;
                }
                break;
            case KEY_BREAK:
            case KEY_ESCAPE:
                return leave(false);
            default:
                break;
            }
        }
        if (moved) {
            if (bar) {
                show();
            } else {
                draw();
            }
        }
        return 0;
    }

    void deinit()
    {
        if (win) {
            win->getScreen()->restore();
            delete win;
            win = NULL;
        }
    }
};

static SubsysResultCode_e timewarp_action(SubsysCommand *cmd)
{
    if (!cmd->user_interface) {
        return SSRET_NO_USER_INTERFACE;
    }
    const char *off = trx_ring_reu_refusal();   // A08: no ring while the REU is on
    if (off) {
        cmd->user_interface->popup(TRX_REU_OFF_POPUP, BUTTON_OK); // `off` is the long text of REST and RPC: too wide here
        return SSRET_OK;
    }
    C64 *c64 = C64::getMachine();
    if (!c64 || !c64->is_frozen()) {
        cmd->user_interface->popup("Timewarp works from the menu over a frozen machine.", BUTTON_OK);
        return SSRET_OK;
    }
    trx_ring_poll();
    TrxTimewarpUI *ui = new TrxTimewarpUI(cmd->user_interface);
    ui->init();
    int ret = cmd->user_interface->uiobject_modal(ui);
    int chosen = ui->chosen;
    bool to_monitor = ui->to_monitor;
    ui->deinit();
    delete ui;
    if (ret == 1 && to_monitor && chosen >= 0) {
        // U4: the monitor puts the machine on this entry itself (ring->land) and holds it there, so no
        // rewind is made pending: the timeline is not cut and leaving the monitor changes nothing.
        trx_service_open_at(cmd->user_interface, chosen);
    } else if (ret == 1 && chosen >= 0) {
        trx_ring_set_pending(chosen);
    }
    // Every way out closes the menu (RETURN, RUN/STOP, the menu button): the
    // box is hidden behind the bar, and after a scrub step only the rewind in
    // C64::unfreeze() puts a machine back that can run.
    cmd->user_interface->menu_response_to_action = MENU_HIDE;
    return SSRET_OK;
}

// ---- "Save State" (T20 §17.4, T66, U8) ----------------------------------------------
// The newest whole frame plus the medium as a .c64re of format 2, beside the image
// that is running (trx_c64re_place()). A progress box while it is written, then the
// result. The file carries the chip block, so the browser's "Load State" puts the
// machine back exactly (filetype_c64re.cc).
struct SaveBox {
    UserInterface *ui;
    bool shown;
};

static void save_progress(int done, int total, void *ctx)
{
    SaveBox *b = (SaveBox *)ctx;
    char msg[32];
    if (done == 0) {
        b->ui->show_progress("Saving the state", total);
        b->shown = true;
        return;
    }
    sprintf(msg, "Saving the state %d / %d", done, total);
    b->ui->update_progress(msg, 1);
}

// The name: <image>-<date>.c64re beside the image itself (U8 §5).
//
// Rtc::get_time() hands back the year as years since 1980 - set_time_utc() puts
// `tm_year - 80` into the chip and get_date_string() prints `1980 + y`. The old
// test here was `y >= 2024`, which no clock can ever satisfy, so every file took
// the fallback and was called ring-2540-t34 with the date branch dead. 1980 + y.
static void save_path(char *path, int n, const char *dir, const char *base)
{
    int y, M, D, wd, h, m, s;
    rtc.get_time(y, M, D, wd, h, m, s);
    y += 1980;
    if (y >= 2024) {                // a clock that was set
        snprintf(path, n, "%s/%s-%04d%02d%02d-%02d%02d%02d.c64re", dir, base, y, M, D, h, m, s);
    } else {
        snprintf(path, n, "%s/%s-t%u.c64re", dir, base, (unsigned)(getMsTimer() / 1000));
    }
}

// What the save needs: the ring runs and holds a frame it can read, and there is
// somewhere to put the file. Returns the timeline index, or -1 after it put the
// reason on the screen.
static int save_target(UserInterface *ui, TrxFrame *newest, char *dir, int dirlen,
                       char *base, int baselen)
{
    const char *off = trx_ring_reu_refusal();   // A08: no ring while the REU is on
    if (off) {
        ui->popup(TRX_REU_OFF_POPUP, BUTTON_OK);
        return -1;
    }
    trx_ring_poll();
    int target = trx_ring_target(0);
    if (target < 0 || trx_ring_count() < 2 || !trx_ring_frame(target, newest)) {
        ui->popup("The ring holds no frame yet.", BUTTON_OK);
        return -1;
    }
    if (!trx_c64re_place(dir, dirlen, base, baselen)) {
        ui->popup("No USB stick or SD card to save to.", BUTTON_OK);
        return -1;
    }
    return target;
}

// State and medium, one file, beside the image that is running (U8). The
// cartridge goes in as it stands at the freeze, once - a saved game that the
// C64 wrote into the flash comes back with it.
static SubsysResultCode_e save_state_action(SubsysCommand *cmd)
{
    if (!cmd->user_interface) {
        return SSRET_NO_USER_INTERFACE;
    }
    UserInterface *ui = cmd->user_interface;
    TrxFrame newest;
    char dir[160], base[48], path[224], msg[256];
    int target = save_target(ui, &newest, dir, sizeof(dir), base, sizeof(base));
    if (target < 0) {
        return SSRET_OK;
    }
    save_path(path, sizeof(path), dir, base);
    uint32_t bytes = 0;
    SaveBox box = { ui, false };
    const char *err = trx_c64re_dump_ex(path, target, &bytes, save_progress, &box);
    if (box.shown) {
        ui->hide_progress();
    }
    char line[256];
    if (err) {
        snprintf(line, sizeof(line), "Saving the state failed: %s", err);
    } else {
        // the folder and the name on their own lines: a path in one line is
        // longer than the overlay is wide, and UIPopup does not wrap
        const char *name = strrchr(path, '/');
        snprintf(line, sizeof(line), "%u KB written to\n%.*s\n%s",
                 (unsigned)(bytes / 1024), name ? int(name - path) : 0, path,
                 name ? name + 1 : path);
    }
    trx_c64re_wrap(msg, sizeof(msg), line, ui->get_screen()->get_size_x() - 6);
    ui->popup(msg, BUTTON_OK);
    return SSRET_OK;
}

class TrxTimewarpMenu : public ObjectWithMenu {
public:
    void create_task_items(void)
    {
        if (!(trx_io_caps() & 0x04)) {     // CAPS b2: the engine
            return;
        }
        TaskCategory *cat = TasksCollection::getCategory("C64 Machine", SORT_ORDER_C64);
        cat->append(new Action("Timewarp", timewarp_action, 0, 0));
        // One entry, replacing Save Timewarp Frame / Ring / Ring (native)
        // (U8 §5). Writing the ring to a file is gone; .c64rering and .trxring
        // still load (U8 §7).
        cat->append(new Action("Save State", save_state_action, 0, 0));
    }
};

static TrxTimewarpMenu trx_timewarp_menu;

#endif
