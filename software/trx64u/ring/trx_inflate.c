// PROVENANCE: TRX64-Ultimate software/ultimate-app/software/io/c64/trx_inflate.c, copied for the ring library (U18 D6: this copy is the master).
// See trx_inflate.h. A canonical-Huffman inflate: counts per code length and
// the symbols sorted by code, decoded by walking the lengths (RFC 1951 §3.2.2,
// the shape of zlib's own puff.c). No allocation, no window copy: the output
// buffer is the window, so a match reads what was already written there.
#include "trx_inflate.h"

#include <string.h>

typedef struct {
    const uint8_t *src;
    uint32_t slen, spos;
    uint32_t acc;                   // bits not used yet, LSB first
    int nbits;
    int over;                       // the input ran out
} Bitin;

typedef struct {
    uint16_t counts[16];            // codes of each length (index 0 unused)
    uint16_t symbols[288];          // the symbols, sorted by their code
} Tree;

static uint32_t get_bits(Bitin *b, int n)
{
    while (b->nbits < n) {
        if (b->spos >= b->slen) {
            b->over = 1;
            b->acc |= 0u;           // zeros past the end; the caller sees over
            b->nbits += 8;
            continue;
        }
        b->acc |= (uint32_t)b->src[b->spos++] << b->nbits;
        b->nbits += 8;
    }
    uint32_t v = b->acc & ((n == 32) ? 0xFFFFFFFFu : ((1u << n) - 1u));
    b->acc >>= n;
    b->nbits -= n;
    return v;
}

// A symbol of tree t, or a negative error.
static int decode(Bitin *b, const Tree *t)
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

static int dynamic_trees(Bitin *b)
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

static int block_body(Bitin *b, uint8_t *dst, uint32_t dlen, uint32_t *o)
{
    for (;;) {
        int sym = decode(b, &s_lit);
        if (sym < 0) {
            return sym;
        }
        if (b->over) {
            return TRX_INF_TRUNCATED;
        }
        if (sym < 256) {
            if (*o >= dlen) {
                return TRX_INF_FULL;
            }
            dst[(*o)++] = (uint8_t)sym;
            continue;
        }
        if (sym == 256) {
            return TRX_INF_OK;
        }
        sym -= 257;
        if (sym >= 29) {
            return TRX_INF_BADCODE;
        }
        uint32_t length = len_base[sym] + get_bits(b, len_extra[sym]);
        int ds = decode(b, &s_dist);
        if (ds < 0) {
            return ds;
        }
        if (ds >= 30) {
            return TRX_INF_BADCODE;
        }
        uint32_t dist = dist_base[ds] + get_bits(b, dist_extra[ds]);
        if (b->over) {
            return TRX_INF_TRUNCATED;
        }
        if (dist > *o) {
            return TRX_INF_BADDIST;
        }
        if (*o + length > dlen) {
            return TRX_INF_FULL;
        }
        uint8_t *p = dst + *o - dist;
        uint8_t *q = dst + *o;
        for (uint32_t k = 0; k < length; k++) {
            q[k] = p[k];            // byte by byte: the ranges may overlap
        }
        *o += length;
    }
}

int trx_inflate(const uint8_t *src, uint32_t slen, uint8_t *dst, uint32_t dlen, uint32_t *out_len)
{
    Bitin b;
    memset(&b, 0, sizeof(b));
    b.src = src;
    b.slen = slen;
    uint32_t o = 0;
    for (;;) {
        int last = (int)get_bits(&b, 1);
        int type = (int)get_bits(&b, 2);
        if (b.over) {
            *out_len = o;
            return TRX_INF_TRUNCATED;
        }
        int err = TRX_INF_OK;
        if (type == 0) {
            b.acc = 0;              // a stored block starts on a byte boundary
            b.nbits = 0;
            if (b.spos + 4 > b.slen) {
                *out_len = o;
                return TRX_INF_TRUNCATED;
            }
            uint32_t n = (uint32_t)b.src[b.spos] | ((uint32_t)b.src[b.spos + 1] << 8);
            uint32_t m = (uint32_t)b.src[b.spos + 2] | ((uint32_t)b.src[b.spos + 3] << 8);
            b.spos += 4;
            if ((n ^ 0xFFFFu) != m) {
                *out_len = o;
                return TRX_INF_BADBLOCK;
            }
            if (b.spos + n > b.slen) {
                *out_len = o;
                return TRX_INF_TRUNCATED;
            }
            if (o + n > dlen) {
                *out_len = o;
                return TRX_INF_FULL;
            }
            memcpy(dst + o, b.src + b.spos, n);
            b.spos += n;
            o += n;
        } else if (type == 1) {
            fixed_trees();
            err = block_body(&b, dst, dlen, &o);
        } else if (type == 2) {
            err = dynamic_trees(&b);
            if (!err) {
                err = block_body(&b, dst, dlen, &o);
            }
        } else {
            err = TRX_INF_BADBLOCK;
        }
        if (err) {
            *out_len = o;
            return err;
        }
        if (last) {
            *out_len = o;
            return TRX_INF_OK;
        }
    }
}

// ---- gzip ------------------------------------------------------------------------
static uint32_t crc32_of(const uint8_t *p, uint32_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) {
            c = (c >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(c & 1)));
        }
    }
    return ~c;
}

uint32_t trx_gzip_isize(const uint8_t *src, uint32_t slen)
{
    if (slen < 18) {
        return 0;
    }
    return (uint32_t)src[slen - 4] | ((uint32_t)src[slen - 3] << 8) | ((uint32_t)src[slen - 2] << 16) |
           ((uint32_t)src[slen - 1] << 24);
}

int trx_gunzip(const uint8_t *src, uint32_t slen, uint8_t *dst, uint32_t dlen, uint32_t *out_len)
{
    *out_len = 0;
    if (slen < 18 || src[0] != 0x1F || src[1] != 0x8B || src[2] != 8) {
        return TRX_INF_BADGZIP;
    }
    uint8_t flg = src[3];
    if (flg & 0xE0) {
        return TRX_INF_BADGZIP;     // reserved bits set
    }
    uint32_t i = 10;
    if (flg & 4) {                  // FEXTRA
        if (i + 2 > slen) {
            return TRX_INF_BADGZIP;
        }
        i += 2u + ((uint32_t)src[i] | ((uint32_t)src[i + 1] << 8));
    }
    if (flg & 8) {                  // FNAME
        while (i < slen && src[i]) i++;
        i++;
    }
    if (flg & 16) {                 // FCOMMENT
        while (i < slen && src[i]) i++;
        i++;
    }
    if (flg & 2) {                  // FHCRC
        i += 2;
    }
    if (i + 8 > slen) {
        return TRX_INF_BADGZIP;
    }
    int err = trx_inflate(src + i, slen - 8 - i, dst, dlen, out_len);
    if (err) {
        return err;
    }
    uint32_t crc = (uint32_t)src[slen - 8] | ((uint32_t)src[slen - 7] << 8) | ((uint32_t)src[slen - 6] << 16) |
                   ((uint32_t)src[slen - 5] << 24);
    uint32_t isize = trx_gzip_isize(src, slen);
    if (isize != *out_len || crc != crc32_of(dst, *out_len)) {
        return TRX_INF_BADCRC;
    }
    return TRX_INF_OK;
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
    default:                return "inflate failed";
    }
}
