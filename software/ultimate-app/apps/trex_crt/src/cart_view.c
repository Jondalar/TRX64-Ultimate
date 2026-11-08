/*
 * cart_view.c
 *
 *  The installed view and the calls the core makes into it (T95 D5). No view: the reports go nowhere and the
 *  questions take the answer that changes nothing.
 */

#include "cart_view.h"

#include "cart_port.h" // FRESULT

const cart_view_t *g_view;

void cart_view_set(const cart_view_t *v) { g_view = v; }

static void emit(const cart_event_t *ev)
{
    if (g_view && g_view->event) {
        g_view->event(g_view->ctx, ev);
    }
}

static int ask(const cart_ask_t *q)
{
    if (g_view && g_view->ask) {
        return g_view->ask(g_view->ctx, q);
    }
    return q->kind == CART_ASK_NOTIFY ? 1 : (q->kind == CART_ASK_SAVE ? FR_DENIED : -1);
}

void cart_status(const char *msg)
{
    cart_event_t ev = { CART_EV_STATUS, msg, NULL, NULL, 0, 0 };
    emit(&ev);
}

void cart_banks(const uint8_t *lo, const uint8_t *hi)
{
    cart_event_t ev = { CART_EV_BANKS, NULL, lo, hi, 0, 0 };
    emit(&ev);
}

void cart_header(void)
{
    cart_event_t ev = { CART_EV_HEADER, NULL, NULL, NULL, 0, 0 };
    emit(&ev);
}

void cart_redraw(void)
{
    cart_event_t ev = { CART_EV_REDRAW, NULL, NULL, NULL, 0, 0 };
    emit(&ev);
}

void cart_progress(int done, int total)
{
    cart_event_t ev = { CART_EV_PROGRESS, NULL, NULL, NULL, done, total };
    emit(&ev);
}

void cart_result(int res, const char *msg)
{
    cart_event_t ev = { CART_EV_RESULT, msg, NULL, NULL, res, 0 };
    emit(&ev);
}

void cart_tell(const char *msg)
{
    cart_ask_t q = { CART_ASK_NOTIFY, msg, NULL, 0, NULL, 0, NULL, NULL, NULL, NULL, 0 };
    ask(&q);
}

bool cart_confirm(const char *msg)
{
    cart_ask_t q = { CART_ASK_CONFIRM, msg, NULL, 0, NULL, 0, NULL, NULL, NULL, NULL, 0 };
    return ask(&q) == 1;
}

int cart_choice(const char *title, const char *const *items, int n)
{
    cart_ask_t q = { CART_ASK_CHOICE, title, items, n, NULL, 0, NULL, NULL, NULL, NULL, 0 };
    return ask(&q);
}

bool cart_text(const char *title, char *buf, int size)
{
    cart_ask_t q = { CART_ASK_TEXT, title, NULL, 0, buf, size, NULL, NULL, NULL, NULL, 0 };
    return ask(&q) > 0;
}

bool cart_path(char *buf, int size)
{
    cart_ask_t q = { CART_ASK_PATH, NULL, NULL, 0, buf, size, NULL, NULL, NULL, NULL, 0 };
    return ask(&q) > 0 && buf[0] != 0;
}

int cart_save_file(const char *prompt, const char *ext, const char *path, void **fh, char *name, int namesize,
                   char *fname, int fnamesize)
{
    cart_ask_t q = { CART_ASK_SAVE, prompt, NULL, 0, name, namesize, path, ext, fh, fname, fnamesize };
    return ask(&q);
}
