/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * trxsvc.h — the monitor as the firmware's debug service (U5).
 *
 * The firmware compiles these sources in and runs them as a task of its own
 * (software/io/c64/trx_service.cc). The service owns the debug block from boot:
 * breakpoints and observers outlive every app run and every menu, and the
 * JSON-RPC port answers whether or not anything is loaded.
 *
 * The api table is the firmware's own, not an app's: see trx_service.cc.
 */
#ifndef TRXMON_TRXSVC_H
#define TRXMON_TRXSVC_H

struct u2p_api_s;

#ifdef __cplusplus
extern "C" {
#endif

/* Takes the table, the debug block and the machine; 0, or -1 when this
 * bitstream has no debug block or the firmware no net table. */
int trxsvc_init(const struct u2p_api_s *api);

/* Opens TCP 4312 when it can; 1 once it listens. trxsvc_serve() keeps trying. */
int trxsvc_listen(void);

/* The service loop. Never returns. */
void trxsvc_serve(void);

/* 4312 while the service listens, 0 before that. */
int trxsvc_rpc_port(void);

/* The monitor screen, with the service loop parked. Runs in the UI task only:
 * through trx->ui_request after a stop, or straight from a menu key. Returns
 * what the session returned, -1 when the loop did not park in time. */
int trxsvc_screen(void);

/* Timewarp's C= + O (U4): stand on that ring entry. NULL, or the reason. */
const char *trxsvc_land(int index);

/* Whether anything is armed: a breakpoint, an observer, a capture or the drive
 * comparator (rpc.c). The firmware asks before it lets an app have the block. */
int rpc_stops_armed(void);

#ifdef __cplusplus
}
#endif

#endif /* TRXMON_TRXSVC_H */
