// The ring library's entry (U18 D5): its operations and state values (ringops.cc), its poll (ring.poll every TRX_RING_POLL_MS, not while the
// machine is frozen) and the ring table it exports (ring_api.cc). The engine does not start here: the api's groups (sync, io, ctx, ...) are null while
// lib_register runs, so the core's start hook starts it through the operation ring.start; the log starts from the poll.
#include "ringsys.h"
#include "ringops.h"
#include "trx_ring.h"
#include "libfmt.h"

const LibApi *g_api;

const void *ring_exports(uint32_t *size);

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
    out->exports = ring_exports(&out->exports_size);
    return 0;
}
