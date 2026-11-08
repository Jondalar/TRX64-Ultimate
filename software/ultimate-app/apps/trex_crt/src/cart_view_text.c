/*
 * cart_view_text.c
 *
 *  The character screen of the cartridge tools (T95 D5). It draws what the core reports (cart_event_t), answers
 *  what the core asks (cart_ask_t) with the SDK dialogs, and turns the function keys into the core's operations.
 *  Everything a screen needs - the window, the layout, the key codes, the dialogs - is here and nowhere else, so
 *  the same cores drive MultiCore's Generic App UI (T104.3) without a second copy of the tools.
 */

#include "cart_view_text.h"

#include "cart_dumper.h"
#include "cart_fmt.h"
#include "cart_inspector.h"
#include "cart_screen.h"
#include "cart_view.h"
#include "cart_writer.h"
#include "ui_shim.h"

// One tool is open at a time, as one screen is
static Window *s_window;
static cart_layout_t s_layout;

// =========================================================================
//  The questions, the same for every tool
// =========================================================================

static int view_ask(void *ctx, const cart_ask_t *q)
{
    (void)ctx;
    switch (q->kind) {
    case CART_ASK_NOTIFY:
        u2p_ui_popup_buttons(q->prompt, BUTTON_OK);
        return 1;
    case CART_ASK_CONFIRM:
        return u2p_ui_popup_buttons(q->prompt, BUTTON_YES | BUTTON_NO) == BUTTON_YES ? 1 : 0;
    case CART_ASK_CHOICE:
        return u2p_ui_menu(q->prompt, (const char **)q->choices, q->nchoices);
    case CART_ASK_TEXT:
        // The string box edits in place: a cancel leaves what was offered, as the firmware's does
        return u2p_ui_string_input(q->prompt, q->buf, q->bufsize);
    case CART_ASK_PATH:
        return u2p_ui_path_input(q->buf, q->bufsize) < 0 ? -1 : 1;
    case CART_ASK_SAVE: {
        FRESULT res = create_user_file(q->prompt, q->ext, q->path, q->fh, q->buf);
        if (res == FR_OK && q->fname) {
            user_file_name(q->buf, q->ext, q->fname);
        }
        return (int)res;
    }
    }
    return -1;
}

// =========================================================================
//  The writer's screen
// =========================================================================

static void writer_header(cart_writer_t *w)
{
    cart_layout_compute(&s_layout, w->chip ? w->chip->max_banks : 1, w->chip ? w->chip->has_romh : false,
                        w->chip && w->base.is_ef3);

    char line[CART_SCREEN_COLS + 1];
    if (w->chip) {
        cart_snprintf(line, sizeof(line), "TREX Cart Writer - %s (%dKB)", w->chip->name, w->chip->total_size / 1024);
    } else {
        cart_snprintf(line, sizeof(line), "TREX Cart Writer - No cart detected");
    }
    cart_screen_line(s_window, s_layout.y_header, 1, line);

    // EasyFlash 3: the slot to write, or the slot of the image the writer is in
    if (w->chip && w->base.is_ef3) {
        if (w->img_slot >= 0) {
            cart_snprintf(line, sizeof(line), "Image: slot %d%s", w->img_slot,
                          w->img_slot == 0 ? " (system area)" : "");
        } else if (w->base.ef3_whole) {
            cart_snprintf(line, sizeof(line), "Full EF3 (all slots, type 90)");
        } else if (w->base.ef3_slot) {
            cart_snprintf(line, sizeof(line), "Slot %d%s%s", w->base.ef3_slot, w->base.ef3_name[0] ? ": " : "",
                          w->base.ef3_name);
        } else {
            cart_snprintf(line, sizeof(line), "No slot selected");
        }
        cart_screen_line(s_window, s_layout.y_slot, 15, line);
    }
    cart_screen_file(s_window, &s_layout, w->crt_filename);
}

static void writer_footer(cart_writer_t *w)
{
    static const char *const keys[] = { "F1", "Scan", "F3", "Write", "F5", "Verify", "R/S", "Exit" };
    static const char *const slot_keys[] = { "F7", "Slot/full/erase" };
    cart_screen_keys(s_window, s_layout.y_keys, keys, 4);
    if (w->chip && w->base.is_ef3)
        cart_screen_keys(s_window, s_layout.y_keys2, slot_keys, 1);
}

static void writer_redraw(cart_writer_t *w)
{
    window_clear(s_window);
    writer_header(w);
    cart_draw_bank_map(s_window, &s_layout, w->bank_status_lo, w->bank_status_hi);
    writer_footer(w);
}

static void writer_event(void *ctx, const cart_event_t *ev)
{
    cart_writer_t *w = (cart_writer_t *)ctx;
    if (!s_window)
        return;
    switch (ev->kind) {
    case CART_EV_STATUS:
        cart_screen_status(s_window, &s_layout, ev->text);
        break;
    case CART_EV_BANKS:
        cart_draw_bank_map(s_window, &s_layout, ev->lo, ev->hi);
        break;
    case CART_EV_HEADER:
        writer_header(w);
        writer_footer(w);
        break;
    case CART_EV_REDRAW:
        writer_redraw(w);
        break;
    case CART_EV_PROGRESS: // the status line and the bank map carry the progress on this screen
    case CART_EV_RESULT:   // the core puts its result in the status line
        break;
    }
}

// A dialog restores the screen it found when it closes, and that was not this one: the frame, the header and the
// keys are gone afterwards. The SDK dialogs call this when they close. Open the frame again, which clears the
// whole screen, and draw everything, rather than drawing the frame a second time into the old window, which
// would nest one frame inside the other.
static void writer_rebuild(void *ctx)
{
    if (s_window)
        window_close(s_window);
    s_window = cart_screen_open();
    writer_redraw((cart_writer_t *)ctx);
}

static int writer_poll(void *obj, int c)
{
    cart_writer_t *w = (cart_writer_t *)obj;
    switch (c) {
    case KEY_F1:
        cart_writer_scan(w);
        break;
    case KEY_F3:
        cart_writer_write(w);
        break;
    case KEY_F5:
        cart_writer_verify(w);
        break;
    case KEY_F7:
        cart_writer_select(w);
        break;
    case KEY_ESCAPE:
    case KEY_BACK:
    case KEY_BREAK:
        return MENU_CLOSE;
    }
    return 0;
}

void cart_view_text_writer(const char *path, const char *filename)
{
    cart_writer_t w;
    cart_view_t view = { writer_event, view_ask, &w };
    cart_view_set(&view);

    // A CRT is looked at before the screen opens: a file the writer could not use never gets a writer screen
    if (filename && filename[0]) {
        const char *bad = cart_writer_check_crt(path, filename);
        if (bad) {
            cart_notify(bad);
            cart_view_set(NULL);
            return;
        }
    }

    s_window = cart_screen_open();
    window_clear(s_window);
    u2p_ui_set_redraw(writer_rebuild, &w);

    cart_writer_open(&w, path, filename);
    ui_run(&w, writer_poll);
    cart_writer_close(&w);

    u2p_ui_set_redraw(NULL, NULL);
    cart_view_set(NULL);
    window_close(s_window);
    s_window = NULL;
}

// =========================================================================
//  The dumper's screen
// =========================================================================

static void dumper_header(cart_dumper_t *d)
{
    cart_layout_compute(&s_layout, d->det_banks > 0 ? d->det_banks : 1, d->has_romh, d->base.is_ef3);

    uint16_t bank_size = d->has_romh ? 0x4000 : 0x2000;
    int total_kb = (d->det_banks * bank_size * (d->base.ef3_whole ? EF3_NUM_SLOTS : 1)) / 1024;
    const char *name = d->probe_result.type_name ? d->probe_result.type_name : "Cart";
    char line[CART_SCREEN_COLS + 1];
    cart_snprintf(line, sizeof(line), "TREX Cart Dumper - %s (%dKB)", name, total_kb);
    cart_screen_line(s_window, s_layout.y_header, 1, line);

    // EasyFlash 3: the slot to dump, or the slot the whole-cart dump is reading
    if (d->base.is_ef3) {
        if (d->slot_shown >= 0) {
            cart_snprintf(line, sizeof(line), "Slot %d of 8%s", d->slot_shown,
                          d->slot_shown == 0 ? ": system area" : "");
        } else if (d->base.ef3_whole) {
            cart_snprintf(line, sizeof(line), "All slots, system area first");
        } else if (d->base.ef3_slot) {
            cart_snprintf(line, sizeof(line), "Slot %d%s%s", d->base.ef3_slot, d->base.ef3_name[0] ? ": " : "",
                          d->base.ef3_name);
        } else {
            cart_snprintf(line, sizeof(line), "No slot selected");
        }
        cart_screen_line(s_window, s_layout.y_slot, 15, line);
    }
    cart_screen_file(s_window, &s_layout, d->file);
}

static void dumper_footer(cart_dumper_t *d)
{
    static const char *const keys[] = { "F1", "Scan", "F3", "Dump", "F5", "Inspect", "R/S", "Exit" };
    static const char *const slot_keys[] = { "F7", "Select slot/full" };
    cart_screen_keys(s_window, s_layout.y_keys, keys, 4);
    if (d->base.is_ef3)
        cart_screen_keys(s_window, s_layout.y_keys2, slot_keys, 1);
}

static void dumper_redraw(cart_dumper_t *d)
{
    window_clear(s_window);
    dumper_header(d);
    cart_draw_bank_map(s_window, &s_layout, d->bank_status_lo, d->bank_status_hi);
    dumper_footer(d);
}

static void dumper_event(void *ctx, const cart_event_t *ev)
{
    cart_dumper_t *d = (cart_dumper_t *)ctx;
    if (!s_window)
        return;
    switch (ev->kind) {
    case CART_EV_STATUS:
        cart_screen_status(s_window, &s_layout, ev->text);
        break;
    case CART_EV_BANKS:
        cart_draw_bank_map(s_window, &s_layout, ev->lo, ev->hi);
        break;
    case CART_EV_HEADER:
        dumper_header(d);
        dumper_footer(d);
        break;
    case CART_EV_REDRAW:
        dumper_redraw(d);
        break;
    case CART_EV_PROGRESS:
    case CART_EV_RESULT:
        break;
    }
}

static void dumper_rebuild(void *ctx)
{
    if (s_window)
        window_close(s_window);
    s_window = cart_screen_open();
    dumper_redraw((cart_dumper_t *)ctx);
}

static int dumper_poll(void *obj, int c)
{
    cart_dumper_t *d = (cart_dumper_t *)obj;
    switch (c) {
    case KEY_F1:
        cart_dumper_scan(d);
        break;
    case KEY_F3:
        cart_dumper_dump(d);
        break;
    case KEY_F5:
        if (d->det_banks > 0) {
            cart_status("Use Cart Inspector from menu."); // this screen has no hex view
        }
        break;
    case KEY_F7:
        cart_dumper_select(d);
        break;
    case KEY_ESCAPE:
    case KEY_BACK:
    case KEY_BREAK:
        return MENU_CLOSE;
    }
    return 0;
}

void cart_view_text_dumper(void)
{
    cart_dumper_t d;
    cart_view_t view = { dumper_event, view_ask, &d };
    cart_view_set(&view);

    s_window = cart_screen_open();
    window_clear(s_window);
    u2p_ui_set_redraw(dumper_rebuild, &d);

    cart_dumper_open(&d);
    ui_run(&d, dumper_poll);
    cart_dumper_close(&d);

    u2p_ui_set_redraw(NULL, NULL);
    cart_view_set(NULL);
    window_close(s_window);
    s_window = NULL;
}

// =========================================================================
//  The inspector
// =========================================================================

// The hex viewer draws its own window: it shows bus bytes, it has no bank map and no progress, so there is
// nothing for the event side to do. It asks through the same dialogs as the other two (the session may have
// something to say), so the view is installed around it.
static void inspector_event(void *ctx, const cart_event_t *ev)
{
    (void)ctx;
    (void)ev;
}

static int inspector_poll(void *obj, int key) { return cart_inspector_poll((cart_inspector_t *)obj, key); }

void cart_view_text_inspector(void)
{
    cart_inspector_t t;
    cart_view_t view = { inspector_event, view_ask, &t };
    cart_view_set(&view);
    cart_inspector_init(&t);
    ui_run(&t, inspector_poll);
    cart_inspector_deinit(&t);
    cart_view_set(NULL);
}
