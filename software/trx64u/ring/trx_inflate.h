// PROVENANCE: TRX64-Ultimate software/ultimate-app/software/io/c64/trx_inflate.h, copied for the ring library (U18 D6: this copy is the master).
// DEFLATE (RFC 1951) and gzip (RFC 1952) for the RISC-V: the board writes
// .c64re files with stored blocks only (no compressor), but TRX64 writes them
// with flate2, so loading a file from the emulator needs a real inflate
// (TRX64-Ultimate docs/specs/T20-delta-ring.md §9.4, T66 part 4).
//
// The caller owns both buffers; nothing here allocates. The Huffman tables are
// one static set: one inflate at a time (the loader runs in the menu task or
// the REST task, never both - the same rule trx_c64re.cc's writer follows).
#ifndef TRX_INFLATE_H
#define TRX_INFLATE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TRX_INF_OK          0
#define TRX_INF_TRUNCATED  -1   // the input ended inside a block
#define TRX_INF_FULL       -2   // the output buffer is too small
#define TRX_INF_BADBLOCK   -3   // a block type of 3, or a stored block's ~len is wrong
#define TRX_INF_BADCODE    -4   // a code the tree does not have, or an unused length/distance code
#define TRX_INF_BADDIST    -5   // a match reaches before the start of the output
#define TRX_INF_BADTREE    -6   // the dynamic block's code lengths do not make a tree
#define TRX_INF_BADGZIP    -7   // not a gzip member, or an unsupported one
#define TRX_INF_BADCRC     -8   // the member's CRC-32 or ISIZE does not match

// One raw DEFLATE stream from src into dst. *out_len gets the bytes written.
int trx_inflate(const uint8_t *src, uint32_t slen, uint8_t *dst, uint32_t dlen, uint32_t *out_len);

// One gzip member: header skipped, CRC-32 and ISIZE checked.
int trx_gunzip(const uint8_t *src, uint32_t slen, uint8_t *dst, uint32_t dlen, uint32_t *out_len);

// The ISIZE field of a gzip member (its last four bytes): what the inflated
// body is said to be, so a caller can size its buffer. 0 when slen < 18.
uint32_t trx_gzip_isize(const uint8_t *src, uint32_t slen);

// What went wrong, for a message in front of the user.
const char *trx_inflate_error(int code);

#ifdef __cplusplus
}
#endif

#endif
