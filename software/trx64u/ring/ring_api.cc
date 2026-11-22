// The ring table the library exports (LibRecord::exports, lib_ring.cc): the firmware's u2p_ring_api_t (software/ultimate-app/software/app_loader/
// app_api.h), filled as the firmware fills it (app_api_net.cc s_ring), and after it the dumps in steps (ring_steps.h). The monitor and trex_crt get it
// with api->lib->find("ring", 1, &size); agreed with MultiCore: the export "ring", major 1, the size first, appended to only.
// A file of its own: app_api.h and the SDK's svctab.h both define u2p_ring_status_t and the other record types, so the two never meet in one file.
#include "app_api.h"
#include "ring_steps.h"
#include "trx_ring.h"
#include "trx_c64re.h"
#include "trx_trxring.h"
#include <stddef.h>
#include <string.h>

static int api_ring_status(u2p_ring_status_t *st)
{
    TrxRingStatus rs;
    trx_ring_status(&rs);
    memset(st, 0, sizeof(*st));
    st->present = rs.present;
    st->on = rs.on;
    st->reu_off = rs.reu_off;
    st->frames = rs.frames;
    st->targets = rs.targets;
    st->head_seq = rs.head_seq;
    st->oldest_seq = rs.oldest_seq;
    st->landed_seq = trx_ring_landed();
    st->landed = st->landed_seq != 0;
    return rs.present ? U2P_OK : U2P_ERR_UNSUPPORTED;
}

static int api_ring_frame(int index, u2p_ring_frame_t *f)
{
    static TrxFrame tf;                 // 280 bytes: off the caller's stack
    if (!trx_ring_frame(index, &tf)) {
        return U2P_ERR_NOT_FOUND;
    }
    memset(f, 0, sizeof(*f));
    f->seq = tf.seq;
    f->c24 = tf.c24;
    f->cycles = tf.cycles;
    f->line = tf.line;
    f->x = tf.x;
    f->flags = tf.flags;
    f->target = trx_ring_reachable(index);
    return U2P_OK;
}

static int api_ring_land(int index) { return trx_ring_land(index, NULL); }

static_assert(sizeof(u2p_trace_rec_t) == sizeof(TrxTraceRec), "trace record layout");
static_assert(sizeof(u2p_trace_write_t) == sizeof(TrxWriteRec), "write record layout");
static_assert(sizeof(u2p_trace_hit_t) == sizeof(TrxTraceHit), "hit layout");

static int api_trace_status(u2p_trace_status_t *st)
{
    TrxTraceStatus ts;
    bool ok = trx_ring_trace_status(&ts);
    memset(st, 0, sizeof(*st));
    st->present = ts.present;
    st->on = ts.on;
    st->gap = ts.gap;
    st->records = ts.records;
    st->capacity = ts.capacity;
    st->lost = ts.lost;
    st->oldest_cycle = ts.oldest_cycle;
    st->newest_cycle = ts.newest_cycle;
    return ok ? U2P_OK : U2P_ERR_UNSUPPORTED;
}

static int api_trace_read(uint32_t first, uint32_t n, u2p_trace_rec_t *out)
{
    return trx_ring_trace_read(first, n, (TrxTraceRec *)out);
}

static int api_trace_writes(uint32_t start, uint32_t n, u2p_trace_write_t *out)
{
    return trx_ring_trace_writes(start, n, (TrxWriteRec *)out);
}

// *exhausted: bit 0 the scan reached the start of the history, bit 1 an I/O address scan passed writes through a pointer it cannot resolve
static int api_trace_whowrote(uint16_t addr, int limit, u2p_trace_hit_t *hits, int *exhausted)
{
    bool ex = true;
    uint32_t unres = 0;
    int n = trx_ring_trace_whowrote(addr, limit, (TrxTraceHit *)hits, &ex, &unres);
    *exhausted = (ex ? 1 : 0) | (unres ? 2 : 0);
    return n;
}

static int api_log_read(uint64_t *out, int max, uint32_t *lost) { return trx_ring_log_read(out, max, lost); }

// The ring as a file (.trxring); the size goes back through *bytes. No progress callback through this table.
static const char *api_ring_save_native(const char *path, uint32_t *bytes)
{
    uint32_t n = 0;
    const char *err = trx_trxring_save(path, &n, NULL, NULL);
    if (bytes) {
        *bytes = err ? 0 : n;
    }
    return err;
}

// The dumps in steps (ring_steps.h): trx_dump_* with the dump as an untyped pointer.
static_assert(int(RING_STEPS_FRAME) == int(TRX_DUMP_FRAME) && int(RING_STEPS_RING) == int(TRX_DUMP_RING) &&
                  int(RING_STEPS_MACHINE) == int(TRX_DUMP_MACHINE),
              "RingSteps kinds");

static void *steps_begin(int kind, const char *path, int arg, int stride, const char **err) { return trx_dump_begin(kind, path, arg, stride, err); }
static int steps_step(void *d, const char **err) { return trx_dump_step((TrxDump *)d, err); }
static int steps_pct(const void *d) { return trx_dump_pct((const TrxDump *)d); }
static uint32_t steps_bytes(const void *d) { return trx_dump_bytes((const TrxDump *)d); }
static int steps_entries(const void *d) { return trx_dump_entries((const TrxDump *)d); }
static void steps_drop(void *d) { trx_dump_drop((TrxDump *)d); }

// The export: the firmware's table, then the steps at the offset its `size` states (ring_steps.h).
struct RingExport {
    u2p_ring_api_t ring;
    RingSteps steps;
};
static_assert(offsetof(RingExport, steps) == sizeof(u2p_ring_api_t), "the steps start where the ring table's size says it ends");

// dump, save, load, load_entry and save_native as the firmware's table has them: each writes or reads the whole file in the one call (dump and save run
// every step of trx_dump_* in a loop) and returns NULL or what went wrong. They read and write registers, so the caller holds the core lock: an
// operation or a poll of its library. Each builds the machine its file describes (System Mode, cartridge, drive A, their media) and a load's hooks
// itself (ringmachine.cc), as the menu's operations do: the same file through either way in. The steps write the same bytes as dump and save, one
// piece per call.
static const RingExport s_export = {
    {
        sizeof(u2p_ring_api_t),
        api_ring_status,
        api_ring_frame,
        trx_ring_find,
        api_ring_land,
        trx_ring_commit,
        trx_ring_error,
        trx_c64re_dump,
        trx_c64rering_dump,
        trx_c64re_load,
        trx_ring_trace_ctl,
        api_trace_status,
        api_trace_read,
        api_trace_writes,
        api_trace_whowrote,
        api_log_read,
        trx_c64re_load_entry,
        api_ring_save_native,
    },
    { sizeof(RingSteps), steps_begin, steps_step, steps_pct, steps_bytes, steps_entries, steps_drop },
};

const void *ring_exports(uint32_t *size)
{
    *size = sizeof(s_export);
    return &s_export;
}
