/*
 * u2p_ui.h - U64 App SDK: dialogs that give the app its screen back.
 *
 * A firmware dialog (menu, string box, path picker, popup) restores the
 * screen it found when it closes. That is not necessarily the app's screen:
 * a dialog opened right after another, or over a window the app has redrawn
 * since, leaves pieces of the view before it behind.
 *
 * An app registers one redraw function. Every dialog below calls it once the
 * dialog has closed, so the app draws its whole screen again. The redraw
 * function should start from an empty screen: api->ui->screen_open() clears
 * all of it, so close the app's window and open it again, then draw.
 *
 * Add $(SDK)/lib/u2p_ui.c to SRCS in the app Makefile.
 */

#ifndef U2P_UI_H
#define U2P_UI_H

#include "u2p_app.h"

typedef void (*u2p_redraw_fn)(void *ctx);

/* Once, before any other call here */
void u2p_ui_init(const u2p_api_t *api);

/* The function that draws the app's whole screen; NULL when it has none
 * (before its window is open, and after it is closed) */
void u2p_ui_set_redraw(u2p_redraw_fn redraw, void *ctx);

/* Draws the app's screen again, as the dialogs below do when they close */
void u2p_ui_refresh(void);

/* The dialogs of u2p_ui_api_t, same arguments and results */
void u2p_ui_popup(const char *title, const char *msg);
int u2p_ui_popup_buttons(const char *msg, uint8_t buttons);
int u2p_ui_confirm(const char *msg);
int u2p_ui_menu(const char *title, const char **items, int n);
int u2p_ui_menu_keys(const char *title, const char **items, int n, const char *keys);
int u2p_ui_string_input(const char *title, char *buf, int maxlen);
int u2p_ui_path_input(char *buf, int maxlen);

#endif
