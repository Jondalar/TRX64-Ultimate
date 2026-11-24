// PROVENANCE: the firmware fork TREX/1541ultimate:software/io/c64/trx_inflate.c, copied for the ring library (U18 D6: this copy is the master).
// See trx_inflate.h. A canonical-Huffman inflate: counts per code length and
// the symbols sorted by code, decoded by walking the lengths (RFC 1951 §3.2.2,
// the shape of zlib's own puff.c), streamed: a match reads the caller's 32 KB
// window, which holds the newest output; nothing here allocates.
#include "trx_inflate.h"

#include <string.h>

typedef struct {
    uint16_t counts[16];            // codes of each length (index 0 unused)
    uint16_t symbols[288];          // the symbols, sorted by their code
} Tree;

// The next input byte, the buffer refilled from the source when it is used up; past the end (or after a read error) zeros, and over is set.
static int next_byte(TrxInflate *z)
{
    if (z->in_pos >= z->in_len) {
        z->in_at += z->in_len;
        z->in_len = z->in_pos = 0;
        int got = z->over ? 0 : z->src(z->src_ctx, z->in, z->in_cap);
        if (got <= 0) {
            if (got < 0) {
                z->rd_err = 1;
            }
            z->over = 1;
            return 0;
        }
        z->in_len = (uint32_t)got;
    }
    return z->in[z->in_pos++];
}

static uint32_t get_bits(TrxInflate *z, int n)
{
    while (z->nbits < n) {
        z->acc |= (uint32_t)next_byte(z) << z->nbits;
        z->nbits += 8;
    }
    uint32_t v = z->acc & ((n == 32) ? 0xFFFFFFFFu : ((1u << n) - 1u));
    z->acc >>= n;
    z->nbits -= n;
    return v;
}

// A symbol of tree t, or a negative error.
static int decode(TrxInflate *b, const Tree *t)
{
    int len = 0, code = 0, first = 0, index = 0;
    for (len = 1; len <= 15; len++) {
        code |= (int)get_bits(b, 1);
        int count = t->counts[len];
        if (code - first < count) {
            return t->symbols[index + (code - first)];
        }
        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }
    return TRX_INF_BADCODE;
}

// The tree of n code lengths (0 = the symbol is not in the tree).
static int build(Tree *t, const uint8_t *lengths, int n)
{
    memset(t->counts, 0, sizeof(t->counts));
    for (int i = 0; i < n; i++) {
        t->counts[lengths[i]]++;
    }
    t->counts[0] = 0;
    int left = 1;                   // over-subscribed or incomplete?
    for (int len = 1; len <= 15; len++) {
        left <<= 1;
        left -= t->counts[len];
        if (left < 0) {
            return TRX_INF_BADTREE;
        }
    }
    uint16_t offs[16];
    offs[1] = 0;
    for (int len = 1; len < 15; len++) {
        offs[len + 1] = (uint16_t)(offs[len] + t->counts[len]);
    }
    for (int i = 0; i < n; i++) {
        if (lengths[i]) {
            t->symbols[offs[lengths[i]]++] = (uint16_t)i;
        }
    }
    // left > 0 is an incomplete tree; legal only for a single distance code
    // (RFC 1951 allows it), and decode() answers BADCODE for the gap anyway.
    return TRX_INF_OK;
}

static const uint16_t len_base[29] = { 3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                       31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const uint8_t  len_extra[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                        2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const uint16_t dist_base[30] = { 1,    2,    3,    4,    5,    7,     9,     13,    17,  25,
                                        33,   49,   65,   97,   129,  193,   257,   385,   513, 769,
                                        1025, 1537, 2049, 3073, 4097, 6145,  8193,  12289, 16385, 24577 };
static const uint8_t  dist_extra[30] = { 0, 0, 0,  0,  1,  1,  2,  2,  3,  3,  4,  4,  5,  5,  6,
                                         6, 7, 7,  8,  8,  9,  9,  10, 10, 11, 11, 12, 12, 13, 13 };

static Tree s_lit, s_dist;          // one inflate at a time (trx_inflate.h)

static void fixed_trees(void)
{
    uint8_t l[288];
    int i = 0;
    for (; i < 144; i++) l[i] = 8;
    for (; i < 256; i++) l[i] = 9;
    for (; i < 280; i++) l[i] = 7;
    for (; i < 288; i++) l[i] = 8;
    build(&s_lit, l, 288);
    for (i = 0; i < 30; i++) l[i] = 5;
    build(&s_dist, l, 30);
}

static int dynamic_trees(TrxInflate *b)
{
    static const uint8_t ord[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
    int hlit = (int)get_bits(b, 5) + 257;
    int hdist = (int)get_bits(b, 5) + 1;
    int hclen = (int)get_bits(b, 4) + 4;
    if (hlit > 286 || hdist > 30) {
        return TRX_INF_BADTREE;
    }
    uint8_t cl[19];
    memset(cl, 0, sizeof(cl));
    for (int i = 0; i < hclen; i++) {
        cl[ord[i]] = (uint8_t)get_bits(b, 3);
    }
    Tree clt;
    int err = build(&clt, cl, 19);
    if (err) {
        return err;
    }
    uint8_t lengths[288 + 30];
    memset(lengths, 0, sizeof(lengths));
    int n = 0;
    while (n < hlit + hdist) {
        int sym = decode(b, &clt);
        if (sym < 0) {
            return sym;
        }
        if (b->over) {
            return TRX_INF_TRUNCATED;
        }
        if (sym < 16) {
            lengths[n++] = (uint8_t)sym;
        } else if (sym == 16) {
            if (!n) {
                return TRX_INF_BADTREE;
            }
            uint8_t prev = lengths[n - 1];
            int rep = 3 + (int)get_bits(b, 2);
            while (rep-- && n < hlit + hdist) {
                lengths[n++] = prev;
            }
        } else if (sym == 17) {
            int rep = 3 + (int)get_bits(b, 3);
            while (rep-- && n < hlit + hdist) {
                lengths[n++] = 0;
            }
        } else {
            int rep = 11 + (int)get_bits(b, 7);
            while (rep-- && n < hlit + hdist) {
                lengths[n++] = 0;
            }
        }
    }
    if (lengths[256] == 0) {
        return TRX_INF_BADTREE;     // no end-of-block code
    }
    err = build(&s_lit, lengths, hlit);
    if (err) {
        return err;
    }
    return build(&s_dist, lengths + hlit, hdist);
}

// The window's bytes not given to the sink yet, given now (in two pieces where they wrap).
static int flush_win(TrxInflate *z)
{
    while (z->wsunk != z->wpos) {
        uint32_t at = z->wsunk & TRX_INF_WMASK;
        uint32_t n = z->wpos - z->wsunk;
        if (n > TRX_INF_WSIZE - at) {
            n = TRX_INF_WSIZE - at;
        }
        z->crc = trx_crc32_update(z->crc, z->win + at, n);
        z->wsunk += n;
        int r = z->sink(z->sink_ctx, z->win + at, n);
        if (r) {
            return r > 0 ? TRX_INF_STOPPED : TRX_INF_SINK;
        }
    }
    return TRX_INF_OK;
}

static int block_body(TrxInflate *z)
{
    uint8_t *const w = z->win;
    for (;;) {
        int sym = decode(z, &s_lit);
        if (sym < 0) {
            return sym;
        }
        if (z->over) {
            return TRX_INF_TRUNCATED;
        }
        if (sym < 256) {
            w[z->wpos++ & TRX_INF_WMASK] = (uint8_t)sym;
            z->out++;
            if (z->wpos - z->wsunk == TRX_INF_WSIZE) {
                int r = flush_win(z);
                if (r) {
                    return r;
                }
            }
            continue;
        }
        if (sym == 256) {
            return TRX_INF_OK;
        }
        sym -= 257;
        if (sym >= 29) {
            return TRX_INF_BADCODE;
        }
        uint32_t length = len_base[sym] + get_bits(z, len_extra[sym]);
        int ds = decode(z, &s_dist);
        if (ds < 0) {
            return ds;
        }
        if (ds >= 30) {
            return TRX_INF_BADCODE;
        }
        uint32_t dist = dist_base[ds] + get_bits(z, dist_extra[ds]);
        if (z->over) {
            return TRX_INF_TRUNCATED;
        }
        if (dist > z->wpos) {
            return TRX_INF_BADDIST;
        }
        for (uint32_t k = 0; k < length; k++) {
            w[z->wpos & TRX_INF_WMASK] = w[(z->wpos - dist) & TRX_INF_WMASK];    // byte by byte: the ranges may overlap
            z->wpos++;
            if (z->wpos - z->wsunk == TRX_INF_WSIZE) {
                int r = flush_win(z);
                if (r) {
                    return r;
                }
            }
        }
        z->out += length;
    }
}

// A stored block's bytes, straight from the input buffer to the sink (and into the window when there is one: a later Huffman block may match
// them).
static int stored_body(TrxInflate *z, uint32_t n)
{
    int r = z->win ? flush_win(z) : TRX_INF_OK;
    while (!r && n) {
        if (z->in_pos >= z->in_len) {
            (void)next_byte(z);
            if (z->over) {
                return TRX_INF_TRUNCATED;
            }
            z->in_pos--;            // the byte goes out with the piece below
        }
        uint32_t piece = z->in_len - z->in_pos;
        if (piece > n) {
            piece = n;
        }
        const uint8_t *p = z->in + z->in_pos;
        z->in_pos += piece;
        n -= piece;
        z->out += piece;
        if (z->win) {
            for (uint32_t k = 0; k < piece; k++) {
                z->win[(z->wpos + k) & TRX_INF_WMASK] = p[k];
            }
            z->wpos += piece;
            z->wsunk = z->wpos;
        }
        z->crc = trx_crc32_update(z->crc, p, piece);
        int s = z->sink(z->sink_ctx, p, piece);
        if (s) {
            r = s > 0 ? TRX_INF_STOPPED : TRX_INF_SINK;
        }
    }
    return r;
}

void trx_inf_init(TrxInflate *z)
{
    z->in_len = z->in_pos = 0;
    z->in_at = 0;
    z->acc = 0;
    z->nbits = 0;
    z->over = 0;
    z->rd_err = 0;
    z->wpos = z->wsunk = 0;
    z->crc = 0;
    z->last = 0;
    z->blk_in = 0;
    z->blk_out = z->out;
    z->first_huff = 0xFFFFFFFFu;
}

int trx_inf_run(TrxInflate *z)
{
    while (!z->last) {
        z->blk_in = z->in_at + z->in_pos;
        z->blk_out = z->out;
        z->last = (int)get_bits(z, 1);
        int type = (int)get_bits(z, 2);
        if (z->over) {
            return z->rd_err ? TRX_INF_READ : TRX_INF_TRUNCATED;
        }
        int err = TRX_INF_OK;
        if (type == 0) {
            z->acc = 0;             // a stored block starts on a byte boundary
            z->nbits = 0;
            uint32_t n = (uint32_t)next_byte(z);
            n |= (uint32_t)next_byte(z) << 8;
            uint32_t m = (uint32_t)next_byte(z);
            m |= (uint32_t)next_byte(z) << 8;
            if (z->over) {
                return z->rd_err ? TRX_INF_READ : TRX_INF_TRUNCATED;
            }
            if ((n ^ 0xFFFFu) != m) {
                return TRX_INF_BADBLOCK;
            }
            err = stored_body(z, n);
        } else if (type == 1 || type == 2) {
            if (!z->win) {
                return TRX_INF_NEEDWIN;
            }
            if (z->first_huff == 0xFFFFFFFFu) {
                z->first_huff = z->out;
            }
            if (type == 1) {
                fixed_trees();
            } else {
                err = dynamic_trees(z);
            }
            if (!err) {
                err = block_body(z);
            }
        } else {
            err = TRX_INF_BADBLOCK;
        }
        if (err) {
            return (err == TRX_INF_TRUNCATED && z->rd_err) ? TRX_INF_READ : err;
        }
    }
    int r = z->win ? flush_win(z) : TRX_INF_OK;
    return r;
}

int trx_inf_gzip_header(TrxInflate *z)
{
    uint8_t h[10];
    for (int i = 0; i < 10; i++) {
        h[i] = (uint8_t)next_byte(z);
    }
    if (z->over || h[0] != 0x1F || h[1] != 0x8B || h[2] != 8 || (h[3] & 0xE0)) {
        return TRX_INF_BADGZIP;     // not a gzip member, or reserved bits set
    }
    uint8_t flg = h[3];
    if (flg & 4) {                  // FEXTRA
        uint32_t xlen = (uint32_t)next_byte(z);
        xlen |= (uint32_t)next_byte(z) << 8;
        while (xlen-- && !z->over) {
            (void)next_byte(z);
        }
    }
    if (flg & 8) {                  // FNAME
        while (next_byte(z) && !z->over) {
        }
    }
    if (flg & 16) {                 // FCOMMENT
        while (next_byte(z) && !z->over) {
        }
    }
    if (flg & 2) {                  // FHCRC
        (void)next_byte(z);
        (void)next_byte(z);
    }
    return z->over ? TRX_INF_BADGZIP : TRX_INF_OK;
}

// ---- gzip ------------------------------------------------------------------------
static uint32_t crc_tab[256];

const uint32_t *trx_crc32_table(void)
{
    if (!crc_tab[128]) {            // entry 128 is the polynomial itself: never 0 once built
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) {
                c = (c >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(c & 1)));
            }
            crc_tab[i] = c;
        }
    }
    return crc_tab;
}

uint32_t trx_crc32_update(uint32_t crc, const uint8_t *p, uint32_t n)
{
    const uint32_t *const t = trx_crc32_table();
    uint32_t c = crc ^ 0xFFFFFFFFu;
    for (uint32_t i = 0; i < n; i++) {
        c = (c >> 8) ^ t[(c ^ p[i]) & 0xFF];
    }
    return c ^ 0xFFFFFFFFu;
}

const char *trx_inflate_error(int code)
{
    switch (code) {
    case TRX_INF_OK:        return "ok";
    case TRX_INF_TRUNCATED: return "the compressed data ends early";
    case TRX_INF_FULL:      return "the file says it unpacks to less than it does";
    case TRX_INF_BADBLOCK:  return "a bad deflate block";
    case TRX_INF_BADCODE:   return "a bad deflate code";
    case TRX_INF_BADDIST:   return "a deflate match points before the start";
    case TRX_INF_BADTREE:   return "a bad deflate code table";
    case TRX_INF_BADGZIP:   return "not a gzip body";
    case TRX_INF_BADCRC:    return "the gzip body's checksum does not match";
    case TRX_INF_READ:      return "the file could not be read";
    case TRX_INF_NEEDWIN:   return "out of memory for the deflate window";
    default:                return "inflate failed";
    }
}
