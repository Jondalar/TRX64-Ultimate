/*
 * cart_view.h
 *
 *  What the cartridge tools tell their user interface, and what they ask it (T95 D5).
 *
 *  The core (cart_base, cart_dumper, cart_writer) holds no window, no key code and no dialog. It reports with
 *  cart_event_t and asks with cart_ask_t, and a view answers. cart_view_text.c is the 40 x 22 character screen
 *  the tools have today; MultiCore's Generic App UI renders the same events in LVGL (T104.3) without a second
 *  copy of the tool.
 *
 *  Without an installed view every report is dropped and every question takes its safe answer (NOTIFY passes,
 *  everything else cancels), so the core runs headless in the host harness.
 */

#ifndef CART_VIEW_H
#define CART_VIEW_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    CART_EV_STATUS = 0, // text: the status line, the one sentence about what just happened
    CART_EV_BANKS,      // lo, hi: the bank map of the two chips changed
    CART_EV_HEADER,     // the cartridge, the EF3 slot or the file line changed: the view reads the tool again
    CART_EV_REDRAW,     // everything is stale (a dialog was over the screen): the view draws itself anew
    CART_EV_PROGRESS,   // a of b done inside the running operation (banks, slots)
    CART_EV_RESULT      // a: the FRESULT the operation ended with, text: what to say about it
} cart_ev_kind_t;

typedef struct
{
    cart_ev_kind_t kind;
    const char *text;
    const uint8_t *lo, *hi; // CART_EV_BANKS: MAX_CART_BANKS bytes each, BANK_* values
    int a, b;
} cart_event_t;

typedef enum {
    CART_ASK_NOTIFY = 0, // one message, one button; the answer is always 1
    CART_ASK_CONFIRM,    // yes or no; 1 = yes
    CART_ASK_CHOICE,     // one of choices[0..nchoices-1]; the index, or -1
    CART_ASK_TEXT,       // buf holds the default and takes what was typed; 1 = typed, <= 0 = cancelled
    CART_ASK_PATH,       // buf takes a directory the user picks; 1 = picked, <= 0 = cancelled
    CART_ASK_SAVE        // a file to write: buf holds the default name, path is the directory, ext the extension.
                         // The view asks for the name, asks before it overwrites, creates the file and puts it
                         // in *fh; the answer is the FRESULT. buf comes back as the name the user gave, fname
                         // as the file name it became on the medium (fixed, with the extension).
} cart_ask_kind_t;

typedef struct
{
    cart_ask_kind_t kind;
    const char *prompt;
    const char *const *choices; // CHOICE
    int nchoices;
    char *buf; // TEXT, PATH, SAVE
    int bufsize;
    const char *path, *ext; // SAVE
    void **fh;              // SAVE: the open file
    char *fname;            // SAVE: the file name as it was created
    int fnamesize;
} cart_ask_t;

typedef struct
{
    void (*event)(void *ctx, const cart_event_t *ev);
    int (*ask)(void *ctx, const cart_ask_t *q);
    void *ctx;
} cart_view_t;

extern const cart_view_t *g_view;

void cart_view_set(const cart_view_t *v);

// ---- what the core reports -------------------------------------------------------------------------------
void cart_status(const char *msg);
void cart_banks(const uint8_t *lo, const uint8_t *hi);
void cart_header(void);
void cart_redraw(void);
void cart_progress(int done, int total);
void cart_result(int res, const char *msg);

// ---- what the core asks ----------------------------------------------------------------------------------
// One message with one button. cart_base's cart_notify() is this plus the log line.
void cart_tell(const char *msg);
bool cart_confirm(const char *msg);
int cart_choice(const char *title, const char *const *items, int n);
bool cart_text(const char *title, char *buf, int size);
bool cart_path(char *buf, int size);
int cart_save_file(const char *prompt, const char *ext, const char *path, void **fh, char *name, int namesize,
                   char *fname, int fnamesize);

#endif
