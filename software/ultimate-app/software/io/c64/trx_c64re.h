// .c64re and .c64rering files from the TRX64 Ultimate frame ring
// (TRX64-Ultimate docs/specs/T20-delta-ring.md §9, §10; the container and node
// shapes are TRX64's: crates/trx64-core/src/native_snapshot.rs,
// c64re_snapshot.rs, ring_dump.rs). Written with stored deflate blocks (valid
// gzip, no compressor on the RISC-V). Every file carries our own exact state in
// a "trx64Hw" node, which TRX64 ignores and trx_c64re_load() uses.
#ifndef TRX_C64RE_H
#define TRX_C64RE_H

#include <stdint.h>

#if (U64 == 2) && !defined(RECOVERYAPP)

// Writes timeline entry idx as a .c64re. NULL, or what went wrong.
const char *trx_c64re_dump(const char *path, int idx, uint32_t *bytes);

// Writes the newest "seconds" of the ring, one entry in every "stride"
// frames (oldest first, the newest entry is the cursor), as a .c64rering.
const char *trx_c64rering_dump(const char *path, int seconds, int stride, uint32_t *bytes, int *entries);

// The same, calling progress(done, total, ctx) once the memories are read
// (done 0) and after every entry written.
typedef void (*trx_progress_fn)(int done, int total, void *ctx);
const char *trx_c64rering_dump_ex(const char *path, int seconds, int stride, uint32_t *bytes, int *entries,
                                  trx_progress_fn progress, void *ctx);

// Loads a .c64re written by trx_c64re_dump() into the machine (exact: its
// trx64Hw node). A file without it, or compressed (TRX64's own), is refused.
const char *trx_c64re_load(const char *path);
// With the entry selector of trx_c64re_stage_entry().
const char *trx_c64re_load_entry(const char *path, const char *entry);

// The two halves of that load (T66). The file browser needs them apart: the
// machine is frozen while the menu is up, and a load there would be undone by
// the menu's own restore, so the file is read and checked when it is chosen
// (a wrong file says so at once) and put in when the menu closes - the way the
// ring's rewind is deferred (trx_ring_set_pending, C64::unfreeze()).
struct TrxC64reStaged;
// Reads, checks and keeps the state of the file. NULL and *out set, or what
// went wrong (nothing is kept then).
const char *trx_c64re_stage(const char *path, TrxC64reStaged **out);
// The same, naming which entry of a `.c64rering` to take (T66 part 3): NULL or
// "" is the cursor the file carries (`current_id`), else an entry's id
// ("cp_<frame>_<seq>") or label, a frame number, or "#N" for the Nth entry in
// the file (0 = oldest). A `.c64re` holds one state and ignores it.
const char *trx_c64re_stage_entry(const char *path, const char *entry, TrxC64reStaged **out);
// Puts a staged state into the machine and lets it run; frees it either way.
// NULL, or what the hardware said.
const char *trx_c64re_apply(TrxC64reStaged *st);
// Drops a staged state without loading it.
void trx_c64re_discard(TrxC64reStaged *st);

// The browser's deferred load: the staged state is taken over here, and
// C64::unfreeze() asks for it. A second choice replaces the first.
void trx_c64re_set_pending(TrxC64reStaged *st);
bool trx_c64re_pending(void);
// From C64::unfreeze() when trx_c64re_pending(): loads and runs. What fails
// goes to the console - the menu is already gone by then.
void trx_c64re_unfreeze(void);

#else

static inline const char *trx_c64re_dump(const char *, int, uint32_t *) { return "No frame ring"; }
static inline const char *trx_c64rering_dump(const char *, int, int, uint32_t *, int *) { return "No frame ring"; }
static inline const char *trx_c64re_load(const char *) { return "No frame ring"; }
typedef void (*trx_progress_fn)(int done, int total, void *ctx);
static inline const char *trx_c64rering_dump_ex(const char *, int, int, uint32_t *, int *, trx_progress_fn, void *)
{ return "No frame ring"; }
struct TrxC64reStaged;
static inline const char *trx_c64re_stage(const char *, TrxC64reStaged **out) { *out = 0; return "No frame ring"; }
static inline const char *trx_c64re_stage_entry(const char *, const char *, TrxC64reStaged **out) { *out = 0; return "No frame ring"; }
static inline const char *trx_c64re_load_entry(const char *, const char *) { return "No frame ring"; }
static inline const char *trx_c64re_apply(TrxC64reStaged *) { return "No frame ring"; }
static inline void trx_c64re_discard(TrxC64reStaged *) { }
static inline void trx_c64re_set_pending(TrxC64reStaged *) { }
static inline bool trx_c64re_pending(void) { return false; }
static inline void trx_c64re_unfreeze(void) { }

#endif

#endif
