/*
 * u2p_ui.c - see u2p_ui.h
 */

#include "u2p_ui.h"

static const u2p_api_t *s_api;
static u2p_redraw_fn s_redraw;
static void *s_redraw_ctx;

void u2p_ui_init(const u2p_api_t *api) { s_api = api; }

void u2p_ui_set_redraw(u2p_redraw_fn redraw, void *ctx)
{
    s_redraw = redraw;
    s_redraw_ctx = ctx;
}

void u2p_ui_refresh(void)
{
    if (s_redraw)
        s_redraw(s_redraw_ctx);
}

void u2p_ui_popup(const char *title, const char *msg)
{
    s_api->ui->popup(title, msg);
    u2p_ui_refresh();
}

int u2p_ui_popup_buttons(const char *msg, uint8_t buttons)
{
    int ret = s_api->ui->popup_buttons(msg, buttons);
    u2p_ui_refresh();
    return ret;
}

int u2p_ui_confirm(const char *msg)
{
    int ret = s_api->ui->confirm(msg);
    u2p_ui_refresh();
    return ret;
}

int u2p_ui_menu(const char *title, const char **items, int n)
{
    int ret = s_api->ui->menu(title, items, n);
    u2p_ui_refresh();
    return ret;
}

int u2p_ui_menu_keys(const char *title, const char **items, int n, const char *keys)
{
    int ret = s_api->ui->menu_keys(title, items, n, keys);
    u2p_ui_refresh();
    return ret;
}

int u2p_ui_string_input(const char *title, char *buf, int maxlen)
{
    int ret = s_api->ui->string_input(title, buf, maxlen);
    u2p_ui_refresh();
    return ret;
}

int u2p_ui_path_input(char *buf, int maxlen)
{
    int ret = s_api->ui->path_input(buf, maxlen);
    u2p_ui_refresh();
    return ret;
}
