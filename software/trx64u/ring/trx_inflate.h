// PROVENANCE: the firmware fork TREX/1541ultimate:software/io/c64/trx_inflate.h, copied for the ring library (U18 D6: this copy is the master).
// DEFLATE (RFC 1951) and gzip (RFC 1952) for the RISC-V: the board writes
// .c64re files with stored blocks only (no compressor), but TRX64 writes them
// with flate2, so loading a file from the emulator needs a real inflate
// (TRX64-Ultimate docs/specs/T20-delta-ring.md §9.4, T66 part 4).
//
// The caller owns the buffers; nothing here allocates. The Huffman tables are
// one static set: one inflate at a time (the loader runs in the menu task or
// the REST task, never both - the same rule the writer follows).
#ifndef TRX_INFLATE_H
#define TRX_INFLATE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TRX_INF_OK          0
#define TRX_INF_STOPPED     1   // the sink asked to stop: the stream was not read to its end
#define TRX_INF_TRUNCATED  -1   // the input ended inside a block
#define TRX_INF_FULL       -2   // the stream is longer than the file says (the gzip trailer's ISIZE)
#define TRX_INF_BADBLOCK   -3   // a block type of 3, or a stored block's ~len is wrong
#define TRX_INF_BADCODE    -4   // a code the tree does not have, or an unused length/distance code
#define TRX_INF_BADDIST    -5   // a match reaches before the start of the output
#define TRX_INF_BADTREE    -6   // the dynamic block's code lengths do not make a tree
#define TRX_INF_BADGZIP    -7   // not a gzip member, or an unsupported one
#define TRX_INF_BADCRC     -8   // the member's CRC-32 or ISIZE does not match
#define TRX_INF_READ       -9   // the source answered a read error
#define TRX_INF_NEEDWIN   -10   // a Huffman block in a stream run without a window: run it again with one
#define TRX_INF_SINK      -11   // the sink failed (its own reason is the caller's)

#define TRX_INF_WSIZE   32768u  // the deflate window
#define TRX_INF_WMASK   (TRX_INF_WSIZE - 1u)

// One raw DEFLATE stream, streamed: the input comes from src a buffer at a time, every output byte goes to sink in order. Only the window (32 KB,
// the caller's) is kept of the output, and only when the stream has Huffman blocks: a stream of stored blocks (what the board writes) runs with
// win NULL and its bytes go from the input buffer to the sink. The caller sets the fields before the state, then trx_inf_init().
typedef struct {
    int (*src)(void *ctx, uint8_t *buf, uint32_t cap);      // up to cap bytes into buf: how many, 0 at the end of the input, < 0 a read error
    void *src_ctx;
    uint8_t *in;                                            // the input buffer, in_cap bytes
    uint32_t in_cap;
    int (*sink)(void *ctx, const uint8_t *p, uint32_t n);   // 0 go on, 1 stop the stream here (TRX_INF_STOPPED), < 0 fail it (TRX_INF_SINK)
    void *sink_ctx;
    uint8_t *win;                                           // TRX_INF_WSIZE bytes, or NULL (a Huffman block answers TRX_INF_NEEDWIN)
    uint32_t out;                                           // the stream offset of the next byte: 0, or where a restart starts
    // the state
    uint32_t in_len, in_pos;
    uint64_t in_at;                                         // the input offset of in[0], counted from the first byte src gave
    uint32_t acc;
    int nbits, over, rd_err, last;
    uint32_t wpos, wsunk;                                   // bytes into the window, of them given to the sink
    uint32_t crc;                                           // the CRC-32 of the bytes given to the sink (trx_crc32_update's form)
    uint64_t blk_in;                                        // the input offset of the current block's header
    uint32_t blk_out;                                       // the stream offset of its first byte
    uint32_t first_huff;                                    // the stream offset of the first Huffman block, 0xFFFFFFFF while none came
} TrxInflate;

// The state to the start of a stream; also to the start of a stored block for a restart there (out set to its stream offset, the source at its
// header, which is on a byte boundary in a stream of stored blocks).
void trx_inf_init(TrxInflate *z);
// A gzip member's header (RFC 1952: FEXTRA, FNAME, FCOMMENT, FHCRC skipped): TRX_INF_OK or TRX_INF_BADGZIP.
int trx_inf_gzip_header(TrxInflate *z);
// Blocks until the final one: TRX_INF_OK, TRX_INF_STOPPED, or an error. The gzip trailer is the caller's.
int trx_inf_run(TrxInflate *z);

// What went wrong, for a message in front of the user.
const char *trx_inflate_error(int code);

// CRC-32 (gzip's: reflected, polynomial 0xEDB88320) a byte a step from a 256-entry table, built at the first call (1 KB of bss).
// trx_crc32_update(0, p, n) is the CRC of p[0..n); a running CRC goes back in as crc.
const uint32_t *trx_crc32_table(void);
uint32_t trx_crc32_update(uint32_t crc, const uint8_t *p, uint32_t n);

#ifdef __cplusplus
}
#endif

#endif
