// The picture under the base's Timewarp page as the ring library exports it (U18 T-3; MultiCore T256 D11, Alex's answer A): the export "ringstill", major 1
// (twstill.cc, lib_ring.cc). The ring library owns that picture: a surface with the frame the machine drew last and Timewarp's bar, shown as the overlay's
// still. When the page comes and goes is the core hook menu_video of the trx64u library (lib_trx64u.cc), which reads this table with
// api->lib->find(RING_STILL_EXPORT, U2A3_VERSION(RING_STILL_MAJOR, 0), &size) and calls a member only when both that size and the table's own `size` cover
// it and it is set. `size` first, members only appended within the major. C and C++.
#ifndef RING_STILL_H
#define RING_STILL_H

#include <stdint.h>

#define RING_STILL_EXPORT "ringstill"
#define RING_STILL_MAJOR 1

#ifdef __cplusplus
extern "C" {
#endif

typedef struct RingStill {
    uint32_t size;
    // The page came up (behind 1) or went (behind 0): menu_video's argument, on the task that draws the menu, without the core lock. With 1, rgb is the
    // palette the person set (16 colours, RGB, 8 bits each), whose colours the bar is painted in; it is read during the call only. Ignored with 0.
    void (*page)(int behind, const uint8_t rgb[16][3]);
} RingStill;

#ifdef __cplusplus
}
#endif

#endif
