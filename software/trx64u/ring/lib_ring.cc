// The ring library's entry (U18 D5): its operations and state values (ringops.cc), its poll (ring.poll every TRX_RING_POLL_MS, not while the
// machine is frozen) and the two tables it exports (ring_api.cc): "ring", the firmware's ring table, and "ringsteps", the dumps in steps. The engine
// does not start here: the api's groups (sync, io, ctx, ...) are null while lib_register runs, so the core's start hook starts it through the operation
// ring.init; the log starts from the poll.
#include "ringsys.h"
#include "ringops.h"
#include "ring_steps.h"
#include "trx_ring.h"
#include "libfmt.h"

const LibApi *g_api;

const void *ring_table(uint32_t *size);
const void *ring_steps_table(uint32_t *size);

// The named exports (LibRecord::export_list); the Makefile's LIB_EXPORTS lists the same names and majors, and the loader refuses the library
// when lib_register does not return each of them. Filled in lib_register: the tables and their sizes are ring_api.cc's.
static LibExportDecl s_exports[2];

// lib.ld puts this section first, so the entry is at offset 0 of the image
#ifdef __riscv
#define LIB_ENTRY_ATTR __attribute__((visibility("default"), section(".text.lib_register")))
#else
#define LIB_ENTRY_ATTR
#endif
extern "C" LIB_ENTRY_ATTR int lib_register(const LibApi *api, LibRecord *out)
{
    if (api->abi_major != U2A_ABI_MAJOR || !api->sys || !api->mem) {
        return 1;
    }
    g_api = api;
    ringops_record(out);
    out->poll.op = "ring.poll";
    out->poll.period_ms = TRX_RING_POLL_MS;
    out->poll.when_frozen = false;
    s_exports[0].name = "ring";
    s_exports[0].major = 1;
    s_exports[0].table = ring_table(&s_exports[0].size);
    s_exports[1].name = RING_STEPS_EXPORT;
    s_exports[1].major = RING_STEPS_MAJOR;
    s_exports[1].table = ring_steps_table(&s_exports[1].size);
    out->export_list = s_exports;
    out->nexport_list = 2;
    return 0;
}
