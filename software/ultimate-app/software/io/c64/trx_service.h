/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * trx_service.h — the TRX64 debug service in the firmware (U5).
 *
 * The monitor, its JSON-RPC server (TCP 4312) and the machine control are the
 * sources under apps/trxmon/src, compiled into the firmware and run as a task
 * of their own. The service owns the debug block from boot: breakpoints and
 * observers stay armed across app runs, menus and C64 resets, and a stop opens
 * the monitor wherever the person was (docs/specs/U5-observer-service.md).
 */
#ifndef TRX_SERVICE_H
#define TRX_SERVICE_H

class UserInterface;

/* extern "C": tree_browser.cc and trx_timewarp_ui.cc take these weakly, so the
   same sources build for a target that has no debug block and no service. */
extern "C" {

/* Starts the task. Does nothing when the bitstream has no debug block. */
void trx_service_start(void);

/* Whether the service runs (the menu offers the monitor only then). */
bool trx_service_present(void);

/* The monitor screen, from a menu key. Runs on the caller's UI task in a task
 * of its own, as an app's screen does, and returns when the person leaves. */
void trx_service_open(UserInterface *ui);

/* The same, standing on a ring entry first (Timewarp's C= + O, U4). */
void trx_service_open_at(UserInterface *ui, int ring_index);

}

#endif /* TRX_SERVICE_H */
