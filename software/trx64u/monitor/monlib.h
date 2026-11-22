// What lib_monitor.cc gives the other files of the monitor library (U18). monsrv.cc reads the base's tables itself (g_api, libapi.h); monitor.cc cannot: it
// builds trxmon's trx table from the firmware's application ABI (app_api.h), and the SDK's svctab.h (libapi.h) defines u2p_video_t and the ring records a
// second time, so the two headers never meet in one file. The calls below carry the base's trx table and the ring library's table across in types both
// sides read.
#ifndef TRX64U_MONLIB_H
#define TRX64U_MONLIB_H

#include <stdint.h>

struct LibApi;
extern const LibApi *g_api;             // what lib_register was given: the one pointer to the base's tables

// The members of the base's trx table (api->trx, SvcTrx) whose types both headers spell alike.
struct MonTrxBase {
    volatile uint8_t *regs;
    uint32_t caps;
    int (*acquire)(uint32_t flags);
    void (*release)(void);
    void (*announce)(uint16_t rpc_port);
};
// False: the base has no trx table (the core's ID block does not read "TRX2").
bool monlib_trx(MonTrxBase *t);
// api->trx->video into v, a u2p_video_t: svctab.h and app_api.h lay it out alike (20 bytes; lib_monitor.cc and monitor.cc both assert it). < 0: the base has
// no such call.
int monlib_trx_video(void *v);
// The ring library's table, api->lib->find("ring", 1): the firmware's u2p_ring_api_t (app_api.h), *size bytes of it. Null: no ring library.
const void *monlib_ring(uint32_t *size);
// One line into the base's log, without a newline (the base prints "[lib] " before it and a newline after it).
void monlib_log(const char *text);

#endif
