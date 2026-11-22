// The dumps in steps (trx_c64re.h trx_dump_*) as the ring library exports them (U18): the export "ringsteps", major 1 (ring_api.cc, lib_ring.cc), a
// table of its own beside the export "ring", which is the firmware's u2p_ring_api_t (app_api.h) and nothing else. A reader takes it with
// api->lib->find(RING_STEPS_EXPORT, U2A3_VERSION(RING_STEPS_MAJOR, 0), &size) and uses a member only when both that size and the table's own `size`
// cover it. `size` first, members only appended within the major. Read by the monitor library (monitor.cc), whose poll holds the core lock one step at a
// time; C and C++.
#ifndef RING_STEPS_H
#define RING_STEPS_H

#include <stdint.h>

#define RING_STEPS_EXPORT "ringsteps"
#define RING_STEPS_MAJOR 1

#ifdef __cplusplus
extern "C" {
#endif

// begin's kind: the values of TRX_DUMP_FRAME, TRX_DUMP_RING and TRX_DUMP_MACHINE (ring_api.cc checks them)
enum { RING_STEPS_FRAME = 0, RING_STEPS_RING = 1, RING_STEPS_MACHINE = 2 };

// begin and every step read registers: the caller holds the core lock for each call. Between two steps the machine stays as it is; a ring dump whose
// timeline changed between two steps fails. drop touches no register.
typedef struct RingSteps {
    uint32_t size;
    // FRAME: timeline entry `arg` as a .c64re of format 2 (stride 0); RING: the newest `arg` seconds, one entry in every `stride` frames, as a .c64rering;
    // MACHINE: the machine as it stands as a .c64re of format 1 (arg and stride 0). Builds the machine the file describes and writes nothing yet. Null
    // and *err set when there is no memory or the path does not fit.
    void *(*begin)(int kind, const char *path, int arg, int stride, const char **err);
    // One piece of the file: 1 more to do, 0 the file is complete, -1 failed (*err says why, the partial file is gone).
    int (*step)(void *dump, const char **err);
    int (*pct)(const void *dump);               // 0-100 for a progress display
    uint32_t (*bytes)(const void *dump);        // once step answered 0: the file's size
    int (*entries)(const void *dump);           // once step answered 0: the entries of a .c64rering
    // Ends the dump at any point and removes a file that is not complete.
    void (*drop)(void *dump);
} RingSteps;

#ifdef __cplusplus
}
#endif

#endif
