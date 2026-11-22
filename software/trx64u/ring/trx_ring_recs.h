// PROVENANCE: TRX64-Ultimate software/ultimate-app/software/io/c64/trx_ring_recs.h, copied for the ring library (U18 D6: this copy is the master).
// Moving a run of log records between a file and one of the engine's DDR2
// areas (T20 §10.2, T66 part 6). Two things make this more than a memcpy:
//
//  - an area is a ring. A run of records starts at (count * record size) &
//    (size - 1) and wraps at the end of the area.
//  - the mover takes 16-byte aligned addresses and whole 16-byte groups
//    (trx_engine.v), while a run starts and ends wherever its records are. On
//    the way out the group at each end is read first and written back with the
//    new bytes in it, so the records on either side - older than the oldest
//    restored one, or the place the engine writes next - keep what they have.
//
// The area's reads and writes are callbacks, so the host test bank can run
// this against a byte array and check both the offsets and the alignment.
#ifndef TRX_RING_RECS_H
#define TRX_RING_RECS_H

#include <stdint.h>
#include <string.h>

struct TrxRecIo {
    uint32_t size;      // bytes in the area (a power of two)
    uint32_t rec;       // bytes per record
    uint32_t chunk;     // the biggest transfer, a multiple of 16
    void    *ctx;
    void (*rd)(void *ctx, uint32_t off, uint8_t *p, uint32_t len);
    void (*wr)(void *ctx, uint32_t off, const uint8_t *p, uint32_t len);
};

// Records from count "from" on (at most "left" of them) into buf, which holds
// io->chunk + 16 bytes: *at says where the first one starts in buf, the return
// value is how many BYTES came - whole records, and never past the area's end.
static inline uint32_t trx_recs_in(const TrxRecIo *io, uint32_t from, uint32_t left, uint8_t *buf,
                                   uint32_t *at)
{
    uint32_t off = (from * io->rec) & (io->size - 1);
    uint32_t a0 = off & ~15u;
    uint32_t pre = off - a0;
    uint32_t want = left * io->rec;
    if (want > io->chunk) {
        want = io->chunk;
    }
    if (off + want > io->size) {
        want = io->size - off;
    }
    uint32_t rl = (pre + want + 15u) & ~15u;
    if (a0 + rl > io->size) {
        rl = io->size - a0;
    }
    io->rd(io->ctx, a0, buf, rl);
    uint32_t have = rl - pre;
    if (have > want) {
        have = want;
    }
    *at = pre;
    return (have / io->rec) * io->rec;
}

// n bytes of records, starting at count "from", back into the area. stage is a
// 16-byte aligned buffer of at least io->chunk bytes.
static inline void trx_recs_out(const TrxRecIo *io, uint32_t from, const uint8_t *src, uint32_t n,
                                uint8_t *stage)
{
    uint32_t off = (from * io->rec) & (io->size - 1);
    uint32_t done = 0;
    uint8_t edge[16];
    while (done < n) {
        uint32_t want = n - done;
        if (off + want > io->size) {
            want = io->size - off;          // the rest goes on at the area's start
        }
        uint32_t pre = off & 15u;
        if (pre) {
            uint32_t take = 16 - pre;
            if (take > want) {
                take = want;
            }
            io->rd(io->ctx, off - pre, edge, 16);
            memcpy(edge + pre, src + done, take);
            io->wr(io->ctx, off - pre, edge, 16);
            done += take;
            off += take;
        } else if (want >= 16) {
            uint32_t whole = want & ~15u;
            if (whole > io->chunk) {
                whole = io->chunk;
            }
            memcpy(stage, src + done, whole);
            io->wr(io->ctx, off, stage, whole);
            done += whole;
            off += whole;
        } else {
            io->rd(io->ctx, off, edge, 16);
            memcpy(edge, src + done, want);
            io->wr(io->ctx, off, edge, 16);
            done += want;
            off += want;
        }
        if (off >= io->size) {
            off = 0;
        }
    }
}

#endif
