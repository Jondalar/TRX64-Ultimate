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

// progress(done, total, ctx): done 0 once the state is gathered, then after
// every chunk written.
typedef void (*trx_progress_fn)(int done, int total, void *ctx);

// Writes timeline entry idx as a .c64re of format 2 (U8): a binary section
// table, streamed, with the cartridge image as it stands at the freeze, once.
// NULL, or what went wrong - and then no file is left behind.
const char *trx_c64re_dump(const char *path, int idx, uint32_t *bytes);
const char *trx_c64re_dump_ex(const char *path, int idx, uint32_t *bytes,
                              trx_progress_fn progress, void *ctx);

// Where a state file belongs and what to name it after: the folder of the
// cartridge or disk that is running, else a timewarp/ folder on the first
// volume there is (U8 §5). False when there is nowhere to write at all.
bool trx_c64re_place(char *dir, int dirlen, char *base, int baselen);

// Writes the newest "seconds" of the ring, one entry in every "stride"
// frames (oldest first, the newest entry is the cursor), as a .c64rering.
const char *trx_c64rering_dump(const char *path, int seconds, int stride, uint32_t *bytes, int *entries);

// The same, calling progress(done, total, ctx) once the memories are read
// (done 0) and after every entry written. Format 1; no menu entry writes one
// any more (U8 §7), reading stays.
const char *trx_c64rering_dump_ex(const char *path, int seconds, int stride, uint32_t *bytes, int *entries,
                                  trx_progress_fn progress, void *ctx);

// Loads a .c64re into the machine: format 2 exactly, or a format 1 file with
// our trx64Hw node exactly, or one of trx64's coarsely (T20 §9.4).
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
// The same with a progress box while the file is read (U9): a .c64re is over a
// megabyte and the wait needs the same bar the save has.
const char *trx_c64re_stage_ex(const char *path, TrxC64reStaged **out,
                               trx_progress_fn progress, void *ctx);
// The same, naming which entry of a `.c64rering` to take (T66 part 3): NULL or
// "" is the cursor the file carries (`current_id`), else an entry's id
// ("cp_<frame>_<seq>") or label, a frame number, or "#N" for the Nth entry in
// the file (0 = oldest). A `.c64re` holds one state and ignores it.
const char *trx_c64re_stage_entry(const char *path, const char *entry, TrxC64reStaged **out);
// Puts a staged state into the machine and lets it run; frees it either way.
// NULL, or what the hardware said.
const char *trx_c64re_apply(TrxC64reStaged *st);
// U9 D6: what the last load could not put back, or NULL. The browser shows it
// after the machine is running again.
const char *trx_c64re_missed(void);
// Break a message into lines that fit the overlay: UIPopup widens its box to the
// longest line and centres it, so a path in one line runs off the screen.
void trx_c64re_wrap(char *dst, int cap, const char *text, int cols);
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

typedef void (*trx_progress_fn)(int done, int total, void *ctx);
static inline const char *trx_c64re_dump(const char *, int, uint32_t *) { return "No frame ring"; }
static inline const char *trx_c64re_dump_ex(const char *, int, uint32_t *, trx_progress_fn, void *)
{ return "No frame ring"; }
static inline const char *trx_c64rering_dump(const char *, int, int, uint32_t *, int *) { return "No frame ring"; }
static inline const char *trx_c64re_load(const char *) { return "No frame ring"; }
static inline bool trx_c64re_place(char *, int, char *, int) { return false; }
static inline const char *trx_c64rering_dump_ex(const char *, int, int, uint32_t *, int *, trx_progress_fn, void *)
{ return "No frame ring"; }
struct TrxC64reStaged;
static inline const char *trx_c64re_stage(const char *, TrxC64reStaged **out) { *out = 0; return "No frame ring"; }
static inline const char *trx_c64re_stage_ex(const char *, TrxC64reStaged **out, trx_progress_fn, void *) { *out = 0; return "No frame ring"; }
static inline const char *trx_c64re_stage_entry(const char *, const char *, TrxC64reStaged **out) { *out = 0; return "No frame ring"; }
static inline const char *trx_c64re_load_entry(const char *, const char *) { return "No frame ring"; }
static inline const char *trx_c64re_apply(TrxC64reStaged *) { return "No frame ring"; }
static inline const char *trx_c64re_missed(void) { return 0; }
static inline void trx_c64re_wrap(char *dst, int, const char *, int) { dst[0] = 0; }
static inline void trx_c64re_discard(TrxC64reStaged *) { }
static inline void trx_c64re_set_pending(TrxC64reStaged *) { }
static inline bool trx_c64re_pending(void) { return false; }
static inline void trx_c64re_unfreeze(void) { }

#endif

#endif
