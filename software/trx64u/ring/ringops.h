// Timewarp as operations and state values (ringops.cc): the tables lib_register hands out; the picture under the Timewarp page (twstill.cc).
#ifndef RING_RINGOPS_H
#define RING_RINGOPS_H

#include "libapi.h"
#include "twbar.h"

// Fills the record's operations and state values.
void ringops_record(LibRecord *out);

// What the bar shows now (the state values tw.glyph, tw.pos, tw.info and tw.track, the page's hint); pos and info are written into the buffers given.
void ringops_bar(TwBar *b, char *pos, int poslen, char *info, int infolen);

// A scrub step put the machine on another frame: while the Timewarp page shows, the still under it is painted again from the frame the machine drew last
// with the bar's new state, and the base draws it again (twstill.cc). Nothing while the page is not up.
void tw_still_scrubbed(void);

// The export "ringstill" (ring_still.h) and its size, for lib_register (lib_ring.cc).
const void *ring_still_table(uint32_t *size);

#endif
