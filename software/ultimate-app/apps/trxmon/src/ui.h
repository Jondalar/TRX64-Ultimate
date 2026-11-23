/*
 * ui.h — the monitor screen's model (T21-monitor.md §8): the TRX64 verbs on the
 * overlay, 80 x 40 where the firmware can make the overlay that wide (ui->screen_open_wide,
 * §8.10), else the 40 x 23 window of screen_open_full. No drawing and no firmware calls here:
 * ui_render builds the rows, main.c puts them on the window, the host tests compare them as text.
 */
#ifndef TRXMON_UI_H
#define TRXMON_UI_H

#include "mlib.h"

#define UI_COLS_MAX    80
#define UI_ROWS_MAX    50
#define UI_NARROW_COLS 40
#define UI_NARROW_ROWS 23 /* screen_open_full: the overlay's 25 rows less the two the firmware keeps */
#define UI_WIDE_COLS   80
#define UI_WIDE_ROWS   40 /* what trxmon asks screen_open_wide for */
#define UI_HIST        200
#define UI_CMD         120
#define UI_CHIST       32
#define UI_SHRUNK_ROWS 6 /* narrow, while the machine runs: status, registers x2, output x2, command */
#define UI_WIDE_SHRUNK_ROWS 9 /* wide: three header rows, separator, output x2, separator, command, hint */
#define UI_LIVE_EXTRA_ROWS  7 /* more output rows while the machine runs and a log observer is on (the live log) */

/* Keys the screen understands, after main.c has mapped the firmware's codes (physical F1/F7/F3
 * are what the person pressed, whatever the interface makes of them) */
#define UI_K_PGUP   0x2001
#define UI_K_PGDN   0x2002
#define UI_K_HELP   0x2003
#define UI_K_STOP   0x2004 /* RUN/STOP */
#define UI_K_UP     0x2005
#define UI_K_DOWN   0x2006
#define UI_K_RETURN 0x2007
#define UI_K_BACK   0x2008
#define UI_K_KILL   0x2009 /* the C64's left arrow: delete the line */
#define UI_K_LEFT   0x200A /* the cursor moves within the input line (T55) */
#define UI_K_RIGHT  0x200B
#define UI_K_HOME   0x200C
#define UI_K_END    0x200D
#define UI_K_COMPLETE 0x200E /* CTRL alone or CTRL+I on the C64's keyboard, TAB on a USB one: complete the word */

/* How a row is coloured */
enum { UA_NORMAL = 0, UA_STATUS, UA_REGS, UA_ECHO, UA_ERR, UA_PROMPT, UA_BLANK, UA_SEP, UA_HINT };

enum { UI_STAY = 0, UI_LEAVE_X, UI_LEAVE_QUIT };

typedef struct
{
    char row[UI_ROWS_MAX][UI_COLS_MAX + 1];
    uint8_t attr[UI_ROWS_MAX];
    int cols, rows; /* the window's size: row[0..rows-1], each cols wide */
    int cursor_x, cursor_y;
    int blank_rows; /* rows 0..blank_rows-1 are colour index 0: transparent on the overlay */
} ui_screen_t;

/* The rows of the screen that read the machine, as ui_render draws them: the status row, the registers (narrow two
 * rows, wide one) and the wide device row; whether the machine ran and a log observer was on (the window shrinks) */
typedef struct
{
    int running, live;
    char status[UI_COLS_MAX + 1];
    char regs[2][UI_COLS_MAX + 1];
    char info[UI_COLS_MAX + 1];
} ui_mrows_t;

/* A platform whose screen calls may not reach the machine (the monitor library, U18: its keys, tick and render come
 * on the menu task without the core lock, its poll holds the lock) hands the screen this (ui_set_polled). ui_key then
 * only queues the line it would run (RETURN, the help key, RUN/STOP's pause) and ui_render draws the machine's rows
 * from `rows`; the platform's poll calls ui_poll, which runs the line, ticks and reads the rows again. */
typedef struct
{
    char line[UI_CMD + 1];
    int queued; /* `line` waits for ui_poll */
    ui_mrows_t rows;
} ui_polled_t;

typedef struct
{
    int cols, rows; /* the window: UI_NARROW_* or UI_WIDE_* (ui_set_size) */
    int fk_up, fk_down, fk_help; /* the function keys (1..8) that page up, page down and help (ui_set_keys) */
    char hist[UI_HIST][UI_COLS_MAX + 1]; /* a ring; hn lines, the newest at (hhead - 1) */
    uint8_t hattr[UI_HIST];
    int hn, hhead;
    int scroll; /* lines up from the newest */
    char cmd[UI_CMD + 1];
    int clen;
    int cpos; /* the cursor in cmd, 0..clen */
    char chist[UI_CHIST][UI_CMD + 1];
    int cn;      /* entries */
    int ci;      /* recalled entry, -1 none */
    char last_step[UI_CMD + 1];
    char run_desc[24]; /* "until $c100", "next", "ret" while a run-to waits */
    int was_run;
    int leave; /* UI_STAY, UI_LEAVE_X, UI_LEAVE_QUIT */
    uint32_t olog_cur; /* the next observer log line to show live (mach_olog_next) */
    ui_polled_t *pl; /* null: the screen's calls read and run the machine themselves (ui_set_polled) */
} ui_t;

void ui_init(ui_t *u);
/* The window the screen got: 80 columns and `rows` when the overlay was made wide, else 40 x 23. Lines
 * printed from now on wrap at `cols`; `m` and `d` page to the output area. */
void ui_set_size(ui_t *u, int cols, int rows);
/* The rows of the output area when the window has its full size (what one page of `m` or a PageUp is) */
int ui_out_rows(const ui_t *u);
/* A session begins (the monitor was opened): the line is empty, the view at the newest output */
void ui_open(ui_t *u);
/* Adds text to the output: lines split at \n and wrapped at the window's width, address marks dropped,
 * bytes the overlay cannot show replaced */
void ui_print(ui_t *u, const char *text, int attr);
/* The stop that put the screen up (a breakpoint or observer fired while it was closed) */
void ui_show_stop(ui_t *u);
/* A firmware key code (what getch / getch_monitor return: the keymapper's) as a screen key: a char
 * (0x20..0x7e), a UI_K_*, or 0 for a key the screen ignores (Tasks, the other function keys). The firmware
 * names the keys by what they do (KEY_PAGEUP, KEY_PAGEDOWN, KEY_HELP, KEY_TASKS) and its interface decides
 * which function key carries which: F1 F3 F5 F7 in the Ultimate interface, F3 F7 F1 F5 in the Commodore
 * one. The screen follows the names, so the key the interface labels "page up" scrolls. */
int ui_map_key(int k);
/* Which function keys the interface made PageUp, PageDown and Help (ui->fkey(1..8): the codes F1..F8 give);
 * the hint row and the opening text name them. Defaults: F1, F7, F3. */
void ui_set_keys(ui_t *u, const int *fkey);
/* One key. Returns UI_STAY, UI_LEAVE_X (`x` ran) or UI_LEAVE_QUIT (`/quit` ran); with ui_set_polled the line runs
 * in ui_poll, and u->leave says so after it. */
int ui_key(ui_t *u, int key);
/* After rpc_poll: output of a run-to that finished, the stop text of a stop nobody waited for.
 * Returns 1 when something was printed. With ui_set_polled only ui_poll calls it. */
int ui_tick(ui_t *u);
/* The rows as they are drawn now: the machine's state (M) is read here; with ui_set_polled its rows come from
 * the last ui_poll and nothing of the machine is read */
void ui_render(const ui_t *u, ui_screen_t *s);

/* ---- the screen run from the platform's poll (ui_polled_t) ------------- */
/* p: the screen's calls queue their lines and draw the machine's rows from p (cleared here); null: they run and
 * read the machine themselves (the default) */
void ui_set_polled(ui_t *u, ui_polled_t *p);
/* The platform's poll, holding what the machine needs: the queued line runs, a ring file job of the screen takes
 * its step (ring_job_poll), ui_tick, the machine's rows are read again. Returns 1 when the screen changed (a line
 * ran, output came, the machine's rows differ). */
int ui_poll(ui_t *u);
/* 1: a line waits for ui_poll */
int ui_waiting(const ui_t *u);
/* The screen goes: the line that waits is dropped, without a text */
void ui_drop(ui_t *u);

#endif
