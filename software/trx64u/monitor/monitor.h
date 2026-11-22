// The machine code monitor of F4 in the monitor library (U18 D5): trxmon's own engine and screen model (mon.c, mach.c, ring.c, trace.c, rpc.c, ui.c, ...;
// software/ultimate-app/apps/trxmon/src, one source with the firmware, D4) and the session the menu's monitor tab and the RPC clients share. monitor.cc
// gives trxmon what the firmware's app loader gave it, the trx table with the frame ring; lib_monitor.cc gives it the platform calls (time, files, settings)
// and registers the operations below. The verbs, their texts, the keys of the line editor and the rows are trxmon's; nothing of them is written here.
#ifndef TRX64U_MONITOR_H
#define TRX64U_MONITOR_H

#include <stdint.h>

// The monitor's screen is entered (the textgrid's enter, lib_monitor.cc): the session starts (once, until mon_release()).
void mon_open(void);
// A key typed while the monitor has the focus (every key is the monitor's). True when the monitor took it.
bool mon_key(int k);
// Does RUN/STOP belong to the monitor now? While the machine runs it pauses it (trxmon's ui.c); when it stands, RUN/STOP closes the menu.
bool mon_wants_stop(void);
// `x` ran (resume and leave): read once.
bool mon_leave_requested(void);
// The line `diag` was typed: the menu goes to Diagnostics (read once; the monitor session ends).
bool mon_diag_requested(void);
// The library's poll (monitor.serve): the RPC server on TCP 4312 (monsrv.h) and what its clients ask. The session is claimed only when a client sends a
// command.
void mon_serve(void);
// An RPC client sent a command: the engine is needed now (monsrv.cc calls this before rpc_request). False: the debug block is not there.
bool mon_claim(void);
// Called each pass of the menu loop while the screen is shown (the textgrid's tick): the stops that came, the run-tos that finished, the live log. True
// when the screen changed.
bool mon_tick(void);
// The menu closes (any way): the debugger's hold is given back, breakpoints and observers are disarmed, the next open starts a new session. The
// operation monitor.release (lib_monitor.cc), which the base library's core hook menu_closed calls.
void mon_release(void);
// The window, the export "textgrid" (lib_monitor.cc) the base draws for a node with the layout "terminal": mon_render() builds the rows of the screen as
// ui.c has them now; the accessors read them.
void mon_render(void);
int mon_cols(void);
int mon_rows(void);
const char *mon_line(int r);
int mon_attr(int r);                    // UA_* of trxmon's ui.h
int mon_blank_rows(void);
void mon_cursor(int *x, int *y);
uint32_t mon_sig(void);                 // a hash of what mon_render() built

// A command run as if typed.
void mon_run(const char *line);

#endif
