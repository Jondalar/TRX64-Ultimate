// Host tests: the inflate and the node-built chip block (T66 parts 4 and 9).
// See the Makefile. Every check prints its own line on failure; the exit code
// is the number of failures.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "trx_inflate.h"
#include "trx_c64re_nodes.h"
#include "trx_ring_recs.h"
#include "trx_sha256.h"

static int checks, failed;

#define CHECK(cond, ...)                                                                                   \
    do {                                                                                                   \
        checks++;                                                                                          \
        if (!(cond)) {                                                                                     \
            failed++;                                                                                      \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                    \
            printf(__VA_ARGS__);                                                                           \
            printf("\n");                                                                                  \
        }                                                                                                  \
    } while (0)

#define W_CPU          1
#define W_VIC          4
#define W_CIA1        16
#define W_CIA2        21
#define W_CPUH        27
#define W_CIA1H       34
#define W_CIA2H       46

static uint32_t bits_of(const uint32_t *w, int lo, int n)
{
    uint32_t v = 0;
    for (int b = 0; b < n; b++) {
        v |= ((w[(lo + b) / 32] >> ((lo + b) % 32)) & 1u) << b;
    }
    return v;
}

static uint8_t byte_of(const uint32_t *w, int word0, int reg)
{
    return uint8_t(w[word0 + reg / 4] >> (8 * (reg & 3)));
}

// ---- inflate ---------------------------------------------------------------------
// One gzip member of src, written by zlib at the given level and strategy.
static uint8_t *gzip_it(const uint8_t *src, uint32_t len, int level, int strategy, uint32_t *out_len)
{
    z_stream z;
    memset(&z, 0, sizeof(z));
    if (deflateInit2(&z, level, Z_DEFLATED, 15 + 16, 8, strategy) != Z_OK) {
        return NULL;
    }
    uint32_t cap = uint32_t(deflateBound(&z, len)) + 64;
    uint8_t *out = (uint8_t *)malloc(cap);
    z.next_in = (Bytef *)src;
    z.avail_in = len;
    z.next_out = out;
    z.avail_out = cap;
    int e = deflate(&z, Z_FINISH);
    *out_len = cap - z.avail_out;
    deflateEnd(&z);
    if (e != Z_STREAM_END) {
        free(out);
        return NULL;
    }
    return out;
}

static void one_roundtrip(const char *what, const uint8_t *src, uint32_t len, int level, int strategy)
{
    uint32_t clen = 0;
    uint8_t *cz = gzip_it(src, len, level, strategy, &clen);
    if (!cz) {
        CHECK(false, "%s: zlib would not compress (level %d strategy %d)", what, level, strategy);
        return;
    }
    CHECK(trx_gzip_isize(cz, clen) == len, "%s: ISIZE %lu, not %lu (level %d)", what,
          (unsigned long)trx_gzip_isize(cz, clen), (unsigned long)len, level);
    uint8_t *back = (uint8_t *)malloc(len ? len : 1);
    uint32_t got = 0;
    int err = trx_gunzip(cz, clen, back, len, &got);
    CHECK(err == TRX_INF_OK, "%s: %s (level %d strategy %d)", what, trx_inflate_error(err), level, strategy);
    CHECK(got == len, "%s: %lu bytes back, not %lu (level %d)", what, (unsigned long)got, (unsigned long)len,
          level);
    if (err == TRX_INF_OK && got == len) {
        CHECK(!memcmp(src, back, len), "%s: the bytes differ (level %d strategy %d)", what, level, strategy);
    }
    free(back);
    free(cz);
}

static void test_inflate(void)
{
    const uint32_t N = 300000;
    uint8_t *zeros = (uint8_t *)calloc(N, 1);
    uint8_t *rnd = (uint8_t *)malloc(N);
    uint8_t *text = (uint8_t *)malloc(N);
    uint8_t *mix = (uint8_t *)malloc(N);
    srand(12345);
    for (uint32_t i = 0; i < N; i++) {
        rnd[i] = uint8_t(rand());
    }
    // JSON of the shape a .c64re has: long base64 runs and short keys
    static const char alpha[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (uint32_t i = 0; i < N; i++) {
        text[i] = uint8_t(alpha[rand() & 63]);
    }
    memcpy(text, "{\"schemaVersion\":1,\"ram\":{\"$ta\":\"Uint8Array\",\"b64\":\"", 50);
    // runs and matches at every distance: zeros, a repeated pattern, random
    for (uint32_t i = 0; i < N; i++) {
        mix[i] = uint8_t(i < N / 3 ? 0 : i < 2 * N / 3 ? "C64RE"[i % 5] : rand());
    }
    const struct {
        const char *name;
        const uint8_t *p;
        uint32_t n;
    } cases[] = {
        { "empty", zeros, 0 },      { "one byte", zeros, 1 },   { "zeros", zeros, N },
        { "random", rnd, N },       { "base64 text", text, N }, { "mixed", mix, N },
        { "short", (const uint8_t *)"C64RESNP", 8 },
    };
    const int strategies[] = { Z_DEFAULT_STRATEGY, Z_FILTERED, Z_HUFFMAN_ONLY, Z_RLE, Z_FIXED };
    for (unsigned c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        for (int level = 0; level <= 9; level++) {
            one_roundtrip(cases[c].name, cases[c].p, cases[c].n, level, Z_DEFAULT_STRATEGY);
        }
        for (unsigned s = 0; s < sizeof(strategies) / sizeof(strategies[0]); s++) {
            one_roundtrip(cases[c].name, cases[c].p, cases[c].n, 6, strategies[s]);
        }
    }

    // the refusals
    uint32_t clen = 0, got = 0;
    uint8_t *cz = gzip_it(text, N, 6, Z_DEFAULT_STRATEGY, &clen);
    uint8_t *back = (uint8_t *)malloc(N);
    CHECK(trx_gunzip(cz, clen, back, N / 2, &got) == TRX_INF_FULL, "a short output buffer is not reported");
    CHECK(trx_gunzip(cz, clen / 2, back, N, &got) == TRX_INF_TRUNCATED || clen < 36,
          "a cut-off body is not reported");
    uint8_t *bad = (uint8_t *)malloc(clen);
    memcpy(bad, cz, clen);
    bad[clen - 6] ^= 0xFF;                      // the CRC in the trailer
    CHECK(trx_gunzip(bad, clen, back, N, &got) == TRX_INF_BADCRC, "a wrong CRC is not reported");
    memcpy(bad, cz, clen);
    bad[0] = 'C';
    CHECK(trx_gunzip(bad, clen, back, N, &got) == TRX_INF_BADGZIP, "a file that is not gzip is not reported");
    memcpy(bad, cz, clen);
    bad[40] ^= 0xFF;                            // inside the deflate stream
    int e = trx_gunzip(bad, clen, back, N, &got);
    CHECK(e != TRX_INF_OK, "a damaged deflate stream came back as good");
    free(bad);
    free(back);
    free(cz);

    // a stored-block member, the shape this board writes itself
    {
        uint8_t hdr[10] = { 0x1F, 0x8B, 0x08, 0, 0, 0, 0, 0, 0, 0x03 };
        const char *body = "{\"a\":1}";
        uint32_t bl = uint32_t(strlen(body));
        uint8_t file[64];
        uint32_t o = 0;
        memcpy(file, hdr, 10);
        o = 10;
        file[o++] = 1;                          // final, stored
        file[o++] = uint8_t(bl);
        file[o++] = uint8_t(bl >> 8);
        file[o++] = uint8_t(~bl);
        file[o++] = uint8_t((~bl) >> 8);
        memcpy(file + o, body, bl);
        o += bl;
        uint32_t crc = uint32_t(crc32(0, (const Bytef *)body, bl));
        for (int i = 0; i < 4; i++) file[o++] = uint8_t(crc >> (8 * i));
        for (int i = 0; i < 4; i++) file[o++] = uint8_t(bl >> (8 * i));
        uint8_t out[32];
        uint32_t n = 0;
        CHECK(trx_gunzip(file, o, out, sizeof(out), &n) == TRX_INF_OK, "a stored-block member did not read");
        CHECK(n == bl && !memcmp(out, body, bl), "a stored-block member came back wrong");
    }
    free(zeros);
    free(rnd);
    free(text);
    free(mix);
}

// ---- the node-built block --------------------------------------------------------
static void b64_append(char **p, const uint8_t *src, int len)
{
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char *o = *p;
    int i = 0;
    for (; i + 3 <= len; i += 3) {
        uint32_t v = (uint32_t(src[i]) << 16) | (uint32_t(src[i + 1]) << 8) | src[i + 2];
        *o++ = t[(v >> 18) & 63];
        *o++ = t[(v >> 12) & 63];
        *o++ = t[(v >> 6) & 63];
        *o++ = t[v & 63];
    }
    if (i < len) {
        uint32_t v = uint32_t(src[i]) << 16;
        int rest = len - i;
        if (rest == 2) {
            v |= uint32_t(src[i + 1]) << 8;
        }
        *o++ = t[(v >> 18) & 63];
        *o++ = t[(v >> 12) & 63];
        *o++ = rest == 2 ? t[(v >> 6) & 63] : '=';
        *o++ = '=';
    }
    *p = o;
}

// A checkpoint as TRX64 writes one: no trx64Hw node.
static char *make_json(const uint8_t *ram, const uint8_t *color, int raster_line, int raster_cycle,
                       bool with_ram)
{
    char *js = (char *)malloc(600000);
    char *p = js;
    p += sprintf(p, "{\"manifest\":{\"machine\":{\"model\":\"c64-pal\"}},\"checkpoint\":{");
    p += sprintf(p, "\"schemaVersion\":1,\"atInstructionBoundary\":true,");
    p += sprintf(p, "\"cpu\":{\"pc\":%d,\"a\":%d,\"x\":%d,\"y\":%d,\"sp\":%d,\"flags\":%d,\"cycles\":99},",
                 0xC03A, 0x12, 0x34, 0x56, 0xF8, 0xA5);
    if (with_ram) {
        p += sprintf(p, "\"ram\":{\"$ta\":\"Uint8Array\",\"b64\":\"");
        b64_append(&p, ram, 65536);
        p += sprintf(p, "\"},");
    }
    p += sprintf(p, "\"cpuPortDirection\":47,\"cpuPortValue\":55,");
    // cia1: c_cia, the latches and counters, the mask in c_cia[13], the alarm
    p += sprintf(p, "\"cia1\":{\"v\":2,\"c_cia\":[%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d],",
                 0x7F, 0xFB, 0x00, 0xFF, 0x25, 0x40, 0x11, 0x22, 0x01, 0x23, 0x45, 0x91, 0x5A, 0x91, 0x11,
                 0x08);
    p += sprintf(p, "\"irqflags\":%d,\"ta_latch\":%d,\"ta_cnt\":%d,\"tb_latch\":%d,\"tb_cnt\":%d,",
                 0x81, 0x4025, 0x1234, 0x2211, 0x5678);
    p += sprintf(p, "\"todalarm\":[%d,%d,%d,%d]},", 0x05, 0x30, 0x15, 0x92);
    p += sprintf(p, "\"cia2\":{\"v\":2,\"c_cia\":[%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d],",
                 0x3F, 0x00, 0x3F, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0, 0, 0, 0x02, 0x08, 0x00);
    p += sprintf(p, "\"irqflags\":0,\"ta_latch\":%d,\"ta_cnt\":%d,\"tb_latch\":65535,\"tb_cnt\":65535,",
                 0xFFFF, 0xABCD);
    p += sprintf(p, "\"todalarm\":[0,0,0,0]},");
    p += sprintf(p, "\"vic\":{\"model\":0,\"regs\":[");
    for (int r = 0; r < 0x40; r++) {
        p += sprintf(p, "%s%d", r ? "," : "", r < 0x30 ? (r * 7 + 3) & 0xFF : 0xFF);
    }
    p += sprintf(p, "],\"raster_line\":%d,\"raster_cycle\":%d,\"irq_status\":%d,", raster_line, raster_cycle,
                 0x05);
    p += sprintf(p, "\"sprite_sprite_collisions\":%d,\"sprite_background_collisions\":%d,", 0x81, 0x42);
    p += sprintf(p, "\"color_ram\":{\"$ta\":\"Uint8Array\",\"b64\":\"");
    b64_append(&p, color, 1024);
    p += sprintf(p, "\"}},");
    p += sprintf(p, "\"cartFlash\":{\"$ta\":\"Uint8Array\",\"b64\":\"");
    {
        uint8_t fl[3000];
        for (int i = 0; i < 3000; i++) {
            fl[i] = uint8_t(255 - (i & 0xFF));
        }
        b64_append(&p, fl, 3000);
    }
    p += sprintf(p, "\"},");
    p += sprintf(p, "\"cartState\":{\"currentBank\":%d,\"controlRegister\":%d,\"cartridgeType\":17,\"cartRam\":{"
                    "\"$ta\":\"Uint8Array\",\"b64\":\"",
                 0x2A, 0x87);
    {
        uint8_t *cr = (uint8_t *)malloc(TRX_CART_RAM_BYTES);
        for (int i = 0; i < TRX_CART_RAM_BYTES; i++) {
            cr[i] = uint8_t(i * 13 + 7);
        }
        b64_append(&p, cr, TRX_CART_RAM_BYTES);
        free(cr);
    }
    p += sprintf(p, "\"}},");
    p += sprintf(p, "\"trx64Drive\":{\"regs\":true,\"cycles\":123456,\"pc\":%d,\"a\":%d,\"x\":%d,"
                    "\"y\":%d,\"sp\":%d,\"p\":%d,\"via1\":[",
                 0x0355, 0x11, 0x22, 0x33, 0xE8, 0x34);
    for (int i = 0; i < 16; i++) {
        p += sprintf(p, "%s%d", i ? "," : "", 0x50 + i);
    }
    p += sprintf(p, "],\"via2\":[");
    for (int i = 0; i < 16; i++) {
        p += sprintf(p, "%s%d", i ? "," : "", 0x60 + i);
    }
    p += sprintf(p, "],\"ram\":{\"$ta\":\"Uint8Array\",\"b64\":\"");
    {
        uint8_t dr[TRX_DRIVE_RAM];
        for (int i = 0; i < TRX_DRIVE_RAM; i++) {
            dr[i] = uint8_t(i * 5 + 1);
        }
        b64_append(&p, dr, TRX_DRIVE_RAM);
    }
    p += sprintf(p, "\"}},");
    p += sprintf(p, "\"keyboard\":{\"livePressed\":[\"A\",\"LeftShift\"]}");
    p += sprintf(p, "}}");
    return js;
}

static int parse(const char *js, jsmntok_t **tok)
{
    jsmn_parser pp;
    jsmn_init(&pp);
    int need = jsmn_parse(&pp, js, strlen(js), NULL, 0);
    if (need < 1) {
        return -1;
    }
    *tok = (jsmntok_t *)malloc(sizeof(jsmntok_t) * need);
    jsmn_init(&pp);
    return jsmn_parse(&pp, js, strlen(js), *tok, need);
}

static void test_nodes(void)
{
    uint8_t *ram = (uint8_t *)malloc(65536);
    uint8_t color[1024];
    for (int i = 0; i < 65536; i++) {
        ram[i] = uint8_t(i * 31 + (i >> 8));
    }
    for (int i = 0; i < 1024; i++) {
        color[i] = uint8_t(i | 0xB0);            // high nibbles must be dropped
    }
    char *js = make_json(ram, color, 142, 17, true);
    jsmntok_t *t = NULL;
    int n = parse(js, &t);
    CHECK(n > 0, "the test JSON does not parse");
    int cp = json_key(js, t, n, 0, "checkpoint");
    CHECK(cp > 0, "no checkpoint token");

    TrxNodeState st;
    uint8_t *outram = (uint8_t *)malloc(65536);
    uint8_t outcol[1024];
    // a base block with a pattern in every word: what no node names must survive
    for (int i = 0; i < TRX_BLOCK_WORDS; i++) {
        st.w[i] = 0xA5A50000u + uint32_t(i);
    }
    uint32_t base_seq = bits_of(st.w + W_CPUH, 102, 17);   // proc_control's state, a_mux, dout_mux, ...
    uint8_t *outcart = (uint8_t *)malloc(TRX_CART_RAM_BYTES);
    st.ram = outram;
    st.col = outcol;
    st.cart = outcart;
    st.x = 240;
    const char *err = trx_c64re_from_nodes(js, t, n, cp, &st);
    CHECK(err == NULL, "the build refused: %s", err ? err : "");

    // the CPU's registers, where the gateware reads them
    const uint32_t *h = st.w + W_CPUH;
    CHECK(bits_of(h, 0, 8) == 0x12, "A is %02x", bits_of(h, 0, 8));
    CHECK(bits_of(h, 8, 8) == 0x34, "X is %02x", bits_of(h, 8, 8));
    CHECK(bits_of(h, 16, 8) == 0x56, "Y is %02x", bits_of(h, 16, 8));
    CHECK(bits_of(h, 24, 8) == 0xF8, "S is %02x", bits_of(h, 24, 8));
    CHECK(bits_of(h, 32, 8) == 0xA5, "P is %02x", bits_of(h, 32, 8));
    CHECK(bits_of(h, 40, 8) == 0x3A, "PCL is %02x", bits_of(h, 40, 8));
    CHECK(bits_of(h, 48, 8) == 0xC0, "PCH is %02x", bits_of(h, 48, 8));
    CHECK(bits_of(h, 119, 1) == 0 && bits_of(h, 120, 1) == 0, "vectoring/intg not cleared");
    CHECK(bits_of(h, 127, 3) == 0, "nmi_d2 / nmi_act / resetting not cleared");
    CHECK(bits_of(h, 102, 17) == base_seq, "the sequencer state did not keep the base's value");
    CHECK(bits_of(h, 144, 8) == 47, "$00 is %u", bits_of(h, 144, 8));
    CHECK(bits_of(h, 152, 8) == 55, "$01 latch is %u", bits_of(h, 152, 8));
    CHECK((st.w[W_CPU] & 0xFFFFFFFFu) == 0xF8563412u, "the visible register word is %08x", st.w[W_CPU]);
    CHECK((st.w[W_CPU + 1] & 0xFFFFFF) == (0xC03Au | (0xA5u << 16)), "the visible PC/flags word is %08x",
          st.w[W_CPU + 1]);
    CHECK((st.w[W_CPU + 2] & 0xFFFFFF) == (47u | (55u << 8) | (0x37u << 16)), "the port word is %08x",
          st.w[W_CPU + 2]);

    // CIA 1: what the replay reads and what is staged
    const uint32_t *c1 = st.w + W_CIA1H;
    CHECK(bits_of(c1, 8, 8) == 0xFF, "cia1 DDRB is %02x", bits_of(c1, 8, 8));
    CHECK(bits_of(c1, 16, 8) == 0x00, "cia1 DDRA is %02x", bits_of(c1, 16, 8));
    CHECK(bits_of(c1, 24, 8) == 0xFB, "cia1 PRB is %02x", bits_of(c1, 24, 8));
    CHECK(bits_of(c1, 32, 8) == 0x7F, "cia1 PRA is %02x", bits_of(c1, 32, 8));
    CHECK(bits_of(c1, 43, 16) == 0x4025, "cia1 ta latch is %04x", bits_of(c1, 43, 16));
    CHECK(bits_of(c1, 59, 16) == 0x1234, "cia1 ta counter is %04x", bits_of(c1, 59, 16));
    CHECK(bits_of(c1, 83, 16) == 0x2211, "cia1 tb latch is %04x", bits_of(c1, 83, 16));
    CHECK(bits_of(c1, 99, 16) == 0x5678, "cia1 tb counter is %04x", bits_of(c1, 99, 16));
    CHECK(bits_of(c1, 123, 8) == 0x92, "cia1 alarm hour is %02x", bits_of(c1, 123, 8));
    CHECK(bits_of(c1, 131, 8) == 0x15, "cia1 alarm minute is %02x", bits_of(c1, 131, 8));
    CHECK(bits_of(c1, 139, 8) == 0x30, "cia1 alarm second is %02x", bits_of(c1, 139, 8));
    CHECK(bits_of(c1, 147, 8) == 0x05, "cia1 alarm tenth is %02x", bits_of(c1, 147, 8));
    CHECK(bits_of(c1, 155, 8) == 0x91, "cia1 TOD hour is %02x", bits_of(c1, 155, 8));
    CHECK(bits_of(c1, 163, 8) == 0x45, "cia1 TOD minute is %02x", bits_of(c1, 163, 8));
    CHECK(bits_of(c1, 171, 8) == 0x23, "cia1 TOD second is %02x", bits_of(c1, 171, 8));
    CHECK(bits_of(c1, 179, 8) == 0x01, "cia1 TOD tenth is %02x", bits_of(c1, 179, 8));
    CHECK(bits_of(c1, 244, 8) == 0x5A, "cia1 SDR is %02x", bits_of(c1, 244, 8));
    CHECK(bits_of(c1, 293, 8) == 0x81, "cia1 ICR image is %02x", bits_of(c1, 293, 8));
    CHECK(bits_of(c1, 303, 5) == 0x01, "cia1 interrupt flags are %02x", bits_of(c1, 303, 5));
    CHECK(bits_of(c1, 311, 5) == 0x11, "cia1 ICR mask is %02x", bits_of(c1, 311, 5));
    CHECK(bits_of(c1, 317, 8) == 0x11, "cia1 CRA is %02x", bits_of(c1, 317, 8));
    CHECK(bits_of(c1, 327, 8) == 0x08, "cia1 CRB is %02x", bits_of(c1, 327, 8));
    CHECK(byte_of(st.w, W_CIA1, 4) == 0x34 && byte_of(st.w, W_CIA1, 5) == 0x12,
          "the cia1 register image of the timer is wrong");
    CHECK(byte_of(st.w, W_CIA1, 13) == 0x81, "the cia1 register image of the ICR is %02x",
          byte_of(st.w, W_CIA1, 13));

    const uint32_t *c2 = st.w + W_CIA2H;
    CHECK(bits_of(c2, 32, 8) == 0x3F, "cia2 PRA is %02x", bits_of(c2, 32, 8));
    CHECK(bits_of(c2, 59, 16) == 0xABCD, "cia2 ta counter is %04x", bits_of(c2, 59, 16));
    CHECK(bits_of(c2, 311, 5) == 0x02, "cia2 ICR mask is %02x", bits_of(c2, 311, 5));

    // the VIC's registers, and the three latch bytes the restore compares
    for (int r = 0; r <= 0x2E; r++) {
        uint8_t want = uint8_t((r * 7 + 3) & 0xFF);
        if (r == 0x19) {
            want = 0x85;
        } else if (r == 0x1E) {
            want = 0x81;
        } else if (r == 0x1F) {
            want = 0x42;
        }
        CHECK(byte_of(st.w, W_VIC, r) == want, "VIC $d0%02x is %02x, not %02x", r, byte_of(st.w, W_VIC, r),
              want);
    }
    CHECK(st.line == 142 && st.cycle == 17, "the VIC position read back as %d/%d", st.line, st.cycle);
    CHECK(st.x == 240, "the snap position did not stay the base's: %u", st.x);
    CHECK(st.keys_pressed == 2, "%d keys were counted", st.keys_pressed);
    {   // T66 part 8: drive A out of the file's own node
        TrxDriveState ds;
        CHECK(trx_c64re_drive_from_node(js, t, n, cp, &ds), "the drive node was not read");
        CHECK(ds.regs && ds.pc == 0x0355 && ds.a == 0x11 && ds.x == 0x22 && ds.y == 0x33 &&
                  ds.sp == 0xE8 && ds.p == 0x34,
              "the drive registers came back as pc=%04x a=%02x x=%02x y=%02x sp=%02x p=%02x", ds.pc, ds.a,
              ds.x, ds.y, ds.sp, ds.p);
        CHECK(ds.cycles == 123456, "the drive cycles are %lu", (unsigned long)ds.cycles);
        int ok = 1;
        for (int i = 0; i < 16; i++) {
            if (ds.via1[i] != 0x50 + i || ds.via2[i] != 0x60 + i) {
                ok = 0;
            }
        }
        CHECK(ok, "the drive's VIA registers came back different");
        for (int i = 0; i < TRX_DRIVE_RAM; i++) {
            if (ds.ram[i] != uint8_t(i * 5 + 1)) {
                ok = 0;
            }
        }
        CHECK(ok, "the drive's RAM came back different");
    }
    CHECK((st.have & (TRX_NODE_CPU | TRX_NODE_PORT | TRX_NODE_RAM | TRX_NODE_CIA1 | TRX_NODE_CIA2 |
                      TRX_NODE_VIC | TRX_NODE_COLOR)) ==
              (TRX_NODE_CPU | TRX_NODE_PORT | TRX_NODE_RAM | TRX_NODE_CIA1 | TRX_NODE_CIA2 | TRX_NODE_VIC |
               TRX_NODE_COLOR),
          "the have mask is %04x", st.have);
    CHECK(!memcmp(outram, ram, 65536), "the RAM came back different");
    // the cartridge: the banking registers into chip 7 word 0, the "a cartridge
    // is on" bit so the replay writes them, and the RAM window
    CHECK((st.w[26] & 0xFFFF) == 0x872A, "chip 7 word 0 is %08x", st.w[26]);
    CHECK((st.w[0] & 0x08) != 0, "the cartridge bit is not set in the glue word");
    CHECK((st.have & TRX_NODE_CART) && (st.have & TRX_NODE_CARTRAM), "the cart flags are %04x", st.have);
    CHECK(st.cart == outcart, "the cartridge RAM was dropped");
    int cart_ok = 1;
    for (int i = 0; i < TRX_CART_RAM_BYTES; i++) {
        if (outcart[i] != uint8_t(i * 13 + 7)) {
            cart_ok = 0;
        }
    }
    CHECK(cart_ok, "the cartridge RAM came back different");
    CHECK(trx_c64re_cart_type(js, t, n, cp) == 17, "the cartridge type read as %d",
          trx_c64re_cart_type(js, t, n, cp));
    CHECK(trx_c64re_cart_flash_len(js, t, n, cp) == 3000, "cartFlash is %lu bytes long",
          (unsigned long)trx_c64re_cart_flash_len(js, t, n, cp));
    {
        uint8_t *fl = (uint8_t *)malloc(3000);
        CHECK(trx_c64re_cart_flash(js, t, n, cp, fl, 3000) == 3000, "cartFlash did not read");
        int ok = 1;
        for (int i = 0; i < 3000; i++) {
            if (fl[i] != uint8_t(255 - (i & 0xFF))) {
                ok = 0;
            }
        }
        CHECK(ok, "cartFlash came back different");
        free(fl);
    }
    {
        uint8_t *again = (uint8_t *)malloc(TRX_CART_RAM_BYTES);
        CHECK(trx_c64re_cart_ram(js, t, n, cp, again, TRX_CART_RAM_BYTES) == TRX_CART_RAM_BYTES,
              "trx_c64re_cart_ram did not read the node");
        CHECK(!memcmp(again, outcart, TRX_CART_RAM_BYTES), "trx_c64re_cart_ram read other bytes");
        free(again);
    }
    int nib_ok = 1;
    for (int i = 0; i < 1024; i++) {
        if (outcol[i] != (uint8_t(i | 0xB0) & 0x0F)) {
            nib_ok = 0;
        }
    }
    CHECK(nib_ok, "the colour RAM was not masked to its low nibbles");
    free(t);
    free(js);

    // the refusals
    js = make_json(ram, color, 400, 17, true);
    n = parse(js, &t);
    cp = json_key(js, t, n, 0, "checkpoint");
    err = trx_c64re_from_nodes(js, t, n, cp, &st);
    CHECK(err && strstr(err, "312"), "a raster line of 400 was not refused (%s)", err ? err : "accepted");
    free(t);
    free(js);

    js = make_json(ram, color, 10, 70, true);
    n = parse(js, &t);
    cp = json_key(js, t, n, 0, "checkpoint");
    err = trx_c64re_from_nodes(js, t, n, cp, &st);
    CHECK(err && strstr(err, "312"), "a raster cycle of 70 was not refused (%s)", err ? err : "accepted");
    free(t);
    free(js);

    js = make_json(ram, color, 10, 17, false);
    n = parse(js, &t);
    cp = json_key(js, t, n, 0, "checkpoint");
    err = trx_c64re_from_nodes(js, t, n, cp, &st);
    CHECK(err && strstr(err, "ram"), "a file without a ram node was not refused (%s)",
          err ? err : "accepted");
    free(t);
    free(js);

    // without the node: no cart flags, the window untouched, the base's word kept
    js = make_json(ram, color, 10, 17, true);
    {
        char *cs = strstr(js, "\"cartState\"");
        CHECK(cs != NULL, "the test JSON has no cartState");
        cs[1] = 'x';                            // rename the key: the node is gone
    }
    n = parse(js, &t);
    cp = json_key(js, t, n, 0, "checkpoint");
    for (int i = 0; i < TRX_BLOCK_WORDS; i++) {
        st.w[i] = 0xA5A50000u + uint32_t(i);
    }
    st.cart = outcart;
    err = trx_c64re_from_nodes(js, t, n, cp, &st);
    CHECK(err == NULL, "the build without a cartState refused: %s", err ? err : "");
    CHECK(st.w[26] == 0xA5A5001Au, "chip 7 word 0 did not keep the base's value: %08x", st.w[26]);
    CHECK(!(st.have & (TRX_NODE_CART | TRX_NODE_CARTRAM)), "cart flags without a node: %04x", st.have);
    CHECK(st.cart == NULL, "the cartridge RAM was not given up");
    free(t);
    free(js);

    err = trx_c64re_from_nodes("{}", NULL, 0, -1, &st);
    CHECK(err != NULL, "a file without a checkpoint was not refused");

    free(outcart);
    free(outram);
    free(ram);
}

// ---- the log records between a file and an area ----------------------------------
// A fake area: a byte array, with every access checked for the alignment the
// mover needs.
struct FakeArea {
    uint8_t *mem;
    uint32_t size;
    int bad_align, reads, writes;
};

static void fake_rd(void *ctx, uint32_t off, uint8_t *p, uint32_t len)
{
    FakeArea *a = (FakeArea *)ctx;
    if ((off & 15) || (len & 15) || off + len > a->size) {
        a->bad_align++;
        return;
    }
    a->reads++;
    memcpy(p, a->mem + off, len);
}

static void fake_wr(void *ctx, uint32_t off, const uint8_t *p, uint32_t len)
{
    FakeArea *a = (FakeArea *)ctx;
    if ((off & 15) || (len & 15) || off + len > a->size) {
        a->bad_align++;
        return;
    }
    a->writes++;
    memcpy(a->mem + off, p, len);
}

static void test_recs(void)
{
    const uint32_t SIZE = 4096;
    for (uint32_t rec = 4; rec <= 8; rec += 4) {
        for (uint32_t chunk = 64; chunk <= 1024; chunk *= 4) {
            FakeArea a;
            a.mem = (uint8_t *)malloc(SIZE);
            a.size = SIZE;
            a.bad_align = a.reads = a.writes = 0;
            memset(a.mem, 0xEE, SIZE);
            TrxRecIo io = { SIZE, rec, chunk, &a, fake_rd, fake_wr };

            // a run that wraps the area and starts at an odd record
            uint32_t caps = SIZE / rec;
            uint32_t n_recs = caps - 7;
            uint32_t from = 0x0FFFFFFFu - n_recs + 1;    // the rebase the save does
            uint32_t n = n_recs * rec;
            uint8_t *src = (uint8_t *)malloc(n);
            for (uint32_t i = 0; i < n; i++) {
                src[i] = uint8_t(i * 7 + rec + chunk);
            }
            uint8_t *stage = (uint8_t *)malloc(chunk + 16);
            trx_recs_out(&io, from, src, n, stage);
            CHECK(a.bad_align == 0, "rec %u chunk %u: %d accesses were not aligned", rec, chunk,
                  a.bad_align);

            // every byte of the run is where its count says
            int wrong = 0;
            for (uint32_t i = 0; i < n; i++) {
                uint32_t off = (((from + i / rec) * rec) & (SIZE - 1)) + (i % rec);
                if (a.mem[off & (SIZE - 1)] != src[i]) {
                    wrong++;
                }
            }
            CHECK(wrong == 0, "rec %u chunk %u: %d bytes landed in the wrong place", rec, chunk, wrong);

            // the records the run does not cover were not touched
            int touched = 0;
            for (uint32_t k = 0; k < 7; k++) {
                uint32_t off = ((from - 1 - k) * rec) & (SIZE - 1);
                for (uint32_t b = 0; b < rec; b++) {
                    if (a.mem[(off + b) & (SIZE - 1)] != 0xEE) {
                        touched++;
                    }
                }
            }
            CHECK(touched == 0, "rec %u chunk %u: %d bytes outside the run were overwritten", rec, chunk,
                  touched);

            // and back out again, the way the save reads them
            uint8_t *back = (uint8_t *)malloc(n);
            uint8_t *buf = (uint8_t *)malloc(chunk + 16);
            uint32_t done = 0, cnt = from, left = n_recs;
            int steps = 0;
            while (left && steps < 1000) {
                uint32_t at = 0;
                uint32_t got = trx_recs_in(&io, cnt, left, buf, &at);
                if (!got) {
                    break;
                }
                memcpy(back + done, buf + at, got);
                done += got;
                cnt = (cnt + got / rec) & 0x0FFFFFFFu;
                left -= got / rec;
                steps++;
            }
            CHECK(done == n, "rec %u chunk %u: %u bytes came back, not %u", rec, chunk, done, n);
            CHECK(done == n && !memcmp(back, src, n), "rec %u chunk %u: the records came back different",
                  rec, chunk);
            CHECK(a.bad_align == 0, "rec %u chunk %u: reading was not aligned", rec, chunk);
            free(back);
            free(buf);
            free(stage);
            free(src);
            free(a.mem);
        }
    }
}

// ---- sha256 ----------------------------------------------------------------------
static void test_sha(void)
{
    struct {
        const char *in;
        int repeat;
        const char *want;
    } v[] = {
        { "abc", 1, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad" },
        { "", 1, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855" },
        { "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 1,
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1" },
        { "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
          10000, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0" },
    };
    for (unsigned i = 0; i < sizeof(v) / sizeof(v[0]); i++) {
        TrxSha256 s;
        uint8_t d[32];
        char hex[65];
        trx_sha_init(&s);
        for (int k = 0; k < v[i].repeat; k++) {
            trx_sha_add(&s, (const uint8_t *)v[i].in, uint32_t(strlen(v[i].in)));
        }
        trx_sha_final(&s, d);
        for (int k = 0; k < 32; k++) {
            sprintf(hex + 2 * k, "%02x", d[k]);
        }
        CHECK(!strcmp(hex, v[i].want), "sha256 vector %u is %s", i, hex);
    }
}

int main(void)
{
    test_inflate();
    test_nodes();
    test_recs();
    test_sha();
    printf("%d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
