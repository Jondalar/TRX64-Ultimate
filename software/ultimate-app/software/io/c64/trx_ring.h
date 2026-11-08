// The frame ring of the TRX64 Ultimate bitstream (TRX64-Ultimate
// docs/specs/T20-delta-ring.md, ARCH-lean-state-access.md §15.7-§15.9): the
// app side of the engine (trx_engine.v, page 2 of the IO block).
//
// The hardware writes, while the log is on, every C64 RAM write (stream 0),
// every colour RAM and cartridge RAM write (stream 1) and one marker per
// frame with the chip block of that frame's snap (stream 2) into the upper
// DDR2. This module reads stream 2 into a timeline in app memory (one entry
// per frame boundary: its snap position, cycle count, stream positions and
// the 58 state-bus words of its chip block) and puts the machine back on any
// entry the logs still reach: undo of streams 0/1 back to the entry, the
// registers the subset load leaves out replayed as bus cycles, a stop at the
// entry's own snap position, and the CPU and CIA flip-flops loaded from the
// block on that edge (the sequence tb_ring.cpp "restore" plays).
//
// Only the U64-II/C64U application builds this; on a bitstream without the
// engine (CAPS b2) every call refuses and nothing touches the hardware.
#ifndef TRX_RING_H
#define TRX_RING_H

// U9 D4: what a config item shows while a loaded state owns it. set_override()
// is display only (config.h: "the saved value stays"), so the registers are
// written beside it and nothing reaches the flash.
#define TRX_STATE_OVERRIDE "set by the loaded state"


#include <stdint.h>
#include <stddef.h>   // NULL, for the default arguments below

#define TRX_BLOCK_WORDS 58   // the capture table's reset value (c64_core.v map)
#define TRX_CART_RAM_BYTES 65536  // the cartridge RAM window in DDR2 (cart_slot.v: 0xEF0000)

// One frame boundary (the snap that starts frame "seq").
struct TrxFrame {
    uint32_t seq;            // our own number, never reused (the marker numbers restart)
    uint32_t marker;         // F1 frame number
    uint32_t s0, e0;         // stream 0 records of the frame: [s0, e0) (28-bit counts)
    uint32_t s1, e1;         // stream 1, the same
    uint64_t cycles;         // phi2 cycles since the ring started (F2 steps added up)
    uint16_t line, x;        // VIC position of the snap edge (F3)
    uint8_t  flags;          // F1 flags
    uint8_t  bflags;         // B2 flags (b0 torn)
    bool     block;          // a whole chip block came with it
    uint32_t c24;            // F2: the machine's phi2 cycle count after the snap edge, low 24 bits
                             // (the debug block's CYCLES counts the same edges, T21)
    uint32_t w[TRX_BLOCK_WORDS];
};

struct TrxRingStatus {
    bool     present;        // the bitstream has the engine
    bool     on;             // the log runs
    bool     reu_off;        // the REU is enabled: there is no ring (A08, trx_ring_reu_refusal)
    int      frames;         // timeline entries
    int      targets;        // entries a restore can go to
    uint32_t head_seq, oldest_seq;
    uint32_t log_cnt[3];
    uint32_t dropped;
    uint32_t log_stat;
    uint32_t resyncs;        // stream 2 records skipped to find a marker again
    uint32_t spurious_f4;    // F4 records the drain wrote inside a chip block
};

// One instruction of the trace (TRX64's DeltaEntry, 24 bytes; T21 §4.11.1): the registers and PC
// BEFORE it, its opcode and operand bytes (0 for those the opcode does not have), the
// timeline's cycle count of the edge that ended its opcode fetch, and its writes as
// stream 0 records [write_start, write_start + write_count).
struct TrxTraceRec {
    uint64_t cycle;
    uint32_t write_start;
    uint16_t pc;
    uint16_t write_count;
    uint8_t  a, x, y, sp, p, opcode, b1, b2;
};

struct TrxWriteRec {         // TRX64's WriteRec
    uint16_t addr;
    uint8_t  old_value, new_value;
};

struct TrxTraceHit {         // a write of whowrote's address
    uint64_t cycle;
    uint16_t addr, pc;
    uint8_t  old_value, new_value;
    uint16_t flags;          // TRX_HIT_*
    uint32_t index;          // of the writing instruction in the history
};
// Hit flags: a write of an I/O address ($D000-$DFFF) is found from the instruction trace, which carries
// no old byte, and for a read-modify-write no new byte either.
#define TRX_HIT_OLD_UNKNOWN 1
#define TRX_HIT_NEW_UNKNOWN 2
// whowrote's `unresolved` out value: the history holds writes through a zero page pointer ((zp,X), (zp),Y)
// or an SH* store over a page crossing; their address is not in the trace record, so an I/O address scan
// cannot say whether they hit it. Counted, never guessed.

struct TrxTraceStatus {
    bool     present, on, gap;
    uint32_t records;        // in the history
    uint32_t capacity;       // the area holds this many
    uint32_t lost;           // records the engine dropped (DROPPED)
    uint64_t oldest_cycle, newest_cycle;
};

struct TrxRestoreStats {
    uint32_t ms;             // stop to release
    uint32_t ms_stop;        // until the machine stood (on line 0) and the log was read
    uint32_t ms_pos;         // until it stood on the frame's snap position
    uint32_t frames;         // timeline entries undone
    uint32_t rec0, rec1;     // records undone
    uint32_t ram_writes;     // C64 RAM bytes written (the records coalesced per address)
    uint32_t bus_writes;     // register replay and colour RAM, as C64 bus cycles
    uint32_t cart_bytes;     // cartridge RAM bytes put back in DDR2
    uint32_t other;          // stream 1 records of spaces nothing undoes (counted)
    uint32_t next_seq;       // the seq the re-run's first frame boundary gets
    bool     vic_latches;    // the VIC's IRQ and collision latches at the load equal the block's
};

#if (U64 == 2) && !defined(RECOVERYAPP)

// The machine's frame rate in the System Mode that runs now (T40: PAL and
// NTSC-50 are 63 cycles x 312 lines, NTSC and PAL-60 65 x 263), read from
// C64_VIDEOFORMAT each call: whole frames per second for the UI's 1 s / 10 s
// steps, hundredths of a frame per second for time spans, phi2 cycles per frame.
// Tests: 1 the engine applies the stream 0 records (when the bitstream has APPLY), 0 the CPU
// walks them (the older way). Returns whether APPLY is in use after the call.
bool     trx_ring_apply_hw(int mode);
int      trx_ring_fps(void);
uint32_t trx_ring_fps_x100(void);
uint32_t trx_ring_frame_cycles(void);

// The instruction trace (stream 3). present: the bitstream has the former (CAPS2 b6) and the
// ring runs. ctl: on 1/0 sets the trace gate, -1 asks; returns the gate or -1 when not there.
// read: records of the history by index (0 = oldest); writes: stream 0 records by physical
// index (a record's write_start); whowrote: newest first.
bool trx_ring_trace_present(void);
int  trx_ring_trace_ctl(int on);
bool trx_ring_trace_status(TrxTraceStatus *st);
int  trx_ring_trace_read(uint32_t first, uint32_t n, TrxTraceRec *out);
int  trx_ring_trace_writes(uint32_t start, uint32_t n, TrxWriteRec *out);
int  trx_ring_trace_whowrote(uint16_t addr, int limit, TrxTraceHit *hits, bool *exhausted, uint32_t *unresolved);

// The observer log (T55): the records of the debug block's log action since the last call (see trx_ring.cc).
// out gets 8-byte engine records; max 0 moves the reader to the present; *lost counts overwritten records.
int  trx_ring_log_read(uint64_t *out, int max, uint32_t *lost);

// A08: "Timewarp is off while the REU is enabled (A08)" while the REU is on, else NULL. Every way
// into the ring (start, restore, land, scrub) answers with it; the UI, REST and trxmon show it.
const char *trx_ring_reu_refusal(void);
// Makes the ring follow the REU enable: on -> log off and the frames dropped; off again -> the ring
// starts over from that moment (when it ran or was due before). Call after anything that can change
// C64_REU_ENABLE; it is cheap and never blocks.
void trx_ring_reu_sync(void);

// Starts the log (restart: the timeline and the areas are emptied) with a
// capture every frame. Returns false without the engine, or while the REU is on.
bool trx_ring_start(void);
// Log off; the timeline stays (it can still be restored to until the next
// start, which empties it).
void trx_ring_stop(void);
void trx_ring_status(TrxRingStatus *st);

// Reads the markers that came since the last call into the timeline.
void trx_ring_poll(void);

// The timeline: entry 0 is the oldest. Copies an entry out (false: no such).
int  trx_ring_count(void);
bool trx_ring_frame(int index, TrxFrame *out);
// The index of the newest entry at or before "frames back" from the newest
// that a restore can go to (a whole chip block, logs still reach it); -1 none.
int  trx_ring_target(int frames_back);

// The index of the entry with this seq (indices move when old entries are
// dropped, seqs do not); -1 when it is gone.
int  trx_ring_find(uint32_t seq);

// Puts the machine back on entry "index" and lets it run from there (the
// frames after it are cut). The machine may run or be paused (also in a
// freeze session). Returns 0, or a negative error (trx_ring_error()).
int  trx_ring_restore(int index, TrxRestoreStats *stats);
const char *trx_ring_error(int code);

// The state of entry "index" without touching the live machine's run: a short
// pause to read RAM and colour RAM, then the undo applied in software.
// ram: 65536 bytes (C64 RAM, $D800-$DBFF the RAM under the I/O), color: 1024
// (low nibbles). cart, when it is not NULL: TRX_CART_RAM_BYTES of the
// cartridge's RAM window, read in the same pause and undone with the rest
// (T66 part 5; it costs a 64 KB DDR2 read, so the ring dump asks for NULL).
// Returns 0 or a negative error.
int  trx_ring_materialize(int index, uint8_t *ram, uint8_t *color, uint8_t *cart = NULL);
// Many entries at once (oldest first in idx[], n of them): one pause, the
// undo walked back from the head once. ram/color: n buffers each.
int  trx_ring_materialize_many(const int *idx, int n, uint8_t **ram, uint8_t **color,
                               uint8_t **carts = NULL);

// The freeze menu's deferred rewind: the menu chooses an entry, the restore
// happens when the menu closes (C64::unfreeze() asks).
void trx_ring_set_pending(int index);
bool trx_ring_pending(void);
// From C64::unfreeze() when trx_ring_pending(): restores and runs.
void trx_ring_unfreeze(void);

// Timewarp's scrub (TRX64-Ultimate docs/specs/T20-delta-ring.md §16.7), with
// the freeze menu up: puts the paused machine on entry "index" (from the entry
// it showed last, by the logged values), runs one frame so the still capture
// holds that frame's picture, and stops again. The first call ends the freeze
// session. From then on trx_ring_pending() is true: the unfreeze rewinds to
// the pending entry, or without one to the newest, from the scrub's state.
int  trx_ring_scrub_show(int index, TrxRestoreStats *stats);
bool trx_ring_scrubbing(void);

// The debugger's moves (T21 §4.10, trxmon): puts the machine on entry
// "index" and leaves it paused exactly on that frame's snap edge, the
// debugger holding it (page 3 hold, OWNER as it was), without cutting the
// timeline. The log stays on from there, so whatever the debugger runs is
// undone by the next land. The stop record says CYC_CMP (reason 7); the app
// sets IPC itself. Returns 0 or a negative error (trx_ring_error()).
int  trx_ring_land(int index, TrxRestoreStats *stats);
// The debugger runs on from where a land left the machine: the frames after
// the entry it stands in are cut, what ran since the land is that entry's,
// capture and the timeline go on. Nothing to do when nothing was landed.
void trx_ring_commit(void);
// The entry the last land put the machine on (its seq), 0 when none is open.
uint32_t trx_ring_landed(void);
// An entry a land or restore can go to (a whole block, the logs reach it).
bool trx_ring_reachable(int index);

// The memories a loaded state brings with it (T66). ram and col are always
// there; the two cartridge buffers only when the file carries them, and they go
// into DDR2 while the machine stands, before it is let go.
struct TrxLoadMem {
    const uint8_t *ram;      // 65536 (C64 RAM, $D800-$DBFF the RAM under the I/O)
    const uint8_t *col;      // 1024, low nibbles
    const uint8_t *cart;     // TRX_CART_RAM_BYTES of cartridge RAM, or NULL
    const uint8_t *rom;      // the cartridge ROM/flash window, or NULL
    uint32_t       rom_len;  // its length (a multiple of 16 KB, the bank size)
};

// Loads a machine state that is not in the ring (a .c64re with our chip
// block): the memories and the block put in as a restore puts a ring entry,
// then the ring restarts.
int  trx_ring_load_state(const TrxLoadMem *mem, const uint32_t *w, uint16_t x);

// ---- the native ring copy (.trxring, T20 §10.2, T66 part 6) ---------------------
// The file is a raw copy of the three log areas as far as the timeline still
// reaches them, so the ring itself comes back and not only the machine.
//
// Why a saved ring can be put back although the engine's record counters are
// read-only (trx_engine.v LOG_CNT: R, and only LOG_CTRL b8 sets them to 0):
// every count is rebased on the way in so that the newest entry's counts are
// 0, which is what the counters hold after that restart. A record's place in
// its area is (count * record size) & (size - 1), and 2^28 records are a whole
// number of area passes for all three areas, so the restored records land at
// the top of each area, ending just before offset 0 - exactly where the
// engine, writing forward from 0, will overwrite them oldest first.
uint32_t trx_ring_area_size(int s);     // bytes of area s (0, 1, 2)
uint32_t trx_ring_rec_size(int s);      // bytes per record of area s
// len bytes at byte offset off of area s, through the engine's mover; off and
// len are multiples of 16 and off + len <= size.
void trx_ring_area_read(int s, uint32_t off, void *dst, uint32_t len);
void trx_ring_area_write(int s, uint32_t off, const void *src, uint32_t len);
// The stream 2 record the parse reads next: the newest entry's marker is
// before it, so the save rebases the stream 2 counts on this one.
uint32_t trx_ring_stream2_pos(void);

// Puts a saved ring back. ents are the timeline entries oldest first, every
// count already rebased as above. areas(ctx) is called with the log off and
// the counters at 0 and has to write the records back with
// trx_ring_area_write(); it returns 0 or a negative error of its own, which is
// handed back. Then the newest entry's state goes into the machine (mem as
// trx_ring_load_state wants it) and the log runs on from there, timeline kept.
typedef int (*trx_area_fn)(void *ctx);
int trx_ring_install(const TrxFrame *ents, int n, const TrxLoadMem *mem, trx_area_fn areas, void *ctx);

// The turbo state the next trx_ring_load_state puts in with it (state bus chip 0 word 1, T29 §12.7:
// {hold[22:8] b31:17, $D031 b16:9, $D030 bit 0 b8, enable word b7:5, PREFER bit 7 b4, index b3:0});
// used once. A restore of a ring entry keeps the machine's current turbo state: the ring is
// recorded at 1 MHz only and does not carry it.
// U9 D0/D3: the cartridge types a .c64re can carry whole. One table in
// trx_ring.cc answers both sides - the writer refuses a type it says no to
// before a file exists, and the loader replays the banking registers of the
// types it says yes to. `name` is filled with the cartridge's name for the
// message, or NULL.
bool trx_ring_cart_state_ok(uint8_t type, const char **name);
// True when this cartridge keeps a 93C86 EEPROM whose contents are state the
// ROM window does not hold (GMod2): the file carries it as its own section.
bool trx_ring_cart_has_eeprom(uint8_t type);

void trx_ring_set_turbo(uint32_t pack);

#else

// Other products: no frame ring (the REST verbs answer 503).
static inline bool trx_ring_apply_hw(int) { return false; }
static inline int  trx_ring_fps(void) { return 50; }
static inline uint32_t trx_ring_fps_x100(void) { return 5012; }
static inline uint32_t trx_ring_frame_cycles(void) { return 19656; }
static inline bool trx_ring_start(void) { return false; }
static inline const char *trx_ring_reu_refusal(void) { return 0; }
static inline void trx_ring_reu_sync(void) { }
static inline int  trx_ring_log_read(uint64_t *, int, uint32_t *lost) { *lost = 0; return 0; }
static inline bool trx_ring_trace_present(void) { return false; }
static inline int  trx_ring_trace_ctl(int) { return -1; }
static inline bool trx_ring_trace_status(TrxTraceStatus *st) { st->present = false; return false; }
static inline int  trx_ring_trace_read(uint32_t, uint32_t, TrxTraceRec *) { return 0; }
static inline int  trx_ring_trace_writes(uint32_t, uint32_t, TrxWriteRec *) { return 0; }
static inline int  trx_ring_trace_whowrote(uint16_t, int, TrxTraceHit *, bool *e, uint32_t *u) { *e = true; *u = 0; return 0; }
static inline void trx_ring_stop(void) { }
static inline void trx_ring_status(TrxRingStatus *st) { st->present = false; }
static inline int  trx_ring_count(void) { return 0; }
static inline bool trx_ring_frame(int, TrxFrame *) { return false; }
static inline int  trx_ring_target(int) { return -1; }
static inline int  trx_ring_find(uint32_t) { return -1; }
static inline int  trx_ring_restore(int, TrxRestoreStats *) { return -1; }
static inline int  trx_ring_materialize(int, uint8_t *, uint8_t *, uint8_t * = NULL) { return -1; }
static inline int  trx_ring_materialize_many(const int *, int, uint8_t **, uint8_t **, uint8_t ** = NULL)
{ return -1; }
static inline int  trx_ring_scrub_show(int, TrxRestoreStats *) { return -1; }
static inline void trx_ring_set_pending(int) { }
static inline const char *trx_ring_error(int) { return "no frame ring in this bitstream"; }
static inline bool trx_ring_pending(void) { return false; }
static inline bool trx_ring_scrubbing(void) { return false; }
static inline int  trx_ring_land(int, TrxRestoreStats *) { return -1; }
static inline void trx_ring_commit(void) { }
static inline uint32_t trx_ring_landed(void) { return 0; }
static inline bool trx_ring_reachable(int) { return false; }
static inline void trx_ring_unfreeze(void) { }
struct TrxLoadMem;
static inline int  trx_ring_load_state(const TrxLoadMem *, const uint32_t *, uint16_t) { return -1; }
static inline uint32_t trx_ring_area_size(int) { return 0; }
static inline uint32_t trx_ring_rec_size(int) { return 0; }
static inline void trx_ring_area_read(int, uint32_t, void *, uint32_t) { }
static inline void trx_ring_area_write(int, uint32_t, const void *, uint32_t) { }
static inline uint32_t trx_ring_stream2_pos(void) { return 0; }
typedef int (*trx_area_fn)(void *ctx);
static inline int trx_ring_install(const TrxFrame *, int, const TrxLoadMem *, trx_area_fn, void *)
{ return -1; }
static inline bool trx_ring_cart_state_ok(uint8_t, const char **name) { if (name) *name = 0; return false; }
static inline bool trx_ring_cart_has_eeprom(uint8_t) { return false; }
static inline void trx_ring_set_turbo(uint32_t) { }

#endif

#endif
