/*
 * C64RE Snapshot - writes the machine as a .c64re file.
 *
 * The container the TRX64 runtime reads: "C64RESNP", a format byte, a sha256
 * over the body, then gzip'd JSON. What goes in is what this machine can
 * honestly show: the 64 KiB of RAM and the colour RAM as they were when the
 * menu took over, the CPU port, the VIC registers, and the media that are in
 * the machine. The 6510's own registers and the internal counters of VIC, CIA
 * and SID are on no bus here and are left out; every node is optional on the
 * reading side, so the dump opens - as a machine to look at, not one that
 * carries on running.
 */

#include "u2p_app.h"
#include <stdint.h>

/* Every step goes to the syslog, and each line gets a moment to leave: the
 * syslog is sent from its own task, and a machine that hangs a step later
 * would otherwise take the line that says where with it. */
static const u2p_api_t *s_log_api;
#define LOG(...) do { if (s_log_api) { s_log_api->sys->log("[C64RE] " __VA_ARGS__); \
                                        s_log_api->sys->delay_ms(20); } } while (0)

/* Apps link without a C library, so the handful of routines this needs live
 * here. memcpy and memset are not static: the compiler emits calls to them. */
void *memcpy(void *d, const void *s, size_t n)
{
    unsigned char *dd = (unsigned char *)d;
    const unsigned char *ss = (const unsigned char *)s;
    while (n--) { *dd++ = *ss++; }
    return d;
}

void *memset(void *d, int c, size_t n)
{
    unsigned char *dd = (unsigned char *)d;
    while (n--) { *dd++ = (unsigned char)c; }
    return d;
}

static int a_strlen(const char *s)
{
    int n = 0;
    while (s[n]) { n++; }
    return n;
}

static const char *a_strrchr(const char *s, char c)
{
    const char *found = 0;
    while (*s) { if (*s == c) { found = s; } s++; }
    return found;
}

static void a_strcat(char *dst, const char *src, int limit)
{
    int n = a_strlen(dst);
    while (*src && (n < limit - 1)) { dst[n++] = *src++; }
    dst[n] = 0;
}

/* ------------------------------------------------------------------ */
/* SHA-256                                                             */
/* ------------------------------------------------------------------ */
typedef struct {
    uint32_t h[8];
    uint8_t block[64];
    uint32_t len_lo, len_hi;
    int fill;
} sha256_t;

static const uint32_t sha_k[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

static uint32_t ror32(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

static void sha256_compress(sha256_t *s, const uint8_t *p)
{
    uint32_t w[64], a, b, c, d, e, f, g, hh;
    int i;
    for (i = 0; i < 16; i++) {
        w[i] = ((uint32_t)p[4*i] << 24) | ((uint32_t)p[4*i+1] << 16) | ((uint32_t)p[4*i+2] << 8) | p[4*i+3];
    }
    for (i = 16; i < 64; i++) {
        uint32_t s0 = ror32(w[i-15], 7) ^ ror32(w[i-15], 18) ^ (w[i-15] >> 3);
        uint32_t s1 = ror32(w[i-2], 17) ^ ror32(w[i-2], 19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    a = s->h[0]; b = s->h[1]; c = s->h[2]; d = s->h[3];
    e = s->h[4]; f = s->h[5]; g = s->h[6]; hh = s->h[7];
    for (i = 0; i < 64; i++) {
        uint32_t t1 = hh + (ror32(e, 6) ^ ror32(e, 11) ^ ror32(e, 25)) + ((e & f) ^ ((~e) & g)) + sha_k[i] + w[i];
        uint32_t t2 = (ror32(a, 2) ^ ror32(a, 13) ^ ror32(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        hh = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d;
    s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += hh;
}

static void sha256_init(sha256_t *s)
{
    static const uint32_t iv[8] = { 0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                                    0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u };
    memcpy(s->h, iv, sizeof(iv));
    s->len_lo = s->len_hi = 0;
    s->fill = 0;
}

static void sha256_add(sha256_t *s, const uint8_t *p, uint32_t len)
{
    uint32_t before = s->len_lo;
    s->len_lo += len;
    if (s->len_lo < before) {
        s->len_hi++;
    }
    while (len) {
        int take = 64 - s->fill;
        if ((uint32_t)take > len) {
            take = (int)len;
        }
        memcpy(s->block + s->fill, p, take);
        s->fill += take;
        p += take;
        len -= (uint32_t)take;
        if (s->fill == 64) {
            sha256_compress(s, s->block);
            s->fill = 0;
        }
    }
}

static void sha256_final(sha256_t *s, uint8_t *out)
{
    uint64_t bits = (((uint64_t)s->len_hi << 32) | s->len_lo) * 8;
    uint8_t pad = 0x80, tail[8];
    int i;
    sha256_add(s, &pad, 1);
    pad = 0;
    while (s->fill != 56) {
        sha256_add(s, &pad, 1);
    }
    for (i = 0; i < 8; i++) {
        tail[i] = (uint8_t)(bits >> (56 - 8 * i));
    }
    sha256_add(s, tail, 8);
    for (i = 0; i < 8; i++) {
        out[4*i]   = (uint8_t)(s->h[i] >> 24);
        out[4*i+1] = (uint8_t)(s->h[i] >> 16);
        out[4*i+2] = (uint8_t)(s->h[i] >> 8);
        out[4*i+3] = (uint8_t)(s->h[i]);
    }
}

/* ------------------------------------------------------------------ */
/* gzip with stored blocks, written as the document is produced        */
/* ------------------------------------------------------------------ */
#define GZ_BLOCK 2048

typedef struct {
    const u2p_api_t *api;
    void *f;
    uint8_t buf[GZ_BLOCK];
    int fill;
    uint32_t crc;
    uint32_t isize;
    sha256_t sha;      /* over the body, for the container header */
    uint8_t carry[3];
    int carry_fill;
    int failed;
    uint32_t written;  /* bytes handed to fwrite, for the log */
} gz_t;

static uint32_t crc32_byte(uint32_t crc, uint8_t b)
{
    int i;
    crc ^= b;
    for (i = 0; i < 8; i++) {
        crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1)));
    }
    return crc;
}

static void gz_raw(gz_t *g, const uint8_t *p, int len)
{
    if (g->failed) {
        return;
    }
    sha256_add(&g->sha, p, (uint32_t)len);
    if (g->api->fs->fwrite(p, 1, len, g->f) != len) {
        LOG("fwrite of %d bytes failed after %d\n", len, (int)g->written);
        g->failed = 1;
    }
    g->written += (uint32_t)len;
}

static void gz_flush(gz_t *g, int final)
{
    uint8_t head[5];
    head[0] = (uint8_t)(final ? 1 : 0);
    head[1] = (uint8_t)(g->fill);
    head[2] = (uint8_t)(g->fill >> 8);
    head[3] = (uint8_t)(~g->fill);
    head[4] = (uint8_t)((~g->fill) >> 8);
    gz_raw(g, head, 5);
    if (g->fill) {
        gz_raw(g, g->buf, g->fill);
    }
    g->fill = 0;
}

static void gz_init(gz_t *g, const u2p_api_t *api, void *f)
{
    static const uint8_t header[10] = { 0x1F, 0x8B, 0x08, 0, 0, 0, 0, 0, 0, 0x03 };
    memset(g, 0, sizeof(*g));
    g->api = api;
    g->f = f;
    g->crc = 0xFFFFFFFFu;
    sha256_init(&g->sha);
    gz_raw(g, header, sizeof(header));
}

static void gz_put(gz_t *g, const char *s, int len)
{
    int i;
    if (len < 0) {
        len = a_strlen(s);
    }
    for (i = 0; i < len; i++) {
        g->crc = crc32_byte(g->crc, (uint8_t)s[i]);
        g->isize++;
        g->buf[g->fill++] = (uint8_t)s[i];
        if (g->fill == GZ_BLOCK) {
            gz_flush(g, 0);
        }
    }
}

static void gz_str(gz_t *g, const char *s) { gz_put(g, s, -1); }

static void gz_num(gz_t *g, const char *prefix, long value)
{
    char tmp[16];
    int n = 0, i;
    if (prefix) {
        gz_str(g, prefix);
    }
    if (value < 0) {
        gz_put(g, "-", 1);
        value = -value;
    }
    do {
        tmp[n++] = (char)('0' + (value % 10));
        value /= 10;
    } while (value);
    for (i = n - 1; i >= 0; i--) {
        gz_put(g, &tmp[i], 1);
    }
}

static const char b64_tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void gz_b64_begin(gz_t *g) { g->carry_fill = 0; }

static void gz_b64_add(gz_t *g, const uint8_t *data, int len)
{
    char out[4];
    int i;
    for (i = 0; i < len; i++) {
        g->carry[g->carry_fill++] = data[i];
        if (g->carry_fill == 3) {
            uint32_t v = ((uint32_t)g->carry[0] << 16) | ((uint32_t)g->carry[1] << 8) | g->carry[2];
            out[0] = b64_tbl[(v >> 18) & 63]; out[1] = b64_tbl[(v >> 12) & 63];
            out[2] = b64_tbl[(v >> 6) & 63];  out[3] = b64_tbl[v & 63];
            gz_put(g, out, 4);
            g->carry_fill = 0;
        }
    }
}

static void gz_b64_end(gz_t *g)
{
    char out[4];
    uint32_t v;
    if (!g->carry_fill) {
        return;
    }
    v = (uint32_t)g->carry[0] << 16;
    if (g->carry_fill == 2) {
        v |= (uint32_t)g->carry[1] << 8;
    }
    out[0] = b64_tbl[(v >> 18) & 63];
    out[1] = b64_tbl[(v >> 12) & 63];
    out[2] = (g->carry_fill == 2) ? b64_tbl[(v >> 6) & 63] : '=';
    out[3] = '=';
    gz_put(g, out, 4);
    g->carry_fill = 0;
}

static void gz_ta(gz_t *g, const uint8_t *data, int len)
{
    gz_str(g, "{\"$ta\":\"Uint8Array\",\"b64\":\"");
    gz_b64_begin(g);
    gz_b64_add(g, data, len);
    gz_b64_end(g);
    gz_str(g, "\"}");
}

static void gz_array(gz_t *g, const uint8_t *data, int count, int mask)
{
    int i;
    gz_str(g, "[");
    for (i = 0; i < count; i++) {
        gz_num(g, i ? "," : NULL, data[i] & mask);
    }
    gz_str(g, "]");
}

static void gz_zeros(gz_t *g, int count)
{
    int i;
    gz_str(g, "[");
    for (i = 0; i < count; i++) {
        gz_str(g, i ? ",0" : "0");
    }
    gz_str(g, "]");
}

static int gz_finish(gz_t *g, uint8_t *sha_out)
{
    uint8_t tail[8];
    int i;
    gz_flush(g, 1);
    g->crc ^= 0xFFFFFFFFu;
    for (i = 0; i < 4; i++) {
        tail[i] = (uint8_t)(g->crc >> (8 * i));
        tail[4+i] = (uint8_t)(g->isize >> (8 * i));
    }
    gz_raw(g, tail, 8);
    sha256_final(&g->sha, sha_out);
    return !g->failed;
}

/* ------------------------------------------------------------------ */
/* The VIC node                                                        */
/* ------------------------------------------------------------------ */
/*
 * All of it or nothing: the reader has no defaults for this node, so a half
 * one makes the whole checkpoint fail. Registers, colour RAM and the raster
 * line are read from the machine; the internal counters and the draw pipeline
 * are not on any bus here and go out cold, which is where a restored machine
 * starts anyway.
 */
static void write_vic(gz_t *g, const uint8_t *regs, const uint8_t *color)
{
    int raster = regs[0x12] | ((regs[0x11] & 0x80) ? 256 : 0);
    uint8_t zeros[0x2f];
    int i;

    memset(zeros, 0, sizeof(zeros));

    gz_str(g, "\"vic\":{\"model\":0,\"regs\":");
    gz_array(g, regs, 0x40, 0xFF);
    gz_num(g, ",\"raster_cycle\":0,\"cycle_flags\":0,\"raster_line\":", raster);
    gz_str(g, ",\"start_of_frame\":0,\"irq_status\":0,\"raster_irq_line\":0,\"raster_irq_triggered\":0");
    gz_str(g, ",\"vbuf\":");   gz_zeros(g, 40);
    gz_str(g, ",\"cbuf\":");   gz_zeros(g, 40);
    gz_str(g, ",\"gbuf\":0,\"dbuf_offset\":0,\"dbuf\":"); gz_zeros(g, 520);
    gz_str(g, ",\"ysmooth\":0,\"allow_bad_lines\":0,\"sprite_sprite_collisions\":0");
    gz_str(g, ",\"sprite_background_collisions\":0,\"clear_collisions\":0,\"idle_state\":0");
    gz_str(g, ",\"vcbase\":0,\"vc\":0,\"rc\":0,\"vmli\":0,\"bad_line\":0");
    gz_str(g, ",\"light_pen\":{\"state\":0,\"triggered\":0,\"x\":0,\"y\":0,\"x_extra_bits\":0,\"trigger_cycle\":0}");
    gz_str(g, ",\"reg11_delay\":0,\"prefetch_cycles\":0,\"sprite_display_bits\":0,\"sprite_dma\":0");
    gz_str(g, ",\"last_color_reg\":0,\"last_color_value\":0,\"last_read_phi1\":0,\"last_bus_phi2\":0");
    gz_str(g, ",\"vborder\":0,\"set_vborder\":0,\"main_border\":0,\"refresh_counter\":0");
    gz_str(g, ",\"color_ram\":");
    gz_array(g, color, 0x400, 0x0F);

    gz_str(g, ",\"sprite\":[");
    for (i = 0; i < 8; i++) {
        gz_str(g, i ? ",{" : "{");
        gz_str(g, "\"data\":0,\"mc\":0,\"mcbase\":0,\"pointer\":0,\"exp_flop\":0");
        gz_num(g, ",\"x\":", regs[i * 2]);
        gz_str(g, "}");
    }
    gz_str(g, "]");

    gz_str(g, ",\"drawCycle\":{\"gbuf_pipe0_reg\":0,\"cbuf_pipe0_reg\":0,\"vbuf_pipe0_reg\":0");
    gz_str(g, ",\"gbuf_pipe1_reg\":0,\"cbuf_pipe1_reg\":0,\"vbuf_pipe1_reg\":0");
    gz_str(g, ",\"xscroll_pipe\":0,\"vmode11_pipe\":0,\"vmode16_pipe\":0,\"vmode16_pipe2\":0");
    gz_str(g, ",\"gbuf_reg\":0,\"gbuf_mc_flop\":0,\"gbuf_pixel_reg\":0,\"cbuf_reg\":0,\"vbuf_reg\":0,\"dmli\":0");
    gz_str(g, ",\"sprite_x_pipe\":"); gz_zeros(g, 8);
    gz_str(g, ",\"sprite_pri_bits\":0,\"sprite_mc_bits\":0,\"sprite_expx_bits\":0");
    gz_str(g, ",\"sprite_pending_bits\":0,\"sprite_active_bits\":0,\"sprite_halt_bits\":0");
    gz_str(g, ",\"sbuf_reg\":");        gz_ta(g, zeros, 8);
    gz_str(g, ",\"sbuf_pixel_reg\":");  gz_ta(g, zeros, 8);
    gz_str(g, ",\"sbuf_expx_flops\":0,\"sbuf_mc_flops\":0,\"border_state\":0");
    gz_str(g, ",\"render_buffer\":");   gz_ta(g, zeros, 8);
    gz_str(g, ",\"pri_buffer\":");      gz_ta(g, zeros, 8);
    gz_str(g, ",\"pixel_buffer\":");    gz_ta(g, zeros, 8);
    gz_str(g, ",\"cregs\":");           gz_ta(g, zeros, 0x2f);
    gz_str(g, ",\"last_color_reg\":0,\"last_color_value\":0,\"cycle_flags_pipe\":0}");
    gz_str(g, "}");
}

/* ------------------------------------------------------------------ */
/* Media                                                               */
/* ------------------------------------------------------------------ */
typedef struct {
    char ref[12];
    const char *role;
    char format[8];
    const char *name;
    char sha[65];
} media_t;

static const char *base_name(const char *path)
{
    const char *slash = a_strrchr(path, '/');
    return slash ? slash + 1 : path;
}

/* A medium travels inside the dump, so the file stands on its own. */
static int embed_medium(gz_t *g, const u2p_api_t *api, int role_id,
                        const char *role, media_t *out, int index, int first)
{
    char path[256];
    uint8_t chunk[512];
    sha256_t sha;
    uint8_t digest[32];
    static const char *hexd = "0123456789abcdef";
    const char *ext;
    void *src;
    int got, i, total;

    if (api->media->source(role_id, path, sizeof(path)) <= 0) {
        LOG("%s: no medium\n", role);
        return 0;
    }
    LOG("%s: opening %s\n", role, path);
    src = api->fs->fopen(path, "rb");
    if (!src) {
        LOG("%s: could not open it\n", role);
        return 0;
    }

    out->ref[0] = 'm'; out->ref[1] = 'e'; out->ref[2] = 'd'; out->ref[3] = 'i';
    out->ref[4] = 'a'; out->ref[5] = (char)('0' + index); out->ref[6] = 0;
    out->role = role;
    out->name = base_name(path);
    out->format[0] = 0;
    ext = a_strrchr(out->name, '.');
    if (ext) {
        for (i = 0; (i < (int)sizeof(out->format) - 1) && ext[i + 1]; i++) {
            char c = ext[i + 1];
            out->format[i] = (char)((c >= 'A' && c <= 'Z') ? (c + 32) : c);
        }
        out->format[i] = 0;
    }

    gz_str(g, first ? "\"" : ",\"");
    gz_str(g, out->ref);
    gz_str(g, "\":\"");
    sha256_init(&sha);
    gz_b64_begin(g);
    total = 0;
    while ((got = api->fs->fread(chunk, 1, (int)sizeof(chunk), src)) > 0) {
        sha256_add(&sha, chunk, (uint32_t)got);
        gz_b64_add(g, chunk, got);
        total += got;
        if ((total & 0xFFFF) == 0) {
            LOG("%s: %d bytes embedded, %d written\n", role, total, (int)g->written);
        }
    }
    gz_b64_end(g);
    gz_str(g, "\"");
    api->fs->fclose(src);
    LOG("%s: %d bytes embedded, failed=%d\n", role, total, g->failed);

    sha256_final(&sha, digest);
    for (i = 0; i < 32; i++) {
        out->sha[2*i] = hexd[digest[i] >> 4];
        out->sha[2*i+1] = hexd[digest[i] & 15];
    }
    out->sha[64] = 0;
    return 1;
}

/* ------------------------------------------------------------------ */
/* The dump                                                            */
/* ------------------------------------------------------------------ */
static int write_dump(const u2p_api_t *api, const char *pathname)
{
    uint8_t header[41];
    uint8_t vicregs[0x40];
    uint8_t color[0x400];
    uint8_t digest[32];
    uint8_t *ram;
    media_t media[3];
    gz_t gz;
    void *f;
    int count = 0, i, offset, ok;

    ram = (uint8_t *)api->mem->malloc(65536);
    if (!ram) {
        api->ui->popup("C64RE", "Not enough memory for the RAM image.");
        return 1;
    }

    LOG("opening %s\n", pathname);
    f = api->fs->fopen(pathname, "wb");
    if (!f) {
        LOG("could not create it\n");
        api->mem->free(ram);
        api->ui->popup("C64RE", "Could not create the file.");
        return 1;
    }

    /* The hash covers the body, so it is not known yet: leave room and come
     * back to it once the document has been written. */
    memset(header, 0, sizeof(header));
    memcpy(header, "C64RESNP", 8);
    header[8] = 1;
    i = api->fs->fwrite(header, 1, (int)sizeof(header), f);
    LOG("header: %d of %d bytes written\n", i, (int)sizeof(header));

    memset(vicregs, 0, sizeof(vicregs));
    memset(color, 0, sizeof(color));
    if (api->c64->dma_read_frozen) {
        for (offset = 0; offset < 65536; offset += 4096) {
            LOG("RAM $%04x..\n", offset);
            api->c64->dma_read_frozen((uint16_t)offset, ram + offset, 4096);
        }
        LOG("VIC registers..\n");
        api->c64->dma_read_frozen(0xD000, vicregs, 0x30);
        LOG("colour RAM..\n");
        api->c64->dma_read_frozen(0xD800, color, 0x400);
        LOG("machine read\n");
    } else {
        LOG("no c64_dma_read_frozen, reading the bus as it stands\n");
        /* Older firmware: the bus as it stands, which means the menu's screen
         * rather than the machine's. */
        for (offset = 0; offset < 65536; offset += 4096) {
            api->c64->dma_read((uint16_t)offset, ram + offset, 4096);
        }
        api->c64->dma_read(0xD000, vicregs, 0x30);
        api->c64->dma_read(0xD800, color, 0x400);
    }

    gz_init(&gz, api, f);
    LOG("gzip header: failed=%d\n", gz.failed);

    /* Media first: a hash is only known once its file has been read, and the
     * manifest that names them comes after. */
    gz_str(&gz, "{\"mediaPayloads\":{");
    if (embed_medium(&gz, api, 0, "cartridge", &media[count], count, count == 0)) {
        count++;
    }
    if (embed_medium(&gz, api, 1, "drive8", &media[count], count, count == 0)) {
        count++;
    }
    gz_str(&gz, "}");

    gz_str(&gz, ",\"manifest\":{\"kind\":\"c64re-runtime-snapshot\",\"version\":1");
    gz_str(&gz, ",\"machine\":{\"model\":\"c64-pal\",\"runtimeVersion\":\"ultimate-app/1\"}");
    gz_str(&gz, ",\"checkpoint\":{\"encoding\":\"ta-json-gz/1\",\"payloadRef\":\"checkpoint\",\"cycle\":0,\"pc\":0}");
    gz_str(&gz, ",\"media\":[");
    for (i = 0; i < count; i++) {
        gz_str(&gz, i ? ",{\"embeddedPayloadRef\":\"" : "{\"embeddedPayloadRef\":\"");
        gz_str(&gz, media[i].ref);
        gz_str(&gz, "\",\"role\":\"");
        gz_str(&gz, media[i].role);
        gz_str(&gz, "\",\"format\":\"");
        gz_str(&gz, media[i].format);
        gz_str(&gz, "\",\"sha256\":\"");
        gz_str(&gz, media[i].sha);
        gz_str(&gz, "\",\"sourceName\":\"");
        gz_str(&gz, media[i].name);
        gz_str(&gz, "\"}");
    }
    gz_str(&gz, "]}");

    LOG("%d media embedded, writing the checkpoint\n", count);
    gz_str(&gz, ",\"checkpoint\":{\"schemaVersion\":1,\"ram\":");
    gz_ta(&gz, ram, 65536);
    gz_num(&gz, ",\"cpuPortDirection\":", ram[0]);
    gz_num(&gz, ",\"cpuPortValue\":", ram[1]);
    gz_str(&gz, ",");
    write_vic(&gz, vicregs, color);
    gz_str(&gz, "}}");

    ok = gz_finish(&gz, digest);
    api->mem->free(ram);
    LOG("body done: %d bytes, ok=%d\n", (int)gz.written, ok);

    if (ok) {
        api->fs->fseek(f, 9, 0);
        ok = (api->fs->fwrite(digest, 1, (int)sizeof(digest), f) == (int)sizeof(digest));
        LOG("hash written: ok=%d\n", ok);
    }
    i = api->fs->fclose(f);
    LOG("closed: %d\n", i);
    return ok ? 0 : 1;
}

int app_main(const u2p_api_t *api, const u2p_app_context_t *ctx)
{
    char path[256];
    char name[64];
    void *progress;
    int len, rc;

    (void)ctx;
    s_log_api = api;

    path[0] = 0;
    if (api->ui->path_input(path, (int)sizeof(path)) <= 0) {
        return 0;
    }
    name[0] = 0;
    if ((api->ui->string_input("Save .C64RE dump as..", name, 22) <= 0) || !name[0]) {
        return 0;
    }

    len = a_strlen(path);
    if (len && (path[len - 1] != '/')) {
        path[len++] = '/';
        path[len] = 0;
    }
    a_strcat(path, name, (int)sizeof(path) - 8);
    if (!a_strrchr(base_name(path), '.')) {
        a_strcat(path, ".c64re", (int)sizeof(path));
    }

    progress = api->ui->progress_open("Writing .c64re dump..", 1);
    LOG("start: %s\n", path);
    rc = write_dump(api, path);
    LOG("done: rc=%d\n", rc);
    api->ui->progress_close(progress);

    if (rc) {
        api->ui->popup("C64RE", "Writing the dump failed.");
    }
    return rc;
}
