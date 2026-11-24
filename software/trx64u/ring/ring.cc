// PROVENANCE: copied from TRX64-Ultimate software/ultimate-app/software/io/c64/trx_ring.cc (the frame ring's driver: timeline, restore, scrub, land, trace; GPL-3.0, see
// that file's own header below). Changes here: the InitFunction is gone (the operation ring.start starts the engine with trx_ring_engine_init(), called
// by the core's start hook), the log's start is the setting "Timewarp Ring" (ring_autostart). The menu is declarative (menu.toml, ring.* operations in
// ringops.cc); no screen code comes along.
// U18 (the ring library on the SDK): the registers through the driver table of c64_basic's TOML A (ringsys.h, drvtab.h) and the C64's memory window
// through api->c64, the cartridge port's page through the io group, the engine's lock through the sync group, the cartridge RAM through the export
// "c64cart", memory on the library's own account, the poll task replaced by the library's poll (trx_ring_engine_poll).
// U18 D6 (2026-11-22): brought to the firmware's state - the whowrote I/O address scan, the REU gate of trx_ring_reu.h, trx_ring_stream2_pos(),
// C64_STOP let go before a restore's RAM load. Kept from MultiCore's copy: a .c64re load (trx_ring_load_state) refuses while the REU is on; the
// cartridge image of a load goes in through TrxLoadMem::rom_put.
// The frame ring, app side. See trx_ring.h.
//
// Hardware: the blocks of ../gateware/c64_basic.toml, each with the RTL lines that decode it: trx_gate (the gate, the capture, the stop position),
// trx_statebus, trx_engine (the log, the copy and the apply), trx_debug and trx_dbg_run, trx_turbo, trx_pixel (the still's count), c64cart (its
// command: the cartridge drops its cached lines). The areas are fixed in the engine (area_base/area_size below, SLIM D4). c64_basic has no
// session register (the firmware's SH_CTRL at page 5 +0: trx_sys.v decodes no word 0x1400), so a stop here is no freeze session.
//
// Record formats (trx_engine.v header): stream 0 {new, old, addr16} 4 bytes;
// stream 1 8 bytes [27:0] address, [31:28] space (1 colour RAM, 2 cartridge
// RAM), [39:32] old, [47:40] new; stream 2 F1-F4 per frame boundary, then
// B1, the 58 words two per record, B2 when a chip block follows (F1 b1).
//
// The app's CPU cannot address the upper DDR2: every read of the areas is an
// engine COPY into a bounce buffer below 64 MB (the rvlite core has no data
// cache, so the copy is visible to it as soon as the mover is done).
//
// Not undone (no hardware for it): the SID (not on the state bus), the VIC's
// internals and its write-only registers ($D012 compare, $D011 bit 7: the
// current ones stay; its IRQ and collision latches only by clearing during
// the run, see clear_vic_latches), the REU, the banking registers of the
// cartridge types replay_cart() leaves out, drive A, and cartridge flash or
// EEPROM writes (no tap: the engine's LOG_PUT is gone, SLIM D6).

#include "trx_ring.h"
#include "trx_ea.h"
#include "trx_ring_reu.h"
#include "ringsys.h"
#include <string.h>

// The registers are the driver table's (ringsys.h HW_RD / HW_WR; c64_basic's TOML A: ident, trx_gate, trx_statebus, trx_engine, trx_debug,
// trx_dbg_run, trx_turbo, trx_pixel, c64cart); the names below are the values and bits this file combines.
#define GATE_FRZ       DRV_trx_gate_ctrl_frz_MASK              // the app's stop pauses the whole machine
#define GATE_HALTED    DRV_trx_gate_ctrl_halted_MASK
#define POS_ARM        DRV_trx_gate_stop_pos_arm_MASK
#define POS_RELEASE    DRV_trx_gate_stop_pos_release_MASK
#define POS_HOLDING    DRV_trx_gate_stop_pos_holding_MASK
#define POS_CYCLE      DRV_trx_gate_stop_pos_cycle_MASK
#define SB_REFUSED     DRV_trx_statebus_status_refused_MASK
#define SB_LOAD_ALL    (DRV_trx_statebus_load_cpu_MASK | DRV_trx_statebus_load_cia1_MASK | DRV_trx_statebus_load_cia2_MASK)

#define LOG_ON_ALL     (DRV_trx_engine_log_ctrl_on_MASK | DRV_trx_engine_log_ctrl_taps_MASK)   // on, outside writes, cartridge RAM, colour RAM
#define LOG_RESTART    DRV_trx_engine_log_ctrl_restart_MASK
#define LOG_TRACE      DRV_trx_engine_log_ctrl_trace_MASK     // the trace former (T21 §4.11.1)
#define LOG_FLUSH      DRV_trx_engine_log_ctrl_flush_MASK     // drop the pending instruction, the machine's flow is cut
#define TRACE_STREAM   3                                      // log_cnt[3]: trace records written, wraps at TRACE_WRAP
#define STAT_COPYING   DRV_trx_engine_log_stat_seq_busy_MASK
#define STAT_FIFO      DRV_trx_engine_log_stat_fifo_MASK

#define M28            0x0FFFFFFFu
#define OPEN           0xFFFFFFFFu

// The areas (the engine's fixed layout, T20 §5.1 / ARCH §15.7, SLIM D4)
static const uint32_t area_base[3] = { 0x7000000, 0x6000000, 0x5C00000 };
static const uint32_t area_size[3] = { 0x1000000, 0x0800000, 0x0400000 };
static const uint32_t rec_size[3]  = { 4, 8, 8 };

#define NREC_BLOCK     ((TRX_BLOCK_WORDS + 1) / 2)
#define TL_CAP         4096    // timeline entries (81 s at one per frame)
#define BOUNCE         0x10000
#define SAFETY         0x1000  // records kept clear of the area's wrap

// the block's words in table order (the capture table's reset value):
// glue 1, CPU 3, VIC 12, CIA 1 5, CIA 2 5, cartridge 1, CPU hidden 7,
// CIA 1 hidden 12, CIA 2 hidden 12
#define W_CPU          1
#define W_VIC          4
#define W_CIA1         16
#define W_CIA2         21
#define W_CPUH         27
#define W_CIA1H        34
#define W_CIA2H        46

static SvcMutex *mutex;            // the engine's lock (sync.mutex_new), made by trx_ring_engine_init
static bool present;
static bool log_on;
static uint8_t *bounce;             // 16-byte aligned
static TrxFrame *tl;                // circular, TL_CAP
static int tl_first, tl_n;
static uint32_t next_seq = 1;
static uint32_t p2;                 // next stream 2 record to parse
static uint64_t cyc_acc;
static uint32_t cyc_last;
static bool cyc_have;
static uint32_t resyncs;
static uint32_t spurious_f4;       // F4s the drain wrote after block records (see parse)
static bool f4_after_blocks;        // an older engine: it writes them (CAPS2 b0 = 0)
static bool apply_hw;               // the engine applies stream 0 records itself (CAPS2 b5)
static uint32_t cnt_now[2];         // the last LOG_CNT 0/1 read
static volatile int pending = -1;   // the freeze menu's choice (timeline index)
static uint32_t log_rd;             // the observer log reader (trx_ring_log_read): the next stream 1 record to look at
static bool log_have;               // log_rd is valid
static volatile bool scrubbing;     // a Timewarp scrub holds the machine (§16.7 below)
static TrxReuGate reu_gate;         // A08: the REU is on, the ring is off
// A08: the REU's enable, as the machine has it now. The ring does not record the REU's contents, so
// while it is on there is no ring (trx_ring_reu_sync) and no way into it (E_REU).
static inline bool reu_on(void) { return CART_RD(reu) != 0; }
static bool trace_on = true;       // the trace former runs with the log (trace_ctl)

// LOG_CTRL as the log runs: every tap, the trace, and with flush the machine's flow cut (a
// restore, land, scrub or load: the pending instruction belongs to the time that was undone)
static inline uint32_t logctl(bool flush)
{
    return LOG_ON_ALL | (trace_on ? LOG_TRACE : 0) | (flush ? LOG_FLUSH : 0);
}

// While a scrub is open every step logs one frame run into the areas: a target
// keeps this much more headroom, so the entry the machine stands on stays
// readable for the next step (a frame writes at most ~10 K records).
#define SCRUB_MARGIN   0x8000


static inline TrxFrame &ent(int i) { return tl[(tl_first + i) % TL_CAP]; }

static void *alloc_aligned(size_t size)
{
    uint8_t *p = (uint8_t *)r_malloc(size + 15);
    if (!p) {
        return NULL;
    }
    return (void *)(((uint32_t)p + 15) & ~15u);
}

static bool lock(void)
{
    if (!present) {
        return false;
    }
    g_api->sync->mutex_lock(mutex, SYNC_FOREVER);       // present: the sync group made the mutex
    return true;
}

static void unlock(void)
{
    g_api->sync->mutex_unlock(mutex);
}

// ---- the mover -------------------------------------------------------------------
static void eng_copy(uint32_t src, void *dst, uint32_t len)
{
    HW_WR(trx_engine_eng_src, src);
    HW_WR(trx_engine_eng_dst, (uint32_t)dst);
    HW_WR(trx_engine_eng_len, len);
    HW_WR(trx_engine_eng_cmd, DRV_trx_engine_eng_cmd_copy);
    uint16_t t0 = r_ms_timer();
    while (HW_RD(trx_engine_eng_cmd) & DRV_trx_engine_eng_cmd_busy_MASK) {
        if (uint16_t(r_ms_timer() - t0) > 200) {
            // the mover never answered: the data is not there, but the CPU must not wait forever
            r_log("[RING] COPY did not finish: %u bytes from 0x%07x\n", (unsigned)len, (unsigned)src);
            break;
        }
    }
    // the copy went through the memory controller behind the CPU's back; a
    // fence keeps the loads after the poll
    __asm__ volatile ("fence" ::: "memory");
}

// Records [from, from + n) of stream s, contiguous in the bounce buffer
// (n * size <= BOUNCE - 32): one COPY, two where the range wraps the area
// (the area's end is 16-byte aligned, so the second part lands aligned
// right behind the first). Returns where record "from" is.
static const uint8_t *fetch(int s, uint32_t from, uint32_t n)
{
    uint32_t size = area_size[s];
    uint32_t off = (from * rec_size[s]) & (size - 1);
    uint32_t a0 = off & ~15u;
    uint32_t pre = off - a0;
    uint32_t len = (pre + n * rec_size[s] + 15) & ~15u;
    if (a0 + len <= size) {
        eng_copy(area_base[s] + a0, bounce, len);
    } else {
        uint32_t l1 = size - a0;
        eng_copy(area_base[s] + a0, bounce, l1);
        eng_copy(area_base[s], bounce + l1, len - l1);
    }
    return bounce + pre;
}

static uint64_t rec64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) {
        v = (v << 8) | p[i];
    }
    return v;
}

static uint32_t rec32(const uint8_t *p)
{
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

// ---- the timeline ----------------------------------------------------------------
static bool reachable(const TrxFrame &f, uint32_t margin = 0)
{
    uint32_t r0 = area_size[0] / rec_size[0] - SAFETY - margin;
    uint32_t r1 = area_size[1] / rec_size[1] - SAFETY - margin;
    return ((cnt_now[0] - f.s0) & M28) <= r0 && ((cnt_now[1] - f.s1) & M28) <= r1;
}

static void trim(void)
{
    while (tl_n && (tl_n >= TL_CAP || !reachable(ent(0)))) {
        tl_first = (tl_first + 1) % TL_CAP;
        tl_n--;
    }
}

static void tl_clear(void)
{
    tl_first = 0;
    tl_n = 0;
}

// ---- trace epochs (T21 §4.11.2) ---------------------------------------------------
// The trace's records carry the engine's phi2 count (24 bits). A restore, land,
// scrub or load cuts the machine's flow and rebases the timeline's cycle count, so
// each cut starts an epoch: the first physical record, the timeline count of the
// entry the machine starts from (tc_start), and an anchor {hw_ref, tc_ref} that
// turns a record's count into a timeline count: tc = tc_ref + sext24(c24 - hw_ref).
// The anchor of the open epoch follows the frame markers (parse_locked), so a
// record is never further than a few frames from it.
#define TR_AREA     0x6800000u
#define TRACE_CAP      349525u                  // records in the area (3 words each)
#define TRACE_WRAP     (TRACE_CAP * 768u)       // LOG_CNT[3] wraps here
#define TRACE_SAFETY   64                       // records kept clear of the wrap
#define TRACE_CHUNK    512                      // records per read
#define MAX_EP         48

struct Epoch {
    uint32_t phys0;         // first physical record (count when the epoch began)
    uint64_t tc_start;      // timeline count of the cut: records after it belong to this epoch
    uint32_t hw_ref;
    int64_t  tc_ref;
    bool     anch;
    bool     keep_known;    // keep_end is set (the epoch is closed)
    bool     gap;           // the cut left a hole in the history
    uint32_t keep_end;      // closed: the first physical record that is no history any more
};
static Epoch ep[MAX_EP];
static int ep_n;
static uint8_t *tbuf;       // TRACE_CHUNK records, and stream 0 records for them

static inline uint32_t trace_cnt(void)
{
    return HW_RD_I(trx_engine_log_cnt, TRACE_STREAM) & M28;
}

static void ep_reset(void)
{
    memset(ep, 0, sizeof(ep));
    ep_n = 1;
    ep[0].tc_start = 0;
}

// the anchor of the open epoch: the newest marker
static void ep_anchor(void)
{
    if (ep_n && cyc_have) {
        ep[ep_n - 1].hw_ref = cyc_last;
        ep[ep_n - 1].tc_ref = (int64_t)cyc_acc;
        ep[ep_n - 1].anch = true;
    }
}

// The machine starts over from the timeline entry at tc_start, the engine's count being
// hw there. Called with the machine stopped, before the log is put on again with a flush.
static void ep_push(uint64_t tc_start, uint32_t hw)
{
    if (!(trx_io_caps() & DRV_ident_caps_debug_MASK) || !ep_n) {
        return;
    }
    Epoch &c = ep[ep_n - 1];
    if (c.tc_start == tc_start && c.anch && c.hw_ref == (hw & 0xFFFFFF) && ep_n > 1) {
        return;             // the commit of a land: the same cut
    }
    if (ep_n == MAX_EP) {
        memmove(&ep[0], &ep[1], sizeof(Epoch) * (MAX_EP - 1));
        ep_n--;
    }
    Epoch &e = ep[ep_n++];
    memset(&e, 0, sizeof(e));
    e.phys0 = trace_cnt();
    e.tc_start = tc_start;
    e.hw_ref = hw & 0xFFFFFF;
    e.tc_ref = (int64_t)tc_start;
    e.anch = true;
    ep[ep_n - 2].keep_known = false;
}

// stream 2: every whole frame since the last call
static void parse_locked(void)
{
    uint32_t cnt = HW_RD_I(trx_engine_log_cnt, 2) & M28;
    cnt_now[0] = HW_RD_I(trx_engine_log_cnt, 0) & M28;
    cnt_now[1] = HW_RD_I(trx_engine_log_cnt, 1) & M28;
    if (scrubbing) {
        // a scrub or a debugger land holds the machine: what runs from there
        // (a frame run, the debugger's steps) makes markers that are no
        // timeline entries; the final rewind or the commit skips them
        return;
    }
    const uint32_t chunk_recs = (BOUNCE - 32) / 8;
    for (;;) {
        uint32_t avail = (cnt - p2) & M28;
        if (avail > area_size[2] / 8) {
            // the parser fell a whole area behind: what it missed is gone
            r_log("[RING] stream 2 lapped (%u records behind), timeline dropped\n", (unsigned)avail);
            tl_clear();
            p2 = cnt;
            break;
        }
        if (avail < 4) {
            break;
        }
        uint32_t n = avail < chunk_recs ? avail : chunk_recs;
        const uint8_t *buf = fetch(2, p2, n);
        uint32_t i = 0;
        for (;;) {
            if (n - i < 4) {
                break;
            }
            uint64_t m0 = rec64(buf + 8 * i), m1 = rec64(buf + 8 * (i + 1));
            uint64_t m2 = rec64(buf + 8 * (i + 2)), m3 = rec64(buf + 8 * (i + 3));
            if (m0 >> 56 != 0xF1 || m1 >> 56 != 0xF2 || m2 >> 56 != 0xF3 || m3 >> 56 != 0xF4) {
                i++;            // not at a marker: step until one comes
                resyncs++;
                continue;
            }
            uint8_t flags = uint8_t(m0 >> 48);
            // the block: B1, the words two per record, B2. An older engine
            // (CAPS2 b0 = 0) takes any stream 2 record whose top byte is F3 for
            // a frame's F3 marker and writes an F4 after it: a block record with
            // F3 in its top byte (an odd word whose high byte is $F3, e.g. the
            // 6510's S) is followed by such an F4, which is skipped here. A fixed
            // engine writes none, and the next record is a block record that
            // may have F4 on top itself: no skip there.
            uint32_t j = i + 4;
            uint32_t wv[TRX_BLOCK_WORDS];
            bool whole = false;
            uint8_t bfl = 0;
            bool incomplete = false;
            if (flags & 2) {
                if (j >= n) {
                    incomplete = true;
                } else if (rec64(buf + 8 * j) >> 56 == 0xB1) {
                    j++;
                    int got = 0;
                    while (got < TRX_BLOCK_WORDS) {
                        if (j >= n) {
                            incomplete = true;
                            break;
                        }
                        uint64_t r = rec64(buf + 8 * j);
                        j++;
                        wv[got++] = uint32_t(r);
                        if (got < TRX_BLOCK_WORDS) {
                            wv[got++] = uint32_t(r >> 32);
                        }
                        if (f4_after_blocks && r >> 56 == 0xF3) {
                            if (j >= n) {
                                incomplete = true;
                                break;
                            }
                            if (rec64(buf + 8 * j) >> 56 == 0xF4) {
                                j++;
                                spurious_f4++;
                            }
                        }
                    }
                    if (!incomplete) {
                        if (j >= n) {
                            incomplete = true;
                        } else {
                            uint64_t bt = rec64(buf + 8 * j);
                            if (bt >> 56 == 0xB2) {
                                j++;
                                bfl = uint8_t(bt >> 48);
                                uint16_t words = uint16_t(bt >> 32);
                                uint16_t g0 = uint16_t(bt), g1 = uint16_t(bt >> 16);
                                whole = (flags & 1) && !(bfl & 1) && words == TRX_BLOCK_WORDS && g0 == g1;
                            }
                        }
                    }
                }
            }
            if (incomplete) {
                break;          // the block is still on its way (or past this chunk)
            }
            uint32_t need = j - i;
            if (tl_n >= TL_CAP) {
                tl_first = (tl_first + 1) % TL_CAP;   // full: the oldest goes
                tl_n--;
            }
            TrxFrame &nf = ent(tl_n);
            nf.seq = next_seq++;
            nf.marker = uint32_t(m0);
            nf.flags = flags;
            uint32_t c24 = uint32_t(m1) & 0xFFFFFF;
            if (cyc_have) {
                cyc_acc += (c24 - cyc_last) & 0xFFFFFF;
            }
            cyc_last = c24;
            cyc_have = true;
            nf.cycles = cyc_acc;
            nf.c24 = c24;
            nf.line = uint16_t(m2 >> 40) & 0x1FF;
            nf.x = uint16_t(m2 >> 24) & 0x3FF;
            nf.s0 = uint32_t(m3) & M28;
            nf.s1 = uint32_t(m3 >> 28) & M28;
            nf.e0 = nf.e1 = OPEN;
            nf.block = whole;
            nf.bflags = bfl;
            if (whole) {
                memcpy(nf.w, wv, sizeof(wv));
            }
            if (tl_n) {
                TrxFrame &pf = ent(tl_n - 1);
                if (pf.e0 == OPEN) {
                    pf.e0 = nf.s0;
                    pf.e1 = nf.s1;
                }
            }
            tl_n++;
            i += need;
        }
        if (!i) {
            break;
        }
        p2 = (p2 + i) & M28;
        if (i < n && n == avail) {
            break;              // the rest is a frame not yet complete
        }
    }
    trim();
    ep_anchor();
}

// The machine starts over from entry e's snap: e's F2 count becomes the one
// of that load edge, so the next marker's cycle step is one frame and the
// count stays the debug block's CYCLES (T21 relies on both). Without the
// debug page (CYCLES reads 0) nothing changes.
#define DBG_ST_NEW     DRV_trx_debug_status_stop_new_MASK
#define DBG_ST_OWNER   DRV_trx_debug_status_owner_MASK

static void rebase_cycles(TrxFrame &e, uint32_t c24)
{
    if (!(trx_io_caps() & DRV_ident_caps_debug_MASK)) {
        return;
    }
    e.c24 = c24 & 0xFFFFFF;
    ep_push(e.cycles, e.c24);
    cyc_acc = e.cycles;
    cyc_last = e.c24;
    cyc_have = true;
}

// While the debugger owns run control (T21: OWNER) the app's C64_STOP does not
// reach the gate, and its hold keeps the machine from the run to a snap
// position. A restore therefore first lets C64_STOP hold the machine, then
// takes OWNER and the debugger's hold away. Returns whether the debugger had
// run control; a restore or load leaves it with C64_STOP (the debugger sees
// that, as when the firmware takes the machine for its menu), a land gives it
// back.
static bool dbg_handover(void)
{
    if (!(trx_io_caps() & DRV_ident_caps_debug_MASK) || !(HW_RD(trx_debug_status) & DBG_ST_OWNER)) {
        return false;
    }
    HW_WR(trx_gate_ctrl, GATE_FRZ);
    io_stop(true);
    HW_WR(trx_dbg_run_owner, 0);
    HW_WR(trx_debug_cmd, DRV_trx_debug_cmd_run);
    return true;
}

// ---- machine control -------------------------------------------------------------
// addr: DRV_<block>_<property>_ADDR of a 32-bit register that reads (ringsys.h hw_rd32)
static bool wait_reg(uint32_t addr, uint32_t mask, uint32_t want, int ms)
{
    uint16_t t0 = r_ms_timer();
    while ((hw_rd32(addr) & mask) != want) {
        if (uint16_t(r_ms_timer() - t0) > ms) {
            return false;
        }
    }
    return true;
}

static void drain(void)
{
    // the drain empties the FIFO by itself; the sequencer's copy is ~8 us
    wait_reg(DRV_trx_engine_log_stat_ADDR, STAT_FIFO | STAT_COPYING, 0, 20);
}

struct Saved {
    uint32_t gate;
    bool was_halted;
};

// Stops the machine on line 0 (STOP_POS) if it runs, so the VIC's
// write-only registers keep the values a frame start has; then the stop is
// held by the app's C64_STOP as a machine pause.
static void stop_machine(Saved *sv, bool at_line0)
{
    sv->gate = HW_RD(trx_gate_ctrl);
    sv->was_halted = (sv->gate & GATE_HALTED) != 0;
    if (!sv->was_halted && at_line0) {
        HW_WR(trx_gate_stop_pos, POS_ARM | 12);
        if (!wait_reg(DRV_trx_gate_stop_pos_ADDR, POS_HOLDING, POS_HOLDING, 60)) {
            r_log("[RING] no stop on line 0, pausing anywhere\n");
            HW_WR(trx_gate_stop_pos, 0);
        }
    }
    HW_WR(trx_gate_ctrl, GATE_FRZ);              // the app's stop = machine pause
    io_stop(true);
    wait_reg(DRV_trx_gate_ctrl_ADDR, GATE_HALTED, GATE_HALTED, 100);
}

// ---- undo ------------------------------------------------------------------------
// What an undo needs is, for every address written between entry lo's start
// and entry hi's start (hi = tl_n: now), the old byte of its earliest write
// there. collect() walks those records newest first and stores each one's old
// byte, so the earliest write's is what stays, and marks the address; the
// caller then writes each marked byte once. The rvlite core reads DDR2
// without a cache (a load costs about a microsecond) while its stores are
// posted, so the walk is one load per record and stores only; the marks are
// bytes, scanned a word at a time afterwards.

struct Coll {
    uint32_t n0, n1, other;
};

static uint8_t *ram_val;            // C64 RAM, 64 KB
static uint8_t *ram_tch;            // a byte per address, 64 KB (word aligned)
static uint8_t *cart_val;           // cartridge RAM, 64 KB
static uint8_t *cart_tch;
static uint8_t col_val[1024];       // colour RAM
static uint32_t col_tch_w[256];
static uint8_t *const col_tch = (uint8_t *)col_tch_w;

static void collect_range(int s, uint32_t from, uint32_t to, Coll *c)
{
    const uint32_t rs = rec_size[s];
    const uint32_t chunk = (BOUNCE - 32) / rs;
    uint8_t *const rv = ram_val, *const rt = ram_tch;
    uint8_t *const kv = cart_val, *const kt = cart_tch;
    while (((to - from) & M28) != 0) {
        uint32_t n = (to - from) & M28;
        if (n > chunk) {
            n = chunk;
        }
        uint32_t start = (to - n) & M28;
        const uint32_t *wp = (const uint32_t *)fetch(s, start, n);
        if (s == 0) {
            for (int k = int(n) - 1; k >= 0; k--) {
                uint32_t r = wp[k];
                uint32_t a = r & 0xFFFF;
                rv[a] = uint8_t(r >> 16);
                rt[a] = 1;
            }
            c->n0 += n;
        } else {
            for (int k = int(n) - 1; k >= 0; k--) {
                uint32_t lo = wp[2 * k];
                uint32_t a = lo & 0xFFFF;
                switch (lo >> 28) {
                case 1:
                    col_val[a & 0x3FF] = uint8_t(wp[2 * k + 1]) & 0x0F;
                    col_tch[a & 0x3FF] = 1;
                    break;
                case 2:
                    kv[a] = uint8_t(wp[2 * k + 1]);
                    kt[a] = 1;
                    break;
                default:
                    c->other++;
                    break;
                }
            }
            c->n1 += n;
        }
        to = start;
    }
}

// entries [lo, hi): their stream 0 and/or stream 1 records, newest first
static void collect(int lo, int hi, bool s0, bool s1, Coll *c)
{
    if (s0) {
        memset(ram_tch, 0, 65536);
    }
    if (s1) {
        memset(col_tch, 0, 1024);
        memset(cart_tch, 0, 65536);
    }
    for (int j = hi - 1; j >= lo; j--) {
        TrxFrame &f = ent(j);
        if (s0) {
            collect_range(0, f.s0, f.e0 == OPEN ? cnt_now[0] : f.e0, c);
        }
        if (s1) {
            collect_range(1, f.s1, f.e1 == OPEN ? cnt_now[1] : f.e1, c);
        }
    }
}

// calls put(a, v) for every marked address (marks: bytes, len a multiple of 4)
template <typename F> static uint32_t each(const uint8_t *tch, int len, const uint8_t *val, F put)
{
    uint32_t n = 0;
    const uint32_t *w = (const uint32_t *)tch;
    for (int i = 0; i < len / 4; i++) {
        uint32_t m = w[i];
        if (!m) {
            continue;
        }
        for (int b = 0; b < 4; b++) {
            if (m & (0xFFu << (8 * b))) {
                int a = 4 * i + b;
                put(a, val[a]);
                n++;
            }
        }
    }
    return n;
}

// The cartridge's RAM (64 KB in DDR2) is reached through the export "c64cart" (ram_read, ram_write: a write is complete when the call returns).
// Puts the marked bytes of `val` back, one ram_write per run of neighbouring marks. pg: the pages to look at, cleared with their marks (null: every
// page, the marks stay). The bytes written; 0 without the export.
static uint32_t cart_ram_put(uint8_t *pg, uint8_t *tch, const uint8_t *val)
{
    const C64CartApi *cc = R_TABLE(C64CartApi, RX_C64CART, ram_write);
    uint32_t n = 0;
    int run = -1;                       // where the open run starts
    for (int p = 0; p < 256; p++) {
        if (pg && !pg[p]) {
            if (run >= 0) {
                if (cc) {
                    cc->ram_write(uint32_t(run), val + run, uint32_t(256 * p - run));
                }
                run = -1;
            }
            continue;
        }
        if (pg) {
            pg[p] = 0;
        }
        uint32_t *w = (uint32_t *)(tch + 256 * p);
        for (int i = 0; i < 64; i++) {
            uint32_t m = w[i];
            for (int b = 0; b < 4; b++) {
                int a = 256 * p + 4 * i + b;
                if (m & (0xFFu << (8 * b))) {
                    if (run < 0) {
                        run = a;
                    }
                    n++;
                } else if (run >= 0) {
                    if (cc) {
                        cc->ram_write(uint32_t(run), val + run, uint32_t(a - run));
                    }
                    run = -1;
                }
            }
            if (pg && m) {
                w[i] = 0;
            }
        }
    }
    if (run >= 0 && cc) {
        cc->ram_write(uint32_t(run), val + run, uint32_t(65536 - run));
    }
    return cc ? n : 0;
}

// ---- restore ---------------------------------------------------------------------
static uint32_t bits(const uint32_t *w, int lo, int n)
{
    uint32_t v = 0;
    for (int b = 0; b < n; b++) {
        v |= ((w[(lo + b) / 32] >> ((lo + b) % 32)) & 1u) << b;
    }
    return v;
}

// VIC registers from the block (read images) as bus writes; $D012-$D014,
// $D019, $D01E, $D01F are not written (their read image is not the
// register), $D011 only when bits 6-0 differ from the machine's now (bit 7
// reads the raster line, so writing it would clear the compare's bit 8).
static void replay_vic(const uint32_t *w, uint32_t *count)
{
    HW_WR(trx_statebus_addr, (2 << 7) | 4);
    uint32_t now11 = HW_RD(trx_statebus_data);   // chip 2 word 4 = $D010-$D013
    auto vreg = [&](int r) { return uint8_t(w[W_VIC + r / 4] >> (8 * (r & 3))); };
    for (int r = 0; r <= 0x2E; r++) {
        if (r == 0x12 || r == 0x13 || r == 0x14 || r == 0x19 || r == 0x1E || r == 0x1F) {
            continue;
        }
        if (r == 0x11 && ((vreg(r) ^ uint8_t(now11 >> 8)) & 0x7F) == 0) {
            continue;
        }
        c64_poke(0xD000 + r, vreg(r));
        (*count)++;
    }
}

// The CIA registers the subset load leaves out (ARCH §15.9): port and timer
// latches, ICR mask, TOD alarm, from the hidden words (bit positions as
// tb_ring.cpp restoretest uses them).
static void replay_cia(const uint32_t *w, uint32_t *count)
{
    for (int c = 0; c < 2; c++) {
        const uint32_t *h = w + (c ? W_CIA2H : W_CIA1H);
        uint16_t io = c ? 0xDD00 : 0xDC00;
        c64_poke(io + 2, bits(h, 16, 8));         // DDRA
        c64_poke(io + 3, bits(h, 8, 8));          // DDRB
        c64_poke(io + 0, bits(h, 32, 8));         // PRA
        c64_poke(io + 1, bits(h, 24, 8));         // PRB
        uint32_t ta = bits(h, 43, 16), tb = bits(h, 83, 16);
        c64_poke(io + 4, ta & 0xFF);
        c64_poke(io + 5, ta >> 8);
        c64_poke(io + 6, tb & 0xFF);
        c64_poke(io + 7, tb >> 8);
        c64_poke(io + 0xD, 0x7F);
        c64_poke(io + 0xD, 0x80 | bits(h, 311, 5));
        c64_poke(io + 0xF, 0x80);                 // CRB: alarm writes (CRB itself is loaded)
        c64_poke(io + 0xB, bits(h, 123, 8));
        c64_poke(io + 0xA, bits(h, 131, 8));
        c64_poke(io + 0x9, bits(h, 139, 8));
        c64_poke(io + 0x8, bits(h, 147, 8));
        *count += 17;
    }
}

// The cartridge's banking registers (chip 7 w0 = {reg_1, reg_0} of icepi's
// model, cart_slot.v) go back as bus writes to IO1, for the types whose
// registers are plain writes there (c64_cartridge.v). Which types those are is
// the table below, and it is the same table the writer refuses by.
#define W_GLUE         0
#define W_CART         26
#define GLUE_CART      0x08

// U9 D0/D3: one list, read by the writer's gate (ringfile.cc frame_prep) and by the
// replay below. A type is on it when a .c64re can carry its whole state back:
// the banking registers go in as bus writes, and nothing of the cartridge's own
// state lives outside the ROM window, the cartridge RAM and - for GMod2 - the
// EEPROM section. A type is added to both sides at once or to neither; what is
// not on the list is refused before a file is written, never half-loaded.
//
// Left off, and why: GMod2 is ON the list (its $DE00 is one register, bank in
// bits 0-5 and mode in 6-7, and the EEPROM array travels as its own section);
// C64MegaCart keeps state at $DF00 that is cartridge RAM too; Dinamic and C64GS
// bank on the address of a read or write, with no register to replay; Action
// Replay and Simons' BASIC have more state than the block carries.
#define CR_NONE        0        // no banking register
#define CR_DE00        1        // one byte at $DE00
#define CR_DE00_DE02   2        // $DE00 and $DE02

struct TrxCartRow {
    uint8_t     base;           // C64_CARTRIDGE_TYPE & 0x1F
    uint8_t     var;            // C64_CARTRIDGE_TYPE >> 5
    bool        any_var;        // the row covers every variant of base
    uint8_t     regs;
    bool        eeprom;         // keeps a 93C86 the file has to carry
    const char *name;
};

static const TrxCartRow c_cart_rows[] = {
    { 0x01, 0, true,  CR_NONE,      false, "Normal" },
    { 0x08, 0, false, CR_DE00,      false, "Ocean" },
    { 0x08, 1, false, CR_DE00,      false, "Magic Desk / Domark" },
    { 0x08, 2, false, CR_DE00,      true,  "GMod2" },
    { 0x08, 5, false, CR_DE00,      false, "Magic Desk 16" },
    { 0x09, 0, true,  CR_DE00,      false, "Ocean 16K" },
    { 0x0F, 0, true,  CR_DE00_DE02, false, "MegaByter" },
    { 0x11, 0, true,  CR_DE00_DE02, false, "EasyFlash" },
};

static const TrxCartRow *cart_row(uint8_t type)
{
    uint8_t base = type & 0x1F, var = uint8_t(type >> 5);
    for (unsigned i = 0; i < sizeof(c_cart_rows) / sizeof(c_cart_rows[0]); i++) {
        if (c_cart_rows[i].base == base && (c_cart_rows[i].any_var || c_cart_rows[i].var == var)) {
            return &c_cart_rows[i];
        }
    }
    return NULL;
}

bool trx_ring_cart_state_ok(uint8_t type, const char **name)
{
    const TrxCartRow *r = cart_row(type);
    if (name) {
        *name = r ? r->name : NULL;
    }
    return r != NULL;
}

bool trx_ring_cart_has_eeprom(uint8_t type)
{
    const TrxCartRow *r = cart_row(type);
    return r && r->eeprom;
}

// The banking registers of the running cartridge, put back (the app has no
// model, cart_slot.v) as bus writes to IO1. Returns false for a cartridge this
// build cannot put back - which the writer's gate has already refused, so a
// false here means a file from an older build or another writer.
static bool replay_cart(const uint32_t *w, uint32_t *count)
{
    if (!(w[W_GLUE] & GLUE_CART)) {
        return true;
    }
    const TrxCartRow *r = cart_row(CART_RD(type));
    if (!r) {
        return false;
    }
    uint8_t r0 = uint8_t(w[W_CART]), r1 = uint8_t(w[W_CART] >> 8);
    // GMod2 rides the CR_DE00 path: the same write carries bank and mode, and it
    // drives the EEPROM's lines to the levels they held at the freeze. The array
    // itself is restored afterwards, out of the file's own section, so what this
    // write does to the serial lines cannot cost a byte of it.
    switch (r->regs) {
    case CR_DE00_DE02:
        c64_poke(0xDE00, r0);
        c64_poke(0xDE02, r1);
        *count += 2;
        return true;
    case CR_DE00:
        c64_poke(0xDE00, r0);
        (*count)++;
        return true;
    default:
        return true;
    }
}

static bool turbo_pending;
static uint32_t turbo_pack;

void trx_ring_set_turbo(uint32_t pack)
{
    turbo_pack = pack;
    turbo_pending = (HW_RD(ident_caps2) & DRV_ident_caps2_turbo_MASK) != 0;
}

static bool stage_and_load(const uint32_t *w)
{
    HW_WR(trx_statebus_status, 0);
    HW_WR(trx_statebus_addr, (1 << 7) | 8);
    for (int k = 0; k < 7; k++) {
        HW_WR(trx_statebus_data, w[W_CPUH + k]);
    }
    HW_WR(trx_statebus_addr, (3 << 7) | 8);
    for (int k = 0; k < 12; k++) {
        HW_WR(trx_statebus_data, w[W_CIA1H + k]);
    }
    HW_WR(trx_statebus_addr, (4 << 7) | 8);
    for (int k = 0; k < 12; k++) {
        HW_WR(trx_statebus_data, w[W_CIA2H + k]);
    }
    uint32_t mask = SB_LOAD_ALL;
    if (turbo_pending) {                // chip 0 word 1: the turbo state, loaded with the rest (bit 0)
        HW_WR(trx_statebus_addr, (0 << 7) | 1);
        HW_WR(trx_statebus_data, turbo_pack);
        mask |= 1;
        turbo_pending = false;
    }
    if (HW_RD(trx_statebus_status) & SB_REFUSED) {
        return false;
    }
    HW_WR(trx_statebus_load, mask);
    return true;
}

enum {
    E_NONE = 0,
    E_ABSENT = -1,
    E_INDEX = -2,
    E_NOBLOCK = -3,
    E_GONE = -4,
    E_NOPOS = -5,
    E_STAGE = -6,
    E_MEM = -7,
    E_APPLY = -8,
    E_REU = -9,
    E_MOVED = -10,
};

const char *trx_ring_error(int code)
{
    switch (code) {
    case E_NONE:    return "ok";
    case E_ABSENT:  return "no frame ring in this bitstream";
    case E_INDEX:   return "no such frame";
    case E_NOBLOCK: return "the frame has no whole chip block";
    case E_GONE:    return "the log no longer reaches the frame";
    case E_NOPOS:   return "the machine did not reach the frame's snap position";
    case E_STAGE:   return "the state bus refused the staged words";
    case E_MEM:     return "out of memory";
    case E_APPLY:   return "the engine did not finish applying the log";
    case E_REU:     return TRX_REU_OFF_TEXT;
    case E_MOVED:   return "the timeline changed while the ring was being read";
    default:        return "error";
    }
}

// ---- APPLY (T20 §17.3) -----------------------------------------------------------
// With the engine's APPLY (apply_hw) stream 0 is not walked by the CPU: the
// ranges to undo or redo are planned as runs (contiguous, one direction) and
// the engine applies them through the RAM's second port when the memory is
// put in (apply_plan()). Consecutive timeline entries are contiguous in the
// stream; a cut leaves a gap, so a plan is a few runs. Without APPLY
// plan_range() walks the records at once (walk_range, marks for each_pg).
struct Run {
    uint32_t a, b;          // records [a, b) of stream 0 (28-bit counts)
    bool redo;              // oldest first, new bytes; else newest first, old bytes
};
#define MAX_RUNS       256
static Run runs[MAX_RUNS];
static int n_runs;
static bool plan_full;      // more runs than MAX_RUNS: the plan is not whole
static int apply_err;       // the last apply_plan()'s result (E_NONE or E_APPLY / E_MEM)

// the engine's job: records of stream 0 from index "start", "count" of them,
// downwards (undo) or upwards (redo); false when it does not finish in time
static bool apply_run(uint32_t start, uint32_t count, bool undo)
{
    HW_WR(trx_engine_ap_start, start & M28);
    HW_WR(trx_engine_ap_count, count);
    HW_WR(trx_engine_ap_cmd, undo ? DRV_trx_engine_ap_cmd_undo : DRV_trx_engine_ap_cmd_redo);
    uint32_t ms = 50 + count / 10000;       // the engine does 20 records per us and more
    uint16_t t0 = r_ms_timer();
    while (HW_RD(trx_engine_ap_cmd) & DRV_trx_engine_ap_cmd_busy_MASK) {
        if (uint16_t(r_ms_timer() - t0) > ms) {
            r_log("[RING] APPLY did not finish: %u records from %u\n", (unsigned)count, (unsigned)start);
            return false;
        }
    }
    return true;
}

static void walk_range(int s, uint32_t from, uint32_t to, bool redo, Coll *c);

static void plan_range(uint32_t a, uint32_t b, bool redo, Coll *c)
{
    uint32_t n = (b - a) & M28;
    if (!n) {
        return;
    }
    if (!apply_hw) {
        walk_range(0, a, b, redo, c);
        return;
    }
    c->n0 += n;
    if (n_runs) {
        Run &r = runs[n_runs - 1];
        if (r.redo == redo && (redo ? r.b == a : r.a == b)) {
            if (redo) {
                r.b = b;
            } else {
                r.a = a;
            }
            return;
        }
    }
    if (n_runs == MAX_RUNS) {
        plan_full = true;
        return;
    }
    runs[n_runs++] = Run{ a, b, redo };
}

// the planned runs, in the order they were planned; the plan is empty after
static int apply_plan(void)
{
    int err = plan_full ? E_MEM : E_NONE;
    for (int i = 0; !err && i < n_runs; i++) {
        const Run &r = runs[i];
        uint32_t n = (r.b - r.a) & M28;
        if (!apply_run(r.redo ? r.a : (r.b - 1) & M28, n, !r.redo)) {
            err = E_APPLY;
        }
    }
    n_runs = 0;
    plan_full = false;
    return err;
}

// Phases 3-8 of the restore with the machine stopped by stop_machine(): the
// register replay and colour RAM as bus cycles (Ultimax, so the I/O is seen
// whatever $01 holds), the run to the snap position with the CPU held, the
// RAM through the second port, the staged words, release. mem() puts the
// memories in: undo for a ring entry, a full copy for a loaded state.
typedef void (*bus_mem_fn)(void *ctx);   // colour RAM, while the bus is ours
typedef void (*ram_fn)(void *ctx);       // C64 RAM, at the stop position

// While the machine runs to the snap position (CPU held), the VIC passes
// every raster line, so its compare sets the raster IRQ latch and sprites set
// the collision latches, which the block's frame did not have: an extra IRQ
// or collision after the restore. The latches are not on the state bus, so
// they are cleared by DMA cycles during the run ($D019 written, $D01E/$D01F
// read: the CPU is held anyway, the VIC goes on in step) for the bits the
// block's frame had clear, until line 300. No DMA after that: one that came
// once the machine stood on the position would open the clock again. A
// compare on lines 300-311 or 0 still leaves its latch (put_back reports
// it). Only when $01 shows the I/O (no Ultimax while the VIC runs).
static void clear_vic_latches(const uint32_t *w)
{
    uint8_t port = uint8_t(w[W_CPU + 2] >> 16);     // $01 as read
    if (!(port & 4) || !(port & 3)) {
        return;
    }
    uint8_t clr = ~uint8_t(w[W_VIC + 6] >> 8) & 0x0F;
    bool c1e = uint8_t(w[W_VIC + 7] >> 16) == 0, c1f = uint8_t(w[W_VIC + 7] >> 24) == 0;
    uint16_t t0 = r_ms_timer();
    while (!(HW_RD(trx_gate_stop_pos) & POS_HOLDING) && uint16_t(r_ms_timer() - t0) < 60) {
        int raster = c64_peek(0xD012) | ((c64_peek(0xD011) & 0x80) << 1);
        if (raster >= 300) {
            break;
        }
        if (clr) {
            c64_poke(0xD019, clr);
        }
        if (c1e) {
            (void)c64_peek(0xD01E);
        }
        if (c1f) {
            (void)c64_peek(0xD01F);
        }
    }
}

static int put_back(const uint32_t *w, uint16_t x, const Saved &sv, bus_mem_fn busmem, ram_fn ram,
                    void *ctx, uint32_t *bus_writes, uint16_t *pos_ms = NULL, bool *latch_ok = NULL)
{
    bool memonly = HW_RD(c64_dma_mode_memonly) != 0;
    io_memonly(false);
    c64_set_mode(DRV_cart_regs_mode_ultimax);
    if (!replay_cart(w, bus_writes)) {
        r_log("[RING] cartridge type %02x: banking registers not replayed\n", CART_RD(type));
    }
    if (busmem) {
        busmem(ctx);
    }
    replay_vic(w, bus_writes);
    replay_cia(w, bus_writes);
    c64_set_mode(DRV_cart_regs_mode_normal);

    // hold the CPU (CPU-only pause), let the machine run to the snap position
    HW_WR(trx_gate_ctrl, 0);
    HW_WR(trx_gate_stop_pos, POS_ARM | POS_RELEASE | ((x >> 3) & POS_CYCLE));
    int err = E_NONE;
    clear_vic_latches(w);
    if (!wait_reg(DRV_trx_gate_stop_pos_ADDR, POS_HOLDING, POS_HOLDING, 100)) {
        err = E_NOPOS;
    }
    if (latch_ok) {
        // the VIC's latches as they are now against the block's ($D019 in
        // chip 2 w6 byte 1, $D01E/$D01F w7 bytes 2/3)
        HW_WR(trx_statebus_addr, (2 << 7) | 6);
        uint32_t v6 = HW_RD(trx_statebus_data), v7 = HW_RD(trx_statebus_data);
        *latch_ok = ((v6 >> 8) & 0x0F) == ((w[W_VIC + 6] >> 8) & 0x0F) && (v7 >> 16) == (w[W_VIC + 7] >> 16);
    }
    if (pos_ms) {
        *pos_ms = r_ms_timer();
    }

    // C64 RAM through the second port (the CPU is held at a read: no more
    // writes of its own). The stop position holds the whole gate now, so the app's pause
    // is let go first: with C64_STOP set, ultimate_sys makes a MEMONLY write of $0000/$0001
    // a bus cycle (T16 Q8), which opens the gate for one edge and moves the machine off the
    // snap position the block is loaded on. The CPU walk's undo of the KERNAL IRQ's STA $01
    // did that (T20: $01F2 differed from the engine's APPLY, which only writes RAM).
    if (!err) {
        io_stop(false);
    }
    io_memonly(true);
    if (ram) {
        ram(ctx);
    }
    io_memonly(memonly);

    if (!err && !stage_and_load(w)) {
        err = E_STAGE;
    }
    if (err) {
        HW_WR(trx_statebus_load, 0);
    }

    // release: the hold of the stop position keeps the machine until b9
    io_stop(false);
    HW_WR(trx_gate_ctrl, sv.gate & GATE_FRZ);
    return err;
}

struct RestoreCtx {
    int idx;
    TrxRestoreStats *st;
};

// colour RAM as bus cycles (stopped, Ultimax); cartridge RAM through c64cart, then the cartridge drops its cached lines
static void restore_bus(void *ctx)
{
    RestoreCtx *c = (RestoreCtx *)ctx;
    Coll cl = { 0, 0, 0 };
    collect(c->idx, tl_n, false, true, &cl);
    c->st->rec1 = cl.n1;
    c->st->other = cl.other;
    c->st->bus_writes += each(col_tch, 1024, col_val, [](int a, uint8_t v) { c64_poke(0xD800 + a, v); });
    c->st->cart_bytes = cart_ram_put(NULL, cart_tch, cart_val);
    if (c->st->cart_bytes) {
        HW_WR(c64cart_cmd, DRV_c64cart_cmd_inval_MASK);
    }
}

// C64 RAM through the second port
static void restore_ram(void *ctx)
{
    RestoreCtx *c = (RestoreCtx *)ctx;
    Coll cl = { 0, 0, 0 };
    if (apply_hw) {
        // the entries' records newest first, by the engine
        n_runs = 0;
        plan_full = false;
        for (int j = tl_n - 1; j >= c->idx; j--) {
            TrxFrame &f = ent(j);
            plan_range(f.s0, f.e0 == OPEN ? cnt_now[0] : f.e0, false, &cl);
        }
        apply_err = apply_plan();
        c->st->rec0 = cl.n0;
        c->st->ram_writes = cl.n0;      // records applied (no coalescing)
        return;
    }
    collect(c->idx, tl_n, true, false, &cl);
    c->st->rec0 = cl.n0;
    c->st->ram_writes = each(ram_tch, 65536, ram_val, [](int a, uint8_t v) { c64_poke(a, v); });
}

static int restore_locked(int idx, TrxRestoreStats *st)
{
    uint16_t t0 = r_ms_timer();
    memset(st, 0, sizeof(*st));
    if (idx < 0 || idx >= tl_n) {
        return E_INDEX;
    }
    uint32_t seq = ent(idx).seq;
    if (dbg_handover()) {
        wait_reg(DRV_trx_gate_ctrl_ADDR, GATE_HALTED, GATE_HALTED, 100);
    }
    Saved sv;
    stop_machine(&sv, true);
    // One bus cycle with the log still on: the first DMA holds the CPU at its
    // next read, and the writes it still makes before that (a push sequence,
    // a read-modify-write) are logged and undone with the rest.
    io_memonly(false);
    (void)c64_peek(0x0002);
    drain();
    parse_locked();                 // up to the stop (trim() may drop from the front)
    idx = -1;
    for (int i = tl_n - 1; i >= 0; i--) {
        if (ent(i).seq == seq) {
            idx = i;
            break;
        }
    }
    int err = E_NONE;
    if (idx < 0) {
        err = E_GONE;
    } else if (!ent(idx).block) {
        err = E_NOBLOCK;
    } else if (!reachable(ent(idx))) {
        err = E_GONE;
    }
    if (err) {
        io_stop(false);
        HW_WR(trx_gate_ctrl, sv.gate & GATE_FRZ);
        if (HW_RD(trx_gate_stop_pos) & POS_HOLDING) {
            HW_WR(trx_gate_stop_pos, POS_RELEASE);
        }
        return err;
    }
    HW_WR(trx_engine_log_ctrl, 0);
    HW_WR(trx_gate_cap_ctrl, 0);
    log_on = false;

    TrxFrame f = ent(idx);
    st->frames = tl_n - 1 - idx;
    st->ms_stop = uint16_t(r_ms_timer() - t0);
    RestoreCtx rc;
    rc.idx = idx;
    rc.st = st;
    uint16_t tpos = t0;
    bool latch_ok = true;
    apply_err = E_NONE;
    err = put_back(f.w, f.x, sv, restore_bus, restore_ram, &rc, &st->bus_writes, &tpos, &latch_ok);
    if (!err) {
        err = apply_err;
    }
    st->vic_latches = latch_ok;
    st->ms_pos = uint16_t(tpos - t0);

    // the frames after it are cut; the re-run of the entry's frame logs from
    // the counts of now on
    tl_n = idx + 1;
    TrxFrame &e = ent(idx);
    e.s0 = cnt_now[0];
    e.s1 = cnt_now[1];
    e.e0 = e.e1 = OPEN;
    rebase_cycles(e, HW_RD(trx_dbg_run_cycles) + 1);   // + the load edge itself
    HW_WR(trx_gate_cap_ctrl, 1);
    HW_WR(trx_engine_log_ctrl, logctl(true));
    log_on = true;
    HW_WR(trx_gate_stop_pos, POS_RELEASE);     // go on: the load is the first edge
    st->ms = uint16_t(r_ms_timer() - t0);
    st->next_seq = next_seq;
    r_log("[RING] restore to entry %d (seq %u): %u frames, %u + %u records, %u RAM / %u bus writes, "
           "%u ms (stopped at %u, at the position %u), %s\n",
           idx, (unsigned)f.seq, (unsigned)st->frames, (unsigned)st->rec0, (unsigned)st->rec1,
           (unsigned)st->ram_writes, (unsigned)st->bus_writes, (unsigned)st->ms, (unsigned)st->ms_stop,
           (unsigned)st->ms_pos, trx_ring_error(err));
    return err;
}

// ---- scrub (TRX64-Ultimate docs/specs/T20-delta-ring.md §16.7) ------------------
// Timewarp moves the paused machine from the entry it shows (D) to another (T)
// and lets it draw that frame: the writes since the last step undone, D -> T
// by the logged values (back: old bytes, newest first; forward: new bytes,
// oldest first; the program never re-runs to go forward), T's block put in as
// a restore puts it, one frame run with the log on, a stop on line 0 of the
// next frame. That frame's writes stay in the log until the next step (or the
// final rewind) takes them back, so between steps the machine is "D plus one
// frame" and the still capture holds the picture the VIC drew from D.
//
// The walks mark pages, so a step costs its records, not 64 KB scans.
#define SCRUB_STOP_CYC 4        // line 0 cycle 4: before every snap (x >= 96)

static struct {
    Saved sv;               // the freeze's gate and sessions, for the final release
    uint8_t mode;           // C64_MODE and the DMA mode of the menu, put back after every step
    bool memonly;
    bool d_now;             // D is the freeze point (the closed head's end)
    uint32_t d_seq;         // else D's entry
    uint32_t mark[2];       // stream 0/1 counts since which the log holds writes not undone
    uint32_t land_c24;      // the debug block's CYCLES after a land's load edge (T21)
} scr;
static uint8_t ram_pg[256], cart_pg[256];

// records [from, to) of stream s: redo (oldest first, new bytes) or undo
// (newest first, old bytes); the last store per address wins either way
static void walk_range(int s, uint32_t from, uint32_t to, bool redo, Coll *c)
{
    const uint32_t rs = rec_size[s];
    const uint32_t chunk = (BOUNCE - 32) / rs;
    uint8_t *const rv = ram_val, *const rt = ram_tch;
    uint8_t *const kv = cart_val, *const kt = cart_tch;
    uint32_t left = (to - from) & M28;
    while (left) {
        uint32_t n = left < chunk ? left : chunk;
        uint32_t start = redo ? from : ((to - n) & M28);
        const uint32_t *wp = (const uint32_t *)fetch(s, start, n);
        int k = redo ? 0 : int(n) - 1;
        const int step = redo ? 1 : -1;
        if (s == 0) {
            const int sh = redo ? 24 : 16;
            for (uint32_t i = 0; i < n; i++, k += step) {
                uint32_t r = wp[k];
                uint32_t a = r & 0xFFFF;
                rv[a] = uint8_t(r >> sh);
                rt[a] = 1;
                ram_pg[a >> 8] = 1;
            }
            c->n0 += n;
        } else {
            const int sh = redo ? 8 : 0;
            for (uint32_t i = 0; i < n; i++, k += step) {
                uint32_t lo = wp[2 * k];
                uint8_t v = uint8_t(wp[2 * k + 1] >> sh);
                uint32_t a = lo & 0xFFFF;
                switch (lo >> 28) {
                case 1:
                    col_val[a & 0x3FF] = v & 0x0F;
                    col_tch[a & 0x3FF] = 1;
                    break;
                case 2:
                    kv[a] = v;
                    kt[a] = 1;
                    cart_pg[a >> 8] = 1;
                    break;
                default:
                    c->other++;
                    break;
                }
            }
            c->n1 += n;
        }
        left -= n;
        if (redo) {
            from = (from + n) & M28;
        } else {
            to = (to - n) & M28;
        }
    }
}

// entries [lo, hi): undo newest first, or redo oldest first
static void walk_entries(int lo, int hi, bool redo, Coll *c)
{
    for (int i = 0; i < hi - lo; i++) {
        TrxFrame &f = ent(redo ? lo + i : hi - 1 - i);
        plan_range(f.s0, f.e0 == OPEN ? cnt_now[0] : f.e0, redo, c);
        walk_range(1, f.s1, f.e1 == OPEN ? cnt_now[1] : f.e1, redo, c);
    }
}

// the marked bytes of a 64 KB space, page by page; the marks are cleared
template <typename F> static uint32_t each_pg(uint8_t *pg, uint8_t *tch, const uint8_t *val, F put)
{
    uint32_t n = 0;
    for (int p = 0; p < 256; p++) {
        if (!pg[p]) {
            continue;
        }
        pg[p] = 0;
        uint32_t *w = (uint32_t *)(tch + 256 * p);
        for (int i = 0; i < 64; i++) {
            uint32_t m = w[i];
            if (!m) {
                continue;
            }
            w[i] = 0;
            for (int b = 0; b < 4; b++) {
                if (m & (0xFFu << (8 * b))) {
                    int a = 256 * p + 4 * i + b;
                    put(a, val[a]);
                    n++;
                }
            }
        }
    }
    return n;
}

struct ScrubCtx {
    TrxRestoreStats *st;
    const uint32_t *w;      // the block being put in
};

// colour RAM as bus cycles, cartridge RAM through c64cart (as restore_bus)
static void scrub_bus(void *ctx)
{
    TrxRestoreStats *st = ((ScrubCtx *)ctx)->st;
    for (int a = 0; a < 1024; a++) {
        if (col_tch[a]) {
            c64_poke(0xD800 + a, col_val[a]);
            st->bus_writes++;
        }
    }
    memset(col_tch, 0, 1024);
    // The frame run may have left IRQ or collision latches the block's frame
    // had clear, and the run to the snap position is only a few cycles from
    // the scrub's stop (too short for clear_vic_latches): cleared here.
    const uint32_t *w = ((ScrubCtx *)ctx)->w;
    uint8_t clr = ~uint8_t(w[W_VIC + 6] >> 8) & 0x0F;
    if (clr) {
        c64_poke(0xD019, clr);
    }
    if (uint8_t(w[W_VIC + 7] >> 16) == 0) {
        (void)c64_peek(0xD01E);
    }
    if (uint8_t(w[W_VIC + 7] >> 24) == 0) {
        (void)c64_peek(0xD01F);
    }
    st->bus_writes += 3;
    st->cart_bytes = cart_ram_put(cart_pg, cart_tch, cart_val);
    if (st->cart_bytes) {
        HW_WR(c64cart_cmd, DRV_c64cart_cmd_inval_MASK);
    }
}

// C64 RAM through the second port
static void scrub_ram(void *ctx)
{
    TrxRestoreStats *st = ((ScrubCtx *)ctx)->st;
    if (apply_hw) {
        apply_err = apply_plan();       // the plan scrub_walk_locked made
        st->ram_writes = st->rec0;
        return;
    }
    st->ram_writes = each_pg(ram_pg, ram_tch, ram_val, [](int a, uint8_t v) { c64_poke(a, v); });
}

// The CIA 1 port directions the keyboard scan needs (C64::init_io sets them
// in the freeze session; without one the scan drives the real CIA). Every
// step's and the final load replays the block's. The slim core's menu scans
// the hardware matrix (KEYB_COL / KEYB_ROW) and the CIA stays the program's.
static void scrub_keyboard_io(void)
{
    if (trx_io_slim()) {
        return;
    }
    io_memonly(false);
    c64_set_mode(DRV_cart_regs_mode_ultimax);
    c64_poke(0xDC02, 0xFF);
    c64_poke(0xDC03, 0x00);
    c64_set_mode(scr.mode);
    io_memonly(scr.memonly);
}

// The first step: the freeze session ends (the machine pause stays), the head
// is closed at the counts of now, capture off. The log stays on: what the
// menu writes from here on is undone by the next step.
static int scrub_begin_locked(void)
{
    parse_locked();
    if (!tl_n) {
        return E_INDEX;
    }
    scr.mode = CART_RD(mode);
    scr.memonly = HW_RD(c64_dma_mode_memonly) != 0;
    stop_machine(&scr.sv, true);
    io_memonly(false);
    (void)c64_peek(0x0002);          // as restore_locked: the CPU's last writes logged
    drain();
    parse_locked();
    TrxFrame &h = ent(tl_n - 1);
    if (h.e0 == OPEN) {
        h.e0 = cnt_now[0];
        h.e1 = cnt_now[1];
    }
    scr.mark[0] = cnt_now[0];
    scr.mark[1] = cnt_now[1];
    HW_WR(trx_gate_cap_ctrl, 0);
    log_on = false;                 // the ring records nothing now: the poll leaves it alone
    memset(ram_tch, 0, 65536);
    memset(cart_tch, 0, 65536);
    memset(col_tch, 0, 1024);
    memset(ram_pg, 0, sizeof(ram_pg));
    memset(cart_pg, 0, sizeof(cart_pg));
    scr.d_now = true;
    scrubbing = true;
    scrub_keyboard_io();
    return E_NONE;
}

// Log off; the writes since the mark undone and D -> T walked into the marks.
static int scrub_walk_locked(int idx, Coll *cl)
{
    drain();
    HW_WR(trx_engine_log_ctrl, 0);
    cnt_now[0] = HW_RD_I(trx_engine_log_cnt, 0) & M28;
    cnt_now[1] = HW_RD_I(trx_engine_log_cnt, 1) & M28;
    n_runs = 0;
    plan_full = false;
    plan_range(scr.mark[0], cnt_now[0], false, cl);
    walk_range(1, scr.mark[1], cnt_now[1], false, cl);
    scr.mark[0] = cnt_now[0];
    scr.mark[1] = cnt_now[1];
    int d = tl_n;
    if (!scr.d_now) {
        d = -1;
        for (int i = tl_n - 1; i >= 0; i--) {
            if (ent(i).seq == scr.d_seq) {
                d = i;
                break;
            }
        }
        if (d < 0) {
            return E_GONE;          // cannot happen with SCRUB_MARGIN
        }
    }
    if (idx < d) {
        walk_entries(idx, d, false, cl);
    } else if (idx > d) {
        walk_entries(d, idx, true, cl);
    }
    return E_NONE;
}

static int scrub_show_locked(int idx, TrxRestoreStats *st)
{
    uint16_t t0 = r_ms_timer();
    memset(st, 0, sizeof(*st));
    if (!scrubbing) {
        int err = scrub_begin_locked();
        if (err) {
            return err;
        }
    }
    if (idx < 0 || idx >= tl_n || !ent(idx).block || !reachable(ent(idx), SCRUB_MARGIN)) {
        return E_NOBLOCK;
    }
    Coll cl = { 0, 0, 0 };
    int err = scrub_walk_locked(idx, &cl);
    if (err) {
        return err;
    }
    st->rec0 = cl.n0;
    st->rec1 = cl.n1;
    st->other = cl.other;
    TrxFrame f = ent(idx);
    ScrubCtx sc = { st, f.w };
    Saved pv = { GATE_FRZ, true };  // released into a running machine
    apply_err = E_NONE;
    err = put_back(f.w, f.x, pv, scrub_bus, scrub_ram, &sc, &st->bus_writes);
    if (!err) {
        err = apply_err;
    }
    st->ms_pos = uint16_t(r_ms_timer() - t0);

    // one frame with the log on, stopped on line 0 of the next one
    uint32_t still_n = ((HW_RD(trx_pixel_still_status) & DRV_trx_pixel_still_status_count_MASK) >> DRV_trx_pixel_still_status_count_SHIFT);
    HW_WR(trx_engine_log_ctrl, logctl(true));
    HW_WR(trx_gate_stop_pos, POS_ARM | POS_RELEASE | SCRUB_STOP_CYC);
    if (!wait_reg(DRV_trx_gate_stop_pos_ADDR, POS_HOLDING, POS_HOLDING, 60) && !err) {
        err = E_NOPOS;
    }
    HW_WR(trx_gate_ctrl, GATE_FRZ);
    io_stop(true);
    wait_reg(DRV_trx_gate_ctrl_ADDR, GATE_HALTED, GATE_HALTED, 100);
    io_memonly(false);
    (void)c64_peek(0x0002);          // the CPU's last writes into the log too
    c64_set_mode(scr.mode);
    io_memonly(scr.memonly);
    scrub_keyboard_io();
    drain();
    p2 = HW_RD_I(trx_engine_log_cnt, 2) & M28;     // the run's markers are no timeline entries
    cnt_now[0] = HW_RD_I(trx_engine_log_cnt, 0) & M28;
    cnt_now[1] = HW_RD_I(trx_engine_log_cnt, 1) & M28;
    scr.d_now = false;
    scr.d_seq = f.seq;
    trim();                         // the run used one frame of the areas
    // the still of that frame: complete at line 285 (60 Hz modes: line 262), long before the stop
    uint16_t t1 = r_ms_timer();
    while ((((HW_RD(trx_pixel_still_status) & DRV_trx_pixel_still_status_count_MASK) >> DRV_trx_pixel_still_status_count_SHIFT)) == still_n && uint16_t(r_ms_timer() - t1) < 10)
        ;
    st->frames = 1;
    st->ms = uint16_t(r_ms_timer() - t0);
    r_log("[RING] scrub to seq %u: %u + %u records, %u RAM / %u bus writes, %u ms (%u to the position) %s\n",
           (unsigned)f.seq, (unsigned)st->rec0, (unsigned)st->rec1, (unsigned)st->ram_writes,
           (unsigned)st->bus_writes, (unsigned)st->ms, (unsigned)st->ms_pos, trx_ring_error(err));
    return err;
}

// RETURN / RUN/STOP: the machine onto entry idx from the scrub's state, the
// timeline cut after it, log and capture on, release (restore_locked's end).
static int scrub_final_locked(int idx, TrxRestoreStats *st)
{
    uint16_t t0 = r_ms_timer();
    memset(st, 0, sizeof(*st));
    int err = E_NONE;
    Coll cl = { 0, 0, 0 };
    if (idx < 0 || idx >= tl_n || !ent(idx).block) {
        err = E_NOBLOCK;
    } else {
        err = scrub_walk_locked(idx, &cl);
    }
    scrubbing = false;
    if (err) {
        // nothing to go back to: the log and capture back on, the machine
        // released as it stands
        HW_WR(trx_gate_cap_ctrl, 1);
        HW_WR(trx_engine_log_ctrl, logctl(true));
        log_on = true;
        c64_set_mode(DRV_cart_regs_mode_normal);
        io_stop(false);
        HW_WR(trx_gate_ctrl, scr.sv.gate & GATE_FRZ);
        HW_WR(trx_gate_stop_pos, POS_RELEASE);
        return err;
    }
    st->rec0 = cl.n0;
    st->rec1 = cl.n1;
    st->other = cl.other;
    TrxFrame f = ent(idx);
    ScrubCtx sc = { st, f.w };
    bool latch_ok = true;
    apply_err = E_NONE;
    err = put_back(f.w, f.x, scr.sv, scrub_bus, scrub_ram, &sc, &st->bus_writes, NULL, &latch_ok);
    if (!err) {
        err = apply_err;
    }
    st->vic_latches = latch_ok;
    tl_n = idx + 1;
    TrxFrame &e = ent(idx);
    e.s0 = cnt_now[0];
    e.s1 = cnt_now[1];
    e.e0 = e.e1 = OPEN;
    p2 = HW_RD_I(trx_engine_log_cnt, 2) & M28;
    rebase_cycles(e, HW_RD(trx_dbg_run_cycles) + 1);
    HW_WR(trx_gate_cap_ctrl, 1);
    HW_WR(trx_engine_log_ctrl, logctl(true));
    log_on = true;
    HW_WR(trx_gate_stop_pos, POS_RELEASE);
    st->ms = uint16_t(r_ms_timer() - t0);
    st->next_seq = next_seq;
    r_log("[RING] scrub rewind to seq %u: %u + %u records, %u ms, %s\n", (unsigned)f.seq,
           (unsigned)st->rec0, (unsigned)st->rec1, (unsigned)st->ms, trx_ring_error(err));
    return err;
}

// ---- the debugger's land and commit (T21 §4.10) ---------------------------------
// The scrub's walk and put-back, but the machine ends standing on the entry's
// snap edge itself: the debug block's CYC_CMP is armed for the next edge while
// the machine holds one edge before it (STOP_POS), so the release lets exactly
// the load edge through. The debugger owns run control (OWNER), which takes the
// app's C64_STOP away from the gate; for the restore it is handed back to
// C64_STOP (which already holds the machine) and taken again before the release.

static int land_locked(int idx, TrxRestoreStats *st)
{
    uint16_t t0 = r_ms_timer();
    memset(st, 0, sizeof(*st));
    uint32_t gate0 = HW_RD(trx_gate_ctrl) & GATE_FRZ;
    // C64_STOP takes the machine over from the debugger's hold
    bool owned = dbg_handover();
    HW_WR(trx_gate_ctrl, GATE_FRZ);
    io_stop(true);
    wait_reg(DRV_trx_gate_ctrl_ADDR, GATE_HALTED, GATE_HALTED, 100);
    int err = E_NONE;
    if (!scrubbing) {
        err = scrub_begin_locked();
        scr.sv.gate = gate0;        // what commit puts back
    }
    if (!err && (idx < 0 || idx >= tl_n || !ent(idx).block || !reachable(ent(idx), SCRUB_MARGIN))) {
        err = E_NOBLOCK;
    }
    Coll cl = { 0, 0, 0 };
    if (!err) {
        err = scrub_walk_locked(idx, &cl);
    }
    if (err) {
        // the machine stays where it is, held by the debugger again
        HW_WR(trx_dbg_run_owner, owned ? 1 : 0);
        HW_WR(trx_debug_cmd, DRV_trx_debug_cmd_pause_cb);   // the gate is shut, a record at once
        io_stop(false);
        if (!scrubbing) {
            HW_WR(trx_gate_ctrl, gate0);
        }
        return err;
    }
    st->rec0 = cl.n0;
    st->rec1 = cl.n1;
    st->other = cl.other;
    TrxFrame f = ent(idx);
    ScrubCtx sc = { st, f.w };
    Saved pv = { GATE_FRZ, true };
    apply_err = E_NONE;
    err = put_back(f.w, f.x, pv, scrub_bus, scrub_ram, &sc, &st->bus_writes);
    if (!err) {
        err = apply_err;
    }
    st->ms_pos = uint16_t(r_ms_timer() - t0);
    // the machine holds one edge before the snap's phi2 fall: CYC_CMP on that edge
    uint32_t now = HW_RD(trx_dbg_run_cycles);
    HW_WR(trx_dbg_run_cyc_cmp, now);
    scr.land_c24 = now + 1;         // the count after the load edge, as F2 has it after a snap
    ep_push(f.cycles, scr.land_c24);
    HW_WR(trx_dbg_run_owner, 1);
    // no bus cycle from here on: it would open the clock before the release
    c64_set_mode(scr.mode);
    io_memonly(scr.memonly);
    HW_WR(trx_engine_log_ctrl, logctl(true));      // what the debugger runs from here is undone by the next land
    HW_WR(trx_gate_stop_pos, POS_RELEASE);
    if (!wait_reg(DRV_trx_debug_status_ADDR, DBG_ST_NEW, DBG_ST_NEW, 100) && !err) {
        err = E_NOPOS;
    }
    if (!owned) {
        HW_WR(trx_dbg_run_owner, 0);
    }
    p2 = HW_RD_I(trx_engine_log_cnt, 2) & M28;
    scr.d_now = false;
    scr.d_seq = f.seq;
    st->frames = tl_n - 1 - idx;
    st->ms = uint16_t(r_ms_timer() - t0);
    r_log("[RING] land on seq %u: %u + %u records, %u RAM / %u bus writes, %u ms, %s\n", (unsigned)f.seq,
           (unsigned)st->rec0, (unsigned)st->rec1, (unsigned)st->ram_writes, (unsigned)st->bus_writes,
           (unsigned)st->ms, trx_ring_error(err));
    return err;
}

static void commit_locked(void)
{
    if (!scrubbing) {
        return;
    }
    scrubbing = false;
    int d = -1;
    if (!scr.d_now) {
        for (int i = tl_n - 1; i >= 0; i--) {
            if (ent(i).seq == scr.d_seq) {
                d = i;
                break;
            }
        }
    }
    if (d >= 0) {
        tl_n = d + 1;
        TrxFrame &e = ent(d);
        e.s0 = scr.mark[0];
        e.s1 = scr.mark[1];
        e.e0 = e.e1 = OPEN;
        rebase_cycles(e, scr.land_c24);
    }
    p2 = HW_RD_I(trx_engine_log_cnt, 2) & M28;
    HW_WR(trx_gate_cap_ctrl, 1);
    HW_WR(trx_engine_log_ctrl, logctl(true));
    log_on = true;
    HW_WR(trx_gate_ctrl, scr.sv.gate & GATE_FRZ);
    r_log("[RING] commit: the timeline goes on from seq %u\n", d >= 0 ? (unsigned)ent(d).seq : 0u);
}

int trx_ring_land(int index, TrxRestoreStats *stats)
{
    TrxRestoreStats dummy;
    if (!stats) {
        stats = &dummy;
    }
    if (!lock()) {
        return E_ABSENT;
    }
    if (reu_on()) {
        unlock();
        return E_REU;
    }
    int err = land_locked(index, stats);
    unlock();
    return err;
}

void trx_ring_commit(void)
{
    if (!lock()) {
        return;
    }
    commit_locked();
    unlock();
}

uint32_t trx_ring_landed(void)
{
    return (scrubbing && !scr.d_now) ? scr.d_seq : 0;
}

bool trx_ring_reachable(int index)
{
    if (!lock()) {
        return false;
    }
    bool ok = index >= 0 && index < tl_n && ent(index).block && reachable(ent(index), scrubbing ? SCRUB_MARGIN : 0);
    unlock();
    return ok;
}

// ---- materialize -----------------------------------------------------------------
// idx[] oldest first. From the head's copy back: the newest target first, each
// older one from the one after it, so the records are walked once. The walk is
// kept in a TrxMat between targets, so a caller can take them one per call
// (trx_ring_mat_next, a stepped .c64rering dump) or all in one. A second walk of
// stream 1 alone gives the targets' colour RAM ahead of the first
// (trx_ring_mat_col_next).
struct TrxMat {
    int n;                  // targets
    int i;                  // the next target to give, n - 1 down to 0
    int hi;                 // the entry the next walk goes down from
    int *at;                // the targets' timeline indices after begin's parse
    uint32_t *seqs;
    uint8_t *ram, *col, *cart;      // the working copies, walked back target by target
    // the colour walk: the next target, the entry it goes down from, its working copy
    int ci, chi;
    uint8_t ccol[1024];
    // the timeline as begin left it: a target asked for after it changed answers E_MOVED
    int tl_n;
    uint32_t head_seq, cnt0, cnt1;
};

static void mat_free(TrxMat *m)
{
    if (m) {
        r_free(m->at);
        r_free(m->seqs);
        r_free(m->ram);
        r_free(m->col);
        r_free(m->cart);
        r_free(m);
    }
}

static int mat_begin_locked(const int *idx, int n, bool carts, TrxMat **out)
{
    *out = NULL;
    for (int i = 0; i < n; i++) {
        if (idx[i] < 0 || idx[i] >= tl_n || (i && idx[i] < idx[i - 1])) {
            return E_INDEX;
        }
    }
    TrxMat *m = (TrxMat *)r_calloc(1, sizeof(TrxMat));
    if (!m) {
        return E_MEM;
    }
    m->ram = (uint8_t *)r_malloc(65536);
    m->col = (uint8_t *)r_malloc(1024);
    m->cart = carts ? (uint8_t *)r_malloc(65536) : NULL;
    m->seqs = (uint32_t *)r_malloc(n * sizeof(uint32_t));
    m->at = (int *)r_malloc(n * sizeof(int));
    if (!m->ram || !m->col || (carts && !m->cart) || !m->seqs || !m->at) {
        mat_free(m);
        return E_MEM;
    }
    uint8_t *const ram = m->ram, *const col = m->col, *const cart = m->cart;
    uint32_t *const seqs = m->seqs;
    int *const at = m->at;
    // a pause (anywhere): RAM through the second port, colour RAM through the
    // freeze session (answered without a C64 cycle)
    uint32_t gate = HW_RD(trx_gate_ctrl);
    bool was = (gate & GATE_HALTED) != 0;
    if (!was) {
        HW_WR(trx_gate_ctrl, GATE_FRZ);
        io_stop(true);
        wait_reg(DRV_trx_gate_ctrl_ADDR, GATE_HALTED, GATE_HALTED, 100);
        c64_set_mode(DRV_cart_regs_mode_ultimax);
    }
    drain();
    bool memonly = HW_RD(c64_dma_mode_memonly) != 0;
    io_memonly(true);
    g_api->c64->dma_read(0, ram, 65536);
    io_memonly(false);
    for (int a = 0; a < 1024; a++) {
        col[a] = c64_peek(0xD800 + a) & 0x0F;
    }
    io_memonly(memonly);
    if (cart) {
        // the cartridge's RAM, 64 KB, through c64cart (zeros without the export)
        const C64CartApi *cc = R_TABLE(C64CartApi, RX_C64CART, ram_read);
        if (cc) {
            cc->ram_read(0, cart, 65536);
        } else {
            memset(cart, 0, 65536);
        }
    }
    // the parse may drop entries from the front: find the targets again
    for (int i = 0; i < n; i++) {
        seqs[i] = ent(idx[i]).seq;
    }
    parse_locked();
    for (int i = 0; i < n; i++) {
        at[i] = -1;
        for (int j = 0; j < tl_n; j++) {
            if (ent(j).seq == seqs[i]) {
                at[i] = j;
                break;
            }
        }
    }
    if (!was) {
        c64_set_mode(DRV_cart_regs_mode_normal);
        io_stop(false);
        HW_WR(trx_gate_ctrl, gate & GATE_FRZ);
    }
    for (int i = 0; i < n; i++) {
        if (at[i] < 0 || !reachable(ent(at[i]))) {
            mat_free(m);
            return E_GONE;
        }
    }
    m->n = n;
    m->i = n - 1;
    m->hi = tl_n;
    m->ci = n - 1;
    m->chi = tl_n;
    memcpy(m->ccol, col, 1024);
    m->tl_n = tl_n;
    m->head_seq = tl_n ? ent(tl_n - 1).seq : 0;
    m->cnt0 = cnt_now[0];
    m->cnt1 = cnt_now[1];
    *out = m;
    return E_NONE;
}

// Target i still where begin found it, in a timeline begin left as it was.
static bool mat_same(const TrxMat *m, int i)
{
    return tl_n == m->tl_n && ent(tl_n - 1).seq == m->head_seq && cnt_now[0] == m->cnt0 && cnt_now[1] == m->cnt1 && ent(m->at[i]).seq == m->seqs[i];
}

// The next target, newest first: its position in idx[] goes to *which, its memories to ram / col and, when begin read the cartridge RAM, cart. A null
// buffer is not copied to: the target stays in the working copies until the next call (trx_ring_mat_mem).
static int mat_next_locked(TrxMat *m, int *which, uint8_t *ram_out, uint8_t *col_out, uint8_t *cart_out)
{
    const int i = m->i;
    if (i < 0) {
        return E_INDEX;
    }
    if (!mat_same(m, i)) {
        return E_MOVED;
    }
    uint8_t *const ram = m->ram, *const col = m->col, *const cart = m->cart;
    Coll cl = { 0, 0, 0 };
    collect(m->at[i], m->hi, true, true, &cl);
    each(ram_tch, 65536, ram_val, [ram](int a, uint8_t v) { ram[a] = v; });
    each(col_tch, 1024, col_val, [col](int a, uint8_t v) { col[a] = v; });
    if (ram_out) {
        memcpy(ram_out, ram, 65536);
    }
    if (col_out) {
        memcpy(col_out, col, 1024);
    }
    if (cart) {
        each(cart_tch, 65536, cart_val, [cart](int a, uint8_t v) { cart[a] = v; });
        if (cart_out) {
            memcpy(cart_out, cart, 65536);
        }
    }
    m->hi = m->at[i];
    m->i = i - 1;
    *which = i;
    return E_NONE;
}

// The colour records of stream 1 in [from, to), newest first, put straight into col: the oldest record of an address leaves its old value, which is
// what collect() and each() leave in the main walk's copy.
static void col_back(uint32_t from, uint32_t to, uint8_t *col)
{
    const uint32_t chunk = (BOUNCE - 32) / rec_size[1];
    while (((to - from) & M28) != 0) {
        uint32_t n = (to - from) & M28;
        if (n > chunk) {
            n = chunk;
        }
        uint32_t start = (to - n) & M28;
        const uint32_t *wp = (const uint32_t *)fetch(1, start, n);
        for (int k = int(n) - 1; k >= 0; k--) {
            uint32_t lo = wp[2 * k];
            if ((lo >> 28) == 1) {
                col[lo & 0x3FF] = uint8_t(wp[2 * k + 1]) & 0x0F;
            }
        }
        to = start;
    }
}

// The next target's colour RAM, newest first, from stream 1 alone (no C64 RAM is walked): its position in idx[] to *which, the walk's own copy of its
// 1024 nibbles to *col (valid until the next call). Independent of mat_next: the same records from the same head copy, so the same colour RAM mat_next
// gives that target.
static int mat_col_next_locked(TrxMat *m, int *which, const uint8_t **col)
{
    const int i = m->ci;
    if (i < 0) {
        return E_INDEX;
    }
    if (!mat_same(m, i)) {
        return E_MOVED;
    }
    for (int j = m->chi - 1; j >= m->at[i]; j--) {
        const TrxFrame &f = ent(j);
        col_back(f.s1, f.e1 == OPEN ? cnt_now[1] : f.e1, m->ccol);
    }
    *col = m->ccol;
    m->chi = m->at[i];
    m->ci = i - 1;
    *which = i;
    return E_NONE;
}

static int materialize_locked(const int *idx, int n, uint8_t **rams, uint8_t **cols, uint8_t **carts)
{
    TrxMat *m;
    int err = mat_begin_locked(idx, n, carts != NULL, &m);
    for (int k = n - 1; !err && k >= 0; k--) {
        int which;
        err = mat_next_locked(m, &which, rams[k], cols[k], carts ? carts[k] : NULL);
    }
    mat_free(m);
    return err;
}

// ---- API -------------------------------------------------------------------------
static void start_locked(void)
{
    HW_WR(trx_engine_log_ctrl, 0);
    tl_clear();
    ep_reset();
    p2 = 0;
    cyc_have = false;
    cyc_acc = 0;
    resyncs = 0;
    HW_WR(trx_gate_cap_ctrl, 1);
    HW_WR(trx_engine_log_ctrl, LOG_RESTART | logctl(false));
    log_rd = 0;                       // the counters start again: the observer log reader goes on from the new beginning
    log_on = true;
}

// 60 Hz modes (NTSC, PAL-60): 65 cycles x 263 lines at the NTSC clock; 50 Hz
// modes: 63 x 312 at the PAL clock
static bool fmt_60hz(void)
{
    return (HW_RD(c64_video_format) & DRV_c64_video_format_hz60_MASK) != 0;
}

bool trx_ring_apply_hw(int mode)
{
    if (mode == 0) {
        apply_hw = false;
    } else if (mode == 1) {
        apply_hw = (HW_RD(ident_caps2) & DRV_ident_caps2_apply_MASK) != 0;
    }
    return apply_hw;
}

int trx_ring_fps(void)
{
    return fmt_60hz() ? 60 : 50;
}

uint32_t trx_ring_fps_x100(void)
{
    return fmt_60hz() ? 5983 : 5012;
}

uint32_t trx_ring_frame_cycles(void)
{
    return fmt_60hz() ? 17095 : 19656;
}

bool trx_ring_start(void)
{
    if (!lock()) {
        return false;
    }
    if (reu_on()) {
        unlock();
        return false;
    }
    start_locked();
    unlock();
    return true;
}

const char *trx_ring_reu_refusal(void)
{
    return (present && reu_on()) ? TRX_REU_OFF_TEXT : NULL;
}

// The one place the ring follows the REU (A08). Called by every poll, which catches a change of the
// REU enable wherever it was made, and by whoever changes it. It never waits for the ring: a restore
// in progress holds the mutex, and the next poll looks again.
void trx_ring_reu_sync(void)
{
    if (!present || !g_api->sync->mutex_lock(mutex, 0)) {
        return;
    }
    switch (trx_reu_gate_step(&reu_gate, reu_on(), scrubbing, log_on)) {
    case TRX_REU_STOP:
        HW_WR(trx_engine_log_ctrl, 0);
        log_on = false;
        pending = -1;
        tl_clear();
        ep_reset();
        p2 = 0;
        cyc_have = false;
        cyc_acc = 0;
        resyncs = 0;
        r_log("[RING] REU enabled: ring off, frames dropped (A08)\n");
        break;
    case TRX_REU_START:
        start_locked();
        r_log("[RING] REU off: ring restarted from now (A08)\n");
        break;
    default:
        break;
    }
    g_api->sync->mutex_unlock(mutex);
}

void trx_ring_stop(void)
{
    if (!lock()) {
        return;
    }
    HW_WR(trx_engine_log_ctrl, 0);
    log_on = false;
    unlock();
}

void trx_ring_status(TrxRingStatus *st)
{
    memset(st, 0, sizeof(*st));
    if (!lock()) {
        return;
    }
    parse_locked();
    st->present = true;
    st->on = log_on;
    st->reu_off = reu_on();
    st->frames = tl_n;
    for (int i = 0; i < tl_n; i++) {
        if (ent(i).block) {
            st->targets++;
        }
    }
    if (tl_n) {
        st->oldest_seq = ent(0).seq;
        st->head_seq = ent(tl_n - 1).seq;
    }
    for (int s = 0; s < 3; s++) {
        st->log_cnt[s] = HW_RD_I(trx_engine_log_cnt, s) & M28;
    }
    st->dropped = HW_RD(trx_engine_dropped) & 0xFFFF;
    st->log_stat = HW_RD(trx_engine_log_stat);
    st->resyncs = resyncs;
    st->spurious_f4 = spurious_f4;
    unlock();
}

void trx_ring_poll(void)
{
    trx_ring_reu_sync();
    if (!lock()) {
        return;
    }
    if (log_on && (HW_RD(ident_caps2) & DRV_ident_caps2_turbo_MASK)) {
        // the engine ran faster than 1 MHz since the last look: its RAM writes are not in the log,
        // the ring cannot be walked back across them; start it over once the engine is off
        uint32_t ts = HW_RD(trx_turbo_stat);
        if ((ts & DRV_trx_turbo_stat_ran_MASK) && !(ts & DRV_trx_turbo_stat_on_MASK)) {
            HW_WR(trx_turbo_ctrl, DRV_trx_turbo_ctrl_ran_clr_MASK);
            start_locked();
            r_log("[RING] ring restarted: the machine ran in turbo\n");
        }
    }
    if (log_on) {
        parse_locked();
        // the splash's READY watch (trx_ui.cc) borrows CAP_CTRL; take it back
        if ((HW_RD(trx_gate_cap_ctrl) & 0xFF) != 1) {
            HW_WR(trx_gate_cap_ctrl, 1);
        }
    }
    unlock();
}

int trx_ring_count(void)
{
    if (!lock()) {
        return 0;
    }
    int n = tl_n;
    unlock();
    return n;
}

bool trx_ring_frame(int index, TrxFrame *out)
{
    if (!lock()) {
        return false;
    }
    bool ok = index >= 0 && index < tl_n;
    if (ok) {
        *out = ent(index);
    }
    unlock();
    return ok;
}

int trx_ring_target(int frames_back)
{
    if (!lock()) {
        return -1;
    }
    int i = tl_n - 1 - frames_back;
    if (i >= tl_n) {
        i = tl_n - 1;
    }
    uint32_t margin = scrubbing ? SCRUB_MARGIN : 0;
    while (i >= 0 && !(ent(i).block && reachable(ent(i), margin))) {
        i--;
    }
    unlock();
    return i;
}

int trx_ring_find(uint32_t seq)
{
    if (!lock()) {
        return -1;
    }
    int idx = -1;
    for (int i = tl_n - 1; i >= 0; i--) {
        if (ent(i).seq == seq) {
            idx = i;
            break;
        }
    }
    unlock();
    return idx;
}

int trx_ring_restore(int index, TrxRestoreStats *stats)
{
    TrxRestoreStats dummy;
    if (!stats) {
        stats = &dummy;
    }
    if (!lock()) {
        return E_ABSENT;
    }
    if (reu_on()) {
        unlock();
        return E_REU;
    }
    int err = restore_locked(index, stats);
    unlock();
    return err;
}

int trx_ring_scrub_show(int index, TrxRestoreStats *stats)
{
    TrxRestoreStats dummy;
    if (!stats) {
        stats = &dummy;
    }
    if (!lock()) {
        return E_ABSENT;
    }
    if (reu_on()) {
        unlock();
        return E_REU;
    }
    int err = scrub_show_locked(index, stats);
    unlock();
    return err;
}

bool trx_ring_scrubbing(void)
{
    return scrubbing;
}

int trx_ring_materialize(int index, uint8_t *ram, uint8_t *color, uint8_t *cart)
{
    return trx_ring_materialize_many(&index, 1, &ram, &color, cart ? &cart : NULL);
}

int trx_ring_materialize_many(const int *idx, int n, uint8_t **ram, uint8_t **color, uint8_t **carts)
{
    if (!lock()) {
        return E_ABSENT;
    }
    int err = materialize_locked(idx, n, ram, color, carts);
    unlock();
    return err;
}

int trx_ring_mat_begin(const int *idx, int n, bool carts, TrxMat **out)
{
    *out = NULL;
    if (!lock()) {
        return E_ABSENT;
    }
    int err = mat_begin_locked(idx, n, carts, out);
    unlock();
    return err;
}

int trx_ring_mat_next(TrxMat *m, int *which, uint8_t *ram, uint8_t *color, uint8_t *cart)
{
    if (!m) {
        return E_INDEX;
    }
    if (!lock()) {
        return E_ABSENT;
    }
    int err = mat_next_locked(m, which, ram, color, cart);
    unlock();
    return err;
}

int trx_ring_mat_col_next(TrxMat *m, int *which, const uint8_t **color)
{
    if (!m) {
        return E_INDEX;
    }
    if (!lock()) {
        return E_ABSENT;
    }
    int err = mat_col_next_locked(m, which, color);
    unlock();
    return err;
}

void trx_ring_mat_mem(TrxMat *m, uint8_t **ram, uint8_t **color)
{
    *ram = m ? m->ram : NULL;
    *color = m ? m->col : NULL;
}

void trx_ring_mat_end(TrxMat *m)
{
    mat_free(m);
}

void trx_ring_set_pending(int index)
{
    pending = index;
}

bool trx_ring_pending(void)
{
    return pending >= 0 || scrubbing;
}

static int scrub_final_locked(int idx, TrxRestoreStats *st);

void trx_ring_unfreeze(void)
{
    int idx = pending;
    pending = -1;
    if (scrubbing && lock()) {
        // a scrub has the machine: the final rewind starts from its state;
        // without a choice (RUN/STOP, any other unfreeze) the newest frame
        TrxRestoreStats st;
        if (idx < 0) {
            idx = tl_n - 1;
            while (idx >= 0 && !(ent(idx).block && reachable(ent(idx)))) {
                idx--;
            }
        }
        int err = scrub_final_locked(idx, &st);
        unlock();
        if (err) {
            r_log("[RING] rewind from the scrub failed: %s\n", trx_ring_error(err));
        }
        return;
    }
    TrxRestoreStats st;
    int err = trx_ring_restore(idx, &st);
    if (err) {
        // the machine is released either way (put_back or the early exit)
        r_log("[RING] rewind from the menu failed: %s\n", trx_ring_error(err));
    }
}

struct LoadCtx {
    const TrxLoadMem *m;
    uint32_t bus;
};

static void load_bus(void *ctx)
{
    LoadCtx *c = (LoadCtx *)ctx;
    for (int a = 0; a < 1024; a++) {
        c64_poke(0xD800 + a, c->m->col[a] & 0x0F);
    }
    c->bus += 1024;
    bool cart_touched = false;
    if (c->m->cart) {
        // the cartridge's RAM, whole, through c64cart (FW trx_ring.cc load_bus)
        if (const C64CartApi *cc = R_TABLE(C64CartApi, RX_C64CART, ram_write)) {
            cc->ram_write(0, c->m->cart, 65536);
            cart_touched = true;
        }
    }
    if (c->m->rom_put) {
        // the cartridge's image, which a rewind never puts back (the log has no flash writes): a game that programmed its own flash comes back whole
        c->m->rom_put(c->m->rom_ctx);
        cart_touched = true;
    }
    if (cart_touched) {
        HW_WR(c64cart_cmd, DRV_c64cart_cmd_inval_MASK);         // the cartridge drops its cached lines
    }
}

static void load_ram(void *ctx)
{
    LoadCtx *c = (LoadCtx *)ctx;
    g_api->c64->dma_write(0, c->m->ram, 65536);
}

int trx_ring_load_state(const TrxLoadMem *mem, const uint32_t *w, uint16_t x)
{
    if (!lock()) {
        return E_ABSENT;
    }
    if (reu_on()) {
        unlock();
        return E_REU;
    }
    if (dbg_handover()) {
        wait_reg(DRV_trx_gate_ctrl_ADDR, GATE_HALTED, GATE_HALTED, 100);
    }
    if (scrubbing) {
        scrubbing = false;          // the file replaces whatever a land or scrub held
    }
    Saved sv;
    stop_machine(&sv, true);
    drain();
    HW_WR(trx_engine_log_ctrl, 0);
    HW_WR(trx_gate_cap_ctrl, 0);
    LoadCtx c = { mem, 0 };
    int err = put_back(w, x, sv, load_bus, load_ram, &c, &c.bus);
    start_locked();                 // a file replaces the machine: the ring starts over
    HW_WR(trx_gate_stop_pos, POS_RELEASE);
    unlock();
    return err;
}

// ---- the native ring copy (FW trx_ring.cc trx_ring_area_*, trx_ring_install; .trxring) --------------------------
uint32_t trx_ring_area_size(int s)
{
    return (s >= 0 && s < 3) ? area_size[s] : 0;
}

uint32_t trx_ring_rec_size(int s)
{
    return (s >= 0 && s < 3) ? rec_size[s] : 0;
}

void trx_ring_area_read(int s, uint32_t off, void *dst, uint32_t len)
{
    if (s < 0 || s >= 3 || off + len > area_size[s]) {
        return;
    }
    eng_copy(area_base[s] + off, dst, len);
}

void trx_ring_area_write(int s, uint32_t off, const void *src, uint32_t len)
{
    if (s < 0 || s >= 3 || off + len > area_size[s]) {
        return;
    }
    // the mover's COPY takes two DDR2 byte addresses, and the app's own memory is in DDR2 too: the same call the other way round
    HW_WR(trx_engine_eng_src, (uint32_t)src);
    HW_WR(trx_engine_eng_dst, area_base[s] + off);
    HW_WR(trx_engine_eng_len, len);
    HW_WR(trx_engine_eng_cmd, DRV_trx_engine_eng_cmd_copy);
    uint16_t t0 = r_ms_timer();
    while (HW_RD(trx_engine_eng_cmd) & DRV_trx_engine_eng_cmd_busy_MASK) {
        if (uint16_t(r_ms_timer() - t0) > 200) {
            r_log("[RING] COPY back did not finish: %u bytes to area %d + 0x%x\n", (unsigned)len, s, (unsigned)off);
            break;
        }
    }
    __asm__ volatile ("fence" ::: "memory");
}

// The stream 2 record the parse reads next (the .trxring rebases its stream 2 counts on it)
uint32_t trx_ring_stream2_pos(void)
{
    return p2 & M28;
}

// A saved ring back into the engine and the machine: the log goes off and its counters to 0 first (the caller's area writes land where those counters
// point), then the timeline is installed, then the newest entry's state goes into the machine the way a restore puts a ring entry, and only then does
// the log run again, without a restart, so the records just written back stay the ring's history.
int trx_ring_install(const TrxFrame *ents, int n, const TrxLoadMem *mem, trx_area_fn areas, void *ctx)
{
    if (n < 1 || n > TL_CAP) {
        return E_INDEX;
    }
    if (!ents[n - 1].block) {
        return E_NOBLOCK;
    }
    if (!lock()) {
        return E_ABSENT;
    }
    if (reu_on()) {
        unlock();
        return E_REU;
    }
    if (dbg_handover()) {
        wait_reg(DRV_trx_gate_ctrl_ADDR, GATE_HALTED, GATE_HALTED, 100);
    }
    if (scrubbing) {
        scrubbing = false;
    }
    Saved sv;
    stop_machine(&sv, true);
    drain();
    HW_WR(trx_engine_log_ctrl, 0);
    HW_WR(trx_gate_cap_ctrl, 0);
    HW_WR(trx_engine_log_ctrl, LOG_RESTART);     // counts, positions and lost counters to 0, log still off
    int err = areas ? areas(ctx) : 0;
    if (!err) {
        tl_clear();
        for (int i = 0; i < n; i++) {
            tl[i] = ents[i];
        }
        tl_first = 0;
        tl_n = n;
        p2 = 0;                     // the newest entry's marker is the last record written back
        ep_reset();
        cyc_acc = ents[n - 1].cycles;
        cyc_last = ents[n - 1].c24;
        cyc_have = true;
        resyncs = 0;
        log_rd = 0;
        LoadCtx c = { mem, 0 };
        err = put_back(ents[n - 1].w, ents[n - 1].x, sv, load_bus, load_ram, &c, &c.bus);
    }
    if (!err) {
        HW_WR(trx_gate_cap_ctrl, 1);
        HW_WR(trx_engine_log_ctrl, logctl(false));   // on, without a restart: the history stays
        log_on = true;
    } else {
        tl_clear();                     // nothing half-installed stays reachable
        start_locked();
    }
    HW_WR(trx_gate_stop_pos, POS_RELEASE);
    unlock();
    return err;
}

// ---- the instruction trace (T21 §4.11) ---------------------------------------------
// Records are TRX64's DeltaEntry: three little-endian 64-bit words per instruction in
// stream 3 (the area at TR_AREA, TRACE_CAP records, trx_engine.v). What the app sees
// is the history of the timeline the machine is on (epochs above).
// Opcode lengths (b1/b2 hold whatever the bus returned; the bytes an opcode does not
// have read 0, as in TRX64)
static const uint8_t op_len[256] = {
    1, 2, 1, 2, 2, 2, 2, 2, 1, 2, 1, 2, 3, 3, 3, 3,
    2, 2, 1, 2, 2, 2, 2, 2, 1, 3, 1, 3, 3, 3, 3, 3,
    3, 2, 1, 2, 2, 2, 2, 2, 1, 2, 1, 2, 3, 3, 3, 3,
    2, 2, 1, 2, 2, 2, 2, 2, 1, 3, 1, 3, 3, 3, 3, 3,
    1, 2, 1, 2, 2, 2, 2, 2, 1, 2, 1, 2, 3, 3, 3, 3,
    2, 2, 1, 2, 2, 2, 2, 2, 1, 3, 1, 3, 3, 3, 3, 3,
    1, 2, 1, 2, 2, 2, 2, 2, 1, 2, 1, 2, 3, 3, 3, 3,
    2, 2, 1, 2, 2, 2, 2, 2, 1, 3, 1, 3, 3, 3, 3, 3,
    2, 2, 2, 2, 2, 2, 2, 2, 1, 2, 1, 2, 3, 3, 3, 3,
    2, 2, 1, 2, 2, 2, 2, 2, 1, 3, 1, 3, 3, 3, 3, 3,
    2, 2, 2, 2, 2, 2, 2, 2, 1, 2, 1, 2, 3, 3, 3, 3,
    2, 2, 1, 2, 2, 2, 2, 2, 1, 3, 1, 3, 3, 3, 3, 3,
    2, 2, 2, 2, 2, 2, 2, 2, 1, 2, 1, 2, 3, 3, 3, 3,
    2, 2, 1, 2, 2, 2, 2, 2, 1, 3, 1, 3, 3, 3, 3, 3,
    2, 2, 2, 2, 2, 2, 2, 2, 1, 2, 1, 2, 3, 3, 3, 3,
    2, 2, 1, 2, 2, 2, 2, 2, 1, 3, 1, 3, 3, 3, 3, 3,
};

static inline uint32_t tr_age(uint32_t p, uint32_t cnt) { return (cnt + TRACE_WRAP - p) % TRACE_WRAP; }
static inline uint32_t tr_add(uint32_t p, uint32_t d) { return (p + d) % TRACE_WRAP; }

// n records from physical index `from` into dst (24 bytes each, n <= TRACE_CHUNK); the
// area's end is not 16-byte aligned, so a run is copied on its own
static void trace_fetch(uint32_t from, uint32_t n, uint8_t *dst)
{
    while (n) {
        uint32_t slot = from % TRACE_CAP;
        uint32_t run = TRACE_CAP - slot;
        if (run > n) {
            run = n;
        }
        uint32_t off = slot * 24;
        uint32_t a0 = off & ~15u;
        uint32_t pre = off - a0;
        uint32_t len = (pre + run * 24 + 15) & ~15u;
        eng_copy(TR_AREA + a0, bounce, len);
        memcpy(dst, bounce + pre, run * 24);
        dst += run * 24;
        from = tr_add(from, run);
        n -= run;
    }
}

static inline uint64_t tr_tc(const Epoch &e, uint32_t c24)
{
    int32_t d = (int32_t)(((c24 - e.hw_ref) & 0xFFFFFF) << 8) >> 8;
    return (uint64_t)(e.tc_ref + d);
}

static void tr_decode(const uint8_t *p, const Epoch &e, TrxTraceRec *o)
{
    uint64_t w0 = rec64(p), w1 = rec64(p + 8), w2 = rec64(p + 16);
    o->cycle = tr_tc(e, uint32_t(w0) & 0xFFFFFF);
    o->write_start = uint32_t(w1);
    o->pc = uint16_t(w1 >> 32);
    o->write_count = uint16_t(w1 >> 48);
    o->a = uint8_t(w2);
    o->x = uint8_t(w2 >> 8);
    o->y = uint8_t(w2 >> 16);
    o->sp = uint8_t(w2 >> 24);
    o->p = uint8_t(w2 >> 32);
    o->opcode = uint8_t(w2 >> 40);
    uint8_t len = op_len[o->opcode];
    o->b1 = len >= 2 ? uint8_t(w2 >> 48) : 0;
    o->b2 = len >= 3 ? uint8_t(w2 >> 56) : 0;
}

// The first physical record of a closed epoch that is not history any more: the
// first one after the cut (the next epoch's tc_start; the instruction the cut stands
// on is kept: its fetch came before the snap, the next epoch has no record of it)
static void ep_keep(int i, uint32_t cnt)
{
    Epoch &E = ep[i];
    if (E.keep_known) {
        return;
    }
    E.keep_known = true;
    E.gap = false;
    uint32_t floor_age = TRACE_CAP - TRACE_SAFETY;
    uint32_t lo = E.phys0;
    uint32_t a0 = tr_age(lo, cnt);
    if (a0 > floor_age) {
        a0 = floor_age;
        lo = tr_add(cnt, TRACE_WRAP - floor_age);
    }
    uint32_t a1 = tr_age(ep[i + 1].phys0, cnt);
    uint32_t m = a0 > a1 ? a0 - a1 : 0;
    if (!E.anch || !m) {
        E.keep_end = lo;
        return;
    }
    uint64_t cut = ep[i + 1].tc_start;
    uint8_t tmp[24 + 16];
    uint32_t l = 0, h = m;      // the first k with tc(rec[lo + k]) > cut
    while (l < h) {
        uint32_t mid = (l + h) / 2;
        trace_fetch(tr_add(lo, mid), 1, tmp);
        uint64_t tc = tr_tc(E, uint32_t(rec64(tmp)) & 0xFFFFFF);
        if (tc > cut) {
            h = mid;
        } else {
            l = mid + 1;
        }
    }
    E.keep_end = tr_add(lo, l);
    if (l == 0) {
        E.gap = true;
    } else {
        trace_fetch(tr_add(lo, l - 1), 1, tmp);
        E.gap = tr_tc(E, uint32_t(rec64(tmp)) & 0xFFFFFF) + 8 < cut;
    }
}

struct TraceSeg {
    int e;
    uint32_t start, n;
};

// the history as physical runs, oldest first; total = records in all of them
static int trace_segs(TraceSeg *sg, uint32_t cnt, uint32_t *total, bool *gap)
{
    uint32_t floor_age = TRACE_CAP - TRACE_SAFETY;
    int ns = 0;
    *total = 0;
    *gap = false;
    for (int i = 0; i < ep_n; i++) {
        bool last = i == ep_n - 1;
        if (!last) {
            ep_keep(i, cnt);
        }
        uint32_t end = last ? cnt : ep[i].keep_end;
        uint32_t start = ep[i].phys0;
        uint32_t a0 = tr_age(start, cnt);
        if (a0 > floor_age) {
            a0 = floor_age;
            start = tr_add(cnt, TRACE_WRAP - floor_age);
        }
        uint32_t a1 = tr_age(end, cnt);
        uint32_t n = a0 > a1 ? a0 - a1 : 0;
        if (!n || !ep[i].anch) {
            continue;
        }
        if (!last && ep[i].gap) {
            *gap = true;
        }
        sg[ns].e = i;
        sg[ns].start = start;
        sg[ns].n = n;
        *total += n;
        ns++;
    }
    return ns;
}

static bool trace_ready(void)
{
    return present && (HW_RD(ident_caps2) & DRV_ident_caps2_trace_MASK) && tbuf;
}

bool trx_ring_trace_present(void)
{
    return trace_ready();
}

int trx_ring_trace_ctl(int on)
{
    if (!trace_ready() || !lock()) {
        return -1;
    }
    if (on >= 0) {
        trace_on = on != 0;
        if (log_on) {
            HW_WR(trx_engine_log_ctrl, logctl(false));
        }
    }
    bool r = trace_on;
    unlock();
    return r;
}

bool trx_ring_trace_status(TrxTraceStatus *st)
{
    memset(st, 0, sizeof(*st));
    if (!trace_ready() || !lock()) {
        return false;
    }
    parse_locked();
    st->present = true;
    st->on = trace_on && log_on;
    st->capacity = TRACE_CAP - TRACE_SAFETY;
    st->lost = HW_RD(trx_engine_dropped) & 0xFFFF;
    TraceSeg sg[MAX_EP];
    uint32_t cnt = trace_cnt(), total;
    bool gap;
    int ns = trace_segs(sg, cnt, &total, &gap);
    st->records = total;
    st->gap = gap;
    if (total) {
        uint8_t tmp[24 + 16];
        trace_fetch(sg[0].start, 1, tmp);
        st->oldest_cycle = tr_tc(ep[sg[0].e], uint32_t(rec64(tmp)) & 0xFFFFFF);
        trace_fetch(tr_add(sg[ns - 1].start, sg[ns - 1].n - 1), 1, tmp);
        st->newest_cycle = tr_tc(ep[sg[ns - 1].e], uint32_t(rec64(tmp)) & 0xFFFFFF);
    }
    unlock();
    return true;
}

// records first.. of the history (0 = oldest), n of them; returns how many
int trx_ring_trace_read(uint32_t first, uint32_t n, TrxTraceRec *out)
{
    if (!trace_ready() || !n || !lock()) {
        return 0;
    }
    parse_locked();
    TraceSeg sg[MAX_EP];
    uint32_t cnt = trace_cnt(), total;
    bool gap;
    int ns = trace_segs(sg, cnt, &total, &gap);
    uint32_t got = 0, base = 0;
    for (int i = 0; i < ns && got < n; i++) {
        if (first + got >= base + sg[i].n) {
            base += sg[i].n;
            continue;
        }
        uint32_t k = first + got - base;
        while (k < sg[i].n && got < n) {
            uint32_t m = sg[i].n - k;
            if (m > TRACE_CHUNK) {
                m = TRACE_CHUNK;
            }
            if (m > n - got) {
                m = n - got;
            }
            trace_fetch(tr_add(sg[i].start, k), m, tbuf);
            for (uint32_t j = 0; j < m; j++) {
                tr_decode(tbuf + 24 * j, ep[sg[i].e], &out[got + j]);
            }
            got += m;
            k += m;
        }
        base += sg[i].n;
    }
    unlock();
    return (int)got;
}

static inline bool s0_reach(uint32_t idx, uint32_t cnt0)
{
    return ((cnt0 - idx) & M28) <= area_size[0] / rec_size[0] - SAFETY;
}

// stream 0 records by physical index (the writes of an instruction: write_start,
// write_count); stops at the first one the area no longer holds
int trx_ring_trace_writes(uint32_t start, uint32_t n, TrxWriteRec *out)
{
    if (!trace_ready() || !n || !lock()) {
        return 0;
    }
    parse_locked();
    uint32_t got = 0;
    while (got < n) {
        uint32_t idx = (start + got) & M28;
        if (!s0_reach(idx, cnt_now[0])) {
            break;
        }
        uint32_t m = n - got;
        if (m > 8000) {
            m = 8000;
        }
        const uint8_t *p = fetch(0, idx, m);
        for (uint32_t j = 0; j < m; j++) {
            uint32_t v = rec32(p + 4 * j);
            out[got + j].addr = uint16_t(v);
            out[got + j].old_value = uint8_t(v >> 16);
            out[got + j].new_value = uint8_t(v >> 24);
        }
        got += m;
    }
    unlock();
    return (int)got;
}

// The debug block's log action (trx_dbg.v): the stream 1 records of space 8 that came since the last call, as the engine wrote them ([15:0] address, [23:16] data,
// [27:24] {0, IB, write, unit}, [31:28] 8, [47:32] IPC, [62:48] CYCLES low 15). max = 0 only moves the reader to the present. *lost counts records the area overwrote
// before they were read. PROVENANCE: trx_ring_log_read() of vendor/gideon/software/io/c64/trx_ring.cc (the TRX64 line, GPL-3.0), T104.2 UI.
int trx_ring_log_read(uint64_t *out, int max, uint32_t *lost)
{
    *lost = 0;
    if (!lock()) {
        return 0;
    }
    uint32_t cnt = HW_RD_I(trx_engine_log_cnt, 1) & M28;
    if (!log_have || !max || ((cnt - log_rd) & M28) > (M28 >> 1)) {
        log_rd = cnt;
        log_have = true;
        unlock();
        return 0;
    }
    uint32_t cap = area_size[1] / rec_size[1] - SAFETY;
    uint32_t avail = (cnt - log_rd) & M28;
    if (avail > cap) {
        *lost = avail - cap;
        log_rd = (cnt - cap) & M28;
        avail = cap;
    }
    int got = 0;
    while (avail && got < max) {
        uint32_t n = avail > TRACE_CHUNK ? TRACE_CHUNK : avail;
        const uint8_t *p = fetch(1, log_rd, n);
        uint32_t used = 0;
        for (; used < n && got < max; used++) {
            uint64_t r = rec64(p + 8 * used);
            if (((r >> 28) & 15) == 8 && !(r >> 63)) {
                out[got++] = r;
            }
        }
        log_rd = (log_rd + used) & M28;
        avail -= used;
    }
    unlock();
    return got;
}

// The last writers of addr in the history, newest first. *exhausted: the scan reached the
// start of the history (or of stream 0's area) with fewer than limit hits.
// An I/O address ($D000-$DFFF) is not in stream 0 (that is the RAM write log): its writers are the store
// instructions of the trace itself, by effective address (trx_ea.h). The record carries no old byte, so the
// hit says so (TRX_HIT_OLD_UNKNOWN), and no new byte for a read-modify-write. *unresolved counts the
// instructions that write through a zero page pointer (or an SH* store over a page crossing): their address is
// not in the record, they are not guessed.
int trx_ring_trace_whowrote(uint16_t addr, int limit, TrxTraceHit *hits, bool *exhausted, uint32_t *unresolved)
{
    *exhausted = true;
    *unresolved = 0;
    if (!trace_ready() || limit < 1 || !lock()) {
        return 0;
    }
    parse_locked();
    TraceSeg sg[MAX_EP];
    uint32_t cnt = trace_cnt(), total;
    bool gap;
    int ns = trace_segs(sg, cnt, &total, &gap);
    int found = 0;
    uint32_t lbase = total;
    if (addr >= 0xD000 && addr <= 0xDFFF) {
        for (int si = ns - 1; si >= 0 && found < limit; si--) {
            lbase -= sg[si].n;
            uint32_t remaining = sg[si].n;
            while (remaining && found < limit) {
                uint32_t m = remaining < TRACE_CHUNK ? remaining : TRACE_CHUNK;
                uint32_t k0 = remaining - m;
                trace_fetch(tr_add(sg[si].start, k0), m, tbuf);
                for (int j = (int)m - 1; j >= 0 && found < limit; j--) {
                    TrxTraceRec r;
                    tr_decode(tbuf + 24 * j, ep[sg[si].e], &r);
                    trx_store_t st;
                    int k = trx_store_ea(r.opcode, r.b1, r.b2, r.a, r.x, r.y, &st);
                    if (k == TRX_EA_UNRESOLVED) {
                        (*unresolved)++;
                        continue;
                    }
                    if (k != TRX_EA_KNOWN || st.ea != addr) {
                        continue;
                    }
                    TrxTraceHit &h = hits[found++];
                    h.cycle = r.cycle;
                    h.addr = addr;
                    h.pc = r.pc;
                    h.old_value = 0;
                    h.new_value = st.new_known ? st.new_value : 0;
                    h.flags = TRX_HIT_OLD_UNKNOWN | (st.new_known ? 0 : TRX_HIT_NEW_UNKNOWN);
                    h.index = lbase + k0 + (uint32_t)j;
                }
                remaining -= m;
            }
        }
        *exhausted = found < limit;
        unlock();
        return found;
    }
    for (int si = ns - 1; si >= 0 && found < limit; si--) {
        lbase -= sg[si].n;
        uint32_t remaining = sg[si].n;
        while (remaining && found < limit) {
            uint32_t m = remaining < TRACE_CHUNK ? remaining : TRACE_CHUNK;
            uint32_t k0 = remaining - m;
            trace_fetch(tr_add(sg[si].start, k0), m, tbuf);
            // the writes of the chunk's records: one range of stream 0 when they are close
            uint32_t ws0 = uint32_t(rec64(tbuf + 8)) & M28;
            uint32_t maxend = 0;
            bool one = true;
            for (uint32_t j = 0; j < m; j++) {
                uint64_t w1 = rec64(tbuf + 24 * j + 8);
                uint32_t d = (uint32_t(w1) - ws0) & M28;
                uint32_t e = d + uint16_t(w1 >> 48);
                if (d > 12000 || e > 12000) {
                    one = false;
                }
                if (e > maxend) {
                    maxend = e;
                }
            }
            const uint8_t *wp = NULL;
            if (one && maxend && s0_reach(ws0, cnt_now[0])) {
                wp = fetch(0, ws0, maxend);
            }
            for (int j = (int)m - 1; j >= 0 && found < limit; j--) {
                uint64_t w1 = rec64(tbuf + 24 * j + 8);
                uint32_t ws = uint32_t(w1) & M28;
                uint32_t wc = uint16_t(w1 >> 48);
                for (int w = (int)wc - 1; w >= 0 && found < limit; w--) {
                    uint32_t idx = (ws + w) & M28;
                    uint32_t v;
                    if (wp) {
                        v = rec32(wp + 4 * (((ws - ws0) & M28) + w));
                    } else {
                        if (!s0_reach(idx, cnt_now[0])) {
                            continue;
                        }
                        v = rec32(fetch(0, idx, 1));
                    }
                    if (uint16_t(v) != addr) {
                        continue;
                    }
                    TrxTraceRec r;
                    tr_decode(tbuf + 24 * j, ep[sg[si].e], &r);
                    TrxTraceHit &h = hits[found++];
                    h.cycle = r.cycle;
                    h.addr = addr;
                    h.pc = r.pc;
                    h.old_value = uint8_t(v >> 16);
                    h.new_value = uint8_t(v >> 24);
                    h.flags = 0;
                    h.index = lbase + k0 + (uint32_t)j;
                }
            }
            remaining -= m;
        }
    }
    *exhausted = found < limit;
    unlock();
    return found;
}

// ---- the poll (U18: the library's poll, LibRecord::poll -> ring.poll, ringops.cc) ------------------------------
// The firmware's task waited START_AFTER_MS, started the log and then read the markers every 100 ms. The base
// calls the poll every TRX_RING_POLL_MS (lib_ring.cc) and not while the machine is frozen; the start waits for its time here.
#define START_AFTER_MS 6000         // after the splash's READY watch (at most 4 s)

static uint32_t engine_t0;          // api->sys->ms() when the engine was started
static bool engine_started;         // the delayed start is past

bool ring_autostart = true;

bool trx_ring_engine_started(void)
{
    return engine_started;
}

void trx_ring_engine_poll(void)
{
    if (!present) {
        return;
    }
    if (!engine_started) {
        if (g_api->sys->ms() - engine_t0 < START_AFTER_MS) {
            return;
        }
        engine_started = true;
        if (ring_autostart) {
            if (reu_on()) {
                // A08: the REU is on at boot: no ring until it is off, then it starts
                reu_gate.blocked = true;
                reu_gate.resume = true;
                r_log("[RING] REU enabled: no ring until it is off (A08)\n");
            } else {
                trx_ring_start();
            }
        }
        r_log("[RING] log %s: streams at %07x/%07x/%07x%s\n", ring_autostart ? "on" : "off (setting)", (unsigned)area_base[0], (unsigned)area_base[1],
              (unsigned)area_base[2], f4_after_blocks ? ", older engine: F4s after block records skipped" : "");
    }
    trx_ring_poll();
}

const char *trx_ring_engine_init(void)
{
    static bool tried;                  // once: a second call (or a failed first one) does nothing
    if (tried) {
        return NULL;
    }
    tried = true;
    const SvcSync *sync = g_api->sync;
    if (!(trx_io_caps() & DRV_ident_caps_engine_MASK) || !g_api->c64 || !sync) {
        return NULL;
    }
    f4_after_blocks = !(HW_RD(ident_caps2) & DRV_ident_caps2_f4_after_f3_MASK);
    apply_hw = (HW_RD(ident_caps2) & DRV_ident_caps2_apply_MASK) != 0;
    bounce = (uint8_t *)alloc_aligned(BOUNCE);
    ram_val = (uint8_t *)r_malloc(65536);
    tbuf = (uint8_t *)r_malloc(TRACE_CHUNK * 24);
    ram_tch = (uint8_t *)alloc_aligned(65536);
    cart_val = (uint8_t *)r_malloc(65536);
    cart_tch = (uint8_t *)alloc_aligned(65536);
    tl = (TrxFrame *)r_malloc(sizeof(TrxFrame) * TL_CAP);
    mutex = sync->mutex_new();
    if (!bounce || !tl || !mutex || !ram_val || !ram_tch || !cart_val || !cart_tch) {
        r_log("[RING] out of memory, no frame ring\n");
        return "Out of memory: no frame ring";
    }
    engine_t0 = g_api->sys->ms();
    present = true;
    return NULL;
}
