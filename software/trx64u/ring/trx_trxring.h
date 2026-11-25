// PROVENANCE: the firmware fork TREX/1541ultimate:software/io/c64/trx_trxring.h, ported for the ring library (U18 D6: this copy is the master); the
// branch for products without the ring is gone. The code is trxring.cc.
// .trxring - the frame ring itself in a file, this board only
// (TRX64-Ultimate docs/specs/T20-delta-ring.md §10.2, T66 part 6). A .c64re
// carries one machine state; this carries the whole ring, so after a load you
// can rewind through the same seconds again.
//
// What is in it, and why that is enough: the app's timeline (one entry per
// frame boundary with its chip block) and the undo data of streams 0 and 1,
// plus the memories at the newest boundary. Stream 2 is not in the file - it
// holds the markers and blocks the parse has already turned into the timeline,
// and after a load the engine writes its own from count 0 on.
#ifndef TRX_TRXRING_H
#define TRX_TRXRING_H

#include <stdint.h>
#include "trx_c64re.h"      // trx_progress_fn

// Writes the ring as it is now. *bytes gets the file's size. The media it names (and copies beside the file) are the disk in drive A's slot and the
// cartridge's .crt, from the source the call builds (ringmachine.cc) or the one set (trx_c64re_set_source).
const char *trx_trxring_save(const char *path, uint32_t *bytes, trx_progress_fn progress, void *ctx);

// Is this a .trxring at all, and one this build can load? NULL, or what is
// wrong with it. Cheap: the header only, nothing is kept.
const char *trx_trxring_check(const char *path);

// Reads it and puts the ring and the machine back. The machine must be the
// caller's to stop (the monitor, an operation); from a menu that puts its own
// state back when it closes use the two below.
const char *trx_trxring_load(const char *path);

// The deferred load: the path is kept (checked by the caller first) and the
// menu's close asks for it. Only the name is held, not the file: a .trxring can
// be tens of megabytes.
void trx_trxring_set_pending(const char *path);
bool trx_trxring_pending(void);
void trx_trxring_unfreeze(void);

// Does this file start with the .trxring magic? (trx_c64re_load_entry() sends
// such a file here, so one path loads whichever kind it is.)
bool trx_trxring_is_one(const char *path);

#endif
