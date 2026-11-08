// See trx_trxring.h. The file:
//
//   0..7    "TRXRING1"
//   8..11   u32 header length (little endian)
//   12..    the JSON header below, then, in this order, the parts it gives
//           lengths for: the timeline (TrxFrame entries, oldest first, as they
//           lie in memory), C64 RAM, colour RAM, cartridge RAM, then the
//           records of streams 0 and 1 from the oldest entry's start to the
//           newest entry's end.
//
// The counts in the timeline are rebased on the way out so the newest entry's
// are 0, which is what the engine's counters hold after the restart a load
// does (trx_ring.h says why that makes the records land in the right place).
// Stream 2 is not in the file: the timeline is what the parse made of it.
//
// Two digests: sha256State over the timeline and the memories, checked before
// anything is put back, and sha256Log over the stream records, computed while
// they are written back - a damaged log costs rewinding, not the machine's
// state, so it is a message and not a refusal.
#include "trx_trxring.h"

#if (U64 == 2) && !defined(RECOVERYAPP)

#include "trx_ring.h"
#include "trx_io.h"
#include "trx_sha256.h"
#include "trx_json.h"
#include "trx_ring_recs.h"
#include "c1541.h"
#include "c64_crt.h"
#include "filemanager.h"
#include "itu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern C1541 *c1541_A;

#define M28         0x0FFFFFFFu
#define W_GLUE      0               // chip 0 word 0 of the block
#define GLUE_CART   0x08            // its bit 3: a cartridge is on (c64_core.v s_cart)
#define CHUNK       0x8000          // the buffer the areas and the file trade in
#define HDR_MAX     2048
#define VERSION     1

// ---- small helpers ---------------------------------------------------------------
// The mover takes 16-byte aligned addresses on both sides (trx_engine.v), so
// every buffer that meets it is aligned by hand; raw is what free() wants.
struct Buf {
    uint8_t *raw, *p;
};

static bool buf_alloc(Buf *b, uint32_t n)
{
    b->raw = (uint8_t *)malloc(n + 15);
    b->p = b->raw ? (uint8_t *)(((uintptr_t)b->raw + 15) & ~(uintptr_t)15) : NULL;
    return b->p != NULL;
}

static void buf_free(Buf *b)
{
    free(b->raw);
    b->raw = NULL;
    b->p = NULL;
}

static void hexout(char *dst, const uint8_t *d, int n)
{
    static const char h[] = "0123456789abcdef";
    for (int i = 0; i < n; i++) {
        dst[2 * i] = h[d[i] >> 4];
        dst[2 * i + 1] = h[d[i] & 15];
    }
}

static bool hexeq(const char *hex, const uint8_t *d, int n)
{
    char want[64];
    hexout(want, d, n);
    return !strncmp(hex, want, 2 * n);
}

// The directory part of a path, with the trailing slash ("/Usb0/a/b" -> "/Usb0/a/").
static void dir_of(const char *path, char *out, int max)
{
    const char *slash = strrchr(path, '/');
    int n = slash ? int(slash - path) + 1 : 0;
    if (n >= max) {
        n = max - 1;
    }
    memcpy(out, path, n);
    out[n] = 0;
}

// Copies src to dir/name unless a file of that name is already there. Returns
// 1 copied, 0 left alone (it exists), -1 it did not work.
static int copy_beside(const char *dir, const char *src, const char *name)
{
    FileManager *fm = FileManager::getFileManager();
    char dst[192];
    if (int(strlen(dir) + strlen(name)) >= int(sizeof(dst))) {
        return -1;
    }
    strcpy(dst, dir);
    strcat(dst, name);
    File *f = NULL;
    if (fm->fopen(dst, FA_READ, &f) == FR_OK) {
        fm->fclose(f);
        return 0;
    }
    File *in = NULL, *out = NULL;
    if (fm->fopen(src, FA_READ, &in) != FR_OK) {
        return -1;
    }
    if (fm->fopen(dst, FA_WRITE | FA_CREATE_ALWAYS, &out) != FR_OK) {
        fm->fclose(in);
        return -1;
    }
    uint8_t *buf = (uint8_t *)malloc(CHUNK);
    bool ok = buf != NULL;
    uint32_t left = ok ? uint32_t(in->get_size()) : 0;
    while (ok && left) {
        uint32_t want = left > CHUNK ? uint32_t(CHUNK) : left, got = 0, put = 0;
        ok = in->read(buf, want, &got) == FR_OK && got == want &&
             out->write(buf, want, &put) == FR_OK && put == want;
        left -= want;
    }
    free(buf);
    fm->fclose(in);
    fm->fclose(out);
    return ok ? 1 : -1;
}

// ---- what the file says about the media ------------------------------------------
struct MediaNote {
    char full[192];
    char name[80];
    const char *role;
    uint8_t digest[32];
    bool have;
    int copied;
};

static void media_hash(MediaNote *m)
{
    m->have = false;
    File *f = NULL;
    if (FileManager::getFileManager()->fopen(m->full, FA_READ, &f) != FR_OK) {
        return;
    }
    TrxSha256 sh;
    trx_sha_init(&sh);
    uint8_t *buf = (uint8_t *)malloc(CHUNK);
    uint32_t left = buf ? uint32_t(f->get_size()) : 0;
    bool ok = buf != NULL;
    while (ok && left) {
        uint32_t want = left > CHUNK ? uint32_t(CHUNK) : left, got = 0;
        ok = f->read(buf, want, &got) == FR_OK && got == want;
        if (ok) {
            trx_sha_add(&sh, buf, want);
        }
        left -= want;
    }
    trx_sha_final(&sh, m->digest);
    free(buf);
    FileManager::getFileManager()->fclose(f);
    m->have = ok;
}

// The disk image on drive A and the loaded .crt, as files beside the .trxring
// (T20 §10.2). n is how many were found.
static int media_collect(MediaNote *m, int max)
{
    int n = 0;
    if (c1541_A && n < max) {
        mstring path, name;
        c1541_A->get_last_mounted_file(path, name);
        if (name.length() && int(path.length() + name.length() + 2) < int(sizeof(m[n].full))) {
            strcpy(m[n].full, path.c_str());
            if (!path.length() || path[path.length() - 1] != '/') {
                strcat(m[n].full, "/");
            }
            strcat(m[n].full, name.c_str());
            strncpy(m[n].name, name.c_str(), sizeof(m[n].name) - 1);
            m[n].name[sizeof(m[n].name) - 1] = 0;
            m[n].role = "disk";
            m[n].copied = -1;
            media_hash(&m[n]);
            n++;
        }
    }
    const char *crt = C64_CRT::get_source();
    if (crt && *crt && n < max && int(strlen(crt)) < int(sizeof(m[n].full))) {
        strcpy(m[n].full, crt);
        const char *slash = strrchr(crt, '/');
        strncpy(m[n].name, slash ? slash + 1 : crt, sizeof(m[n].name) - 1);
        m[n].name[sizeof(m[n].name) - 1] = 0;
        m[n].role = "cartridge";
        m[n].copied = -1;
        media_hash(&m[n]);
        n++;
    }
    return n;
}

// ---- the records of one stream ---------------------------------------------------
// The ring and alignment arithmetic is in trx_ring_recs.h, where the host bank
// tests it; here it only gets the engine's areas as its two callbacks.
static void area_rd(void *ctx, uint32_t off, uint8_t *p, uint32_t len)
{
    trx_ring_area_read(*(int *)ctx, off, p, len);
}

static void area_wr(void *ctx, uint32_t off, const uint8_t *p, uint32_t len)
{
    trx_ring_area_write(*(int *)ctx, off, p, len);
}

static TrxRecIo io_of(int *s)
{
    TrxRecIo io;
    io.size = trx_ring_area_size(*s);
    io.rec = trx_ring_rec_size(*s);
    io.chunk = CHUNK;
    io.ctx = s;
    io.rd = area_rd;
    io.wr = area_wr;
    return io;
}

// ---- save ------------------------------------------------------------------------
struct Parts {
    uint32_t timeline, ram, color, cart, s0, s1;
};

const char *trx_trxring_save(const char *path, uint32_t *bytes, trx_progress_fn progress, void *ctx)
{
    TrxRingStatus st;
    trx_ring_status(&st);
    if (!st.present) {
        return "This bitstream has no frame ring";
    }
    if (const char *reu = trx_ring_reu_refusal()) {
        return reu;
    }
    trx_ring_poll();
    int n_all = trx_ring_count();
    int first = -1;
    for (int i = 0; i < n_all; i++) {
        if (trx_ring_reachable(i)) {
            first = i;
            break;
        }
    }
    if (first < 0 || n_all - first < 1) {
        return "The ring holds no frame a restore could reach";
    }
    int n = n_all - first;
    TrxFrame *ents = (TrxFrame *)malloc(n * sizeof(TrxFrame));
    uint8_t *ram = (uint8_t *)malloc(65536);
    uint8_t *col = (uint8_t *)malloc(1024);
    Buf buf;
    buf_alloc(&buf, CHUNK + 32);
    uint8_t *cart = NULL;
    const char *err = NULL;
    if (!ents || !ram || !col || !buf.p) {
        err = "Out of memory";
    }
    for (int i = 0; !err && i < n; i++) {
        if (!trx_ring_frame(first + i, &ents[i])) {
            err = "The timeline moved while it was being read";
        }
    }
    if (!err && !ents[n - 1].block) {
        err = "The newest frame has no chip block";
    }
    if (!err && (ents[n - 1].w[W_GLUE] & GLUE_CART)) {
        cart = (uint8_t *)malloc(TRX_CART_RAM_BYTES);
        if (!cart) {
            err = "Out of memory";
        }
    }
    if (!err) {
        if (int e = trx_ring_materialize(n_all - 1, ram, col, cart)) {
            err = trx_ring_error(e);
        }
    }
    MediaNote media[2];
    int n_media = err ? 0 : media_collect(media, 2);
    if (err) {
        free(ents);
        free(ram);
        free(col);
        free(cart);
        buf_free(&buf);
        return err;
    }

    // the record ranges, and the rebase: the newest entry's counts become 0
    uint32_t base0 = ents[n - 1].e0, base1 = ents[n - 1].e1;
    uint32_t len0 = (base0 - ents[0].s0) & M28;
    uint32_t len1 = (base1 - ents[0].s1) & M28;
    uint32_t cap0 = trx_ring_area_size(0) / trx_ring_rec_size(0);
    uint32_t cap1 = trx_ring_area_size(1) / trx_ring_rec_size(1);
    if (len0 > cap0) {
        len0 = cap0;
    }
    if (len1 > cap1) {
        len1 = cap1;
    }
    uint32_t from0 = (base0 - len0) & M28, from1 = (base1 - len1) & M28;
    Parts parts;
    parts.timeline = uint32_t(n) * sizeof(TrxFrame);
    parts.ram = 65536;
    parts.color = 1024;
    parts.cart = cart ? TRX_CART_RAM_BYTES : 0;
    parts.s0 = len0 * trx_ring_rec_size(0);
    parts.s1 = len1 * trx_ring_rec_size(1);
    for (int i = 0; i < n; i++) {
        ents[i].s0 = (ents[i].s0 - base0) & M28;
        ents[i].e0 = (ents[i].e0 - base0) & M28;
        ents[i].s1 = (ents[i].s1 - base1) & M28;
        ents[i].e1 = (ents[i].e1 - base1) & M28;
    }

    // the header, with room for the two digests
    char *hdr = (char *)malloc(HDR_MAX);
    if (!hdr) {
        free(ents);
        free(ram);
        free(col);
        free(cart);
        buf_free(&buf);
        return "Out of memory";
    }
    int hl = sprintf(hdr, "{\"kind\":\"trx64-ultimate-ring\",\"version\":%d,\"bitstreamId\":%lu", VERSION,
                     (unsigned long)trx_io_read32(TRX_ID));
    hl += sprintf(hdr + hl, ",\"caps\":%u,\"caps2\":%u,\"blockWords\":%d,\"frameBytes\":%u",
                  (unsigned)ioRead8(TRX_CAPS), (unsigned)ioRead8(TRX_CAPS2), TRX_BLOCK_WORDS,
                  (unsigned)sizeof(TrxFrame));
    hl += sprintf(hdr + hl, ",\"frames\":%d,\"oldestSeq\":%lu,\"headSeq\":%lu,\"headCycles\":%llu", n,
                  (unsigned long)ents[0].seq, (unsigned long)ents[n - 1].seq,
                  (unsigned long long)ents[n - 1].cycles);
    hl += sprintf(hdr + hl, ",\"fpsX100\":%lu,\"frameCycles\":%lu", (unsigned long)trx_ring_fps_x100(),
                  (unsigned long)trx_ring_frame_cycles());
    hl += sprintf(hdr + hl, ",\"recBytes\":[%lu,%lu],\"s0First\":%lu,\"s1First\":%lu",
                  (unsigned long)trx_ring_rec_size(0), (unsigned long)trx_ring_rec_size(1),
                  (unsigned long)ents[0].s0, (unsigned long)ents[0].s1);
    hl += sprintf(hdr + hl,
                  ",\"lengths\":{\"timeline\":%lu,\"ram\":%lu,\"color\":%lu,\"cart\":%lu,\"s0\":%lu,\"s1\":%lu}",
                  (unsigned long)parts.timeline, (unsigned long)parts.ram, (unsigned long)parts.color,
                  (unsigned long)parts.cart, (unsigned long)parts.s0, (unsigned long)parts.s1);
    hl += sprintf(hdr + hl, ",\"marks\":[]");       // the app keeps no marks (T20 §4.5 is not built)
    hl += sprintf(hdr + hl, ",\"media\":[");
    char dir[160];
    dir_of(path, dir, sizeof(dir));
    for (int i = 0; i < n_media; i++) {
        media[i].copied = media[i].have ? copy_beside(dir, media[i].full, media[i].name) : -1;
        hl += sprintf(hdr + hl, "%s{\"role\":\"%s\",\"name\":\"", i ? "," : "", media[i].role);
        for (const char *q = media[i].name; *q; q++) {
            if (*q != '"' && *q != '\\' && uint8_t(*q) >= 0x20) {
                hdr[hl++] = *q;
            }
        }
        hl += sprintf(hdr + hl, "\",\"sha256\":\"");
        if (media[i].have) {
            hexout(hdr + hl, media[i].digest, 32);
            hl += 64;
        }
        hl += sprintf(hdr + hl, "\",\"beside\":%s}", media[i].copied == 1 ? "\"copied\"" :
                                                     media[i].copied == 0 ? "\"was there\"" : "\"missing\"");
    }
    hl += sprintf(hdr + hl, "],\"sha256State\":\"");
    int off_state = hl;
    memset(hdr + hl, '0', 64);
    hl += 64;
    hl += sprintf(hdr + hl, "\",\"sha256Log\":\"");
    int off_log = hl;
    memset(hdr + hl, '0', 64);
    hl += 64;
    hl += sprintf(hdr + hl, "\"}");

    FileManager *fm = FileManager::getFileManager();
    File *f = NULL;
    if (fm->fopen(path, FA_WRITE | FA_CREATE_ALWAYS, &f) != FR_OK) {
        err = "The file could not be created";
    }
    uint32_t total = 0;
    TrxSha256 sh_state, sh_log;
    trx_sha_init(&sh_state);
    trx_sha_init(&sh_log);
    uint8_t digest_state[32], digest_log[32];
    uint32_t tr = 0;
    if (!err) {
        uint8_t head[12];
        memcpy(head, "TRXRING1", 8);
        head[8] = uint8_t(hl);
        head[9] = uint8_t(hl >> 8);
        head[10] = uint8_t(hl >> 16);
        head[11] = uint8_t(hl >> 24);
        if (f->write(head, 12, &tr) != FR_OK || f->write(hdr, hl, &tr) != FR_OK) {
            err = "The file could not be written";
        }
        total = 12 + uint32_t(hl);
    }
    // the state parts
    struct {
        const uint8_t *p;
        uint32_t len;
    } state[4] = { { (const uint8_t *)ents, parts.timeline },
                   { ram, parts.ram },
                   { col, parts.color },
                   { cart, parts.cart } };
    for (int i = 0; !err && i < 4; i++) {
        if (!state[i].len) {
            continue;
        }
        trx_sha_add(&sh_state, state[i].p, state[i].len);
        if (f->write(state[i].p, state[i].len, &tr) != FR_OK || tr != state[i].len) {
            err = "The file could not be written";
        }
        total += state[i].len;
    }
    trx_sha_final(&sh_state, digest_state);
    // the records
    int steps = int((parts.s0 + parts.s1) / CHUNK) + 2;
    int step = 0;
    for (int s = 0; !err && s < 2; s++) {
        uint32_t left = (s ? len1 : len0), from = (s ? from1 : from0);
        while (!err && left) {
            uint32_t at = 0;
            int sn = s;
            TrxRecIo io = io_of(&sn);
            uint32_t got = trx_recs_in(&io, from, left, buf.p, &at);
            if (!got) {
                err = "The log could not be read";
                break;
            }
            trx_sha_add(&sh_log, buf.p + at, got);
            if (f->write(buf.p + at, got, &tr) != FR_OK || tr != got) {
                err = "The file could not be written";
                break;
            }
            total += got;
            uint32_t recs = got / trx_ring_rec_size(s);
            from = (from + recs) & M28;
            left -= recs;
            if (progress && (++step & 7) == 0) {
                progress(step, steps, ctx);
            }
        }
    }
    trx_sha_final(&sh_log, digest_log);
    if (!err) {
        // the two digests into the header that is already on disk
        char hex[64];
        hexout(hex, digest_state, 32);
        if (f->seek(12 + off_state) != FR_OK || f->write(hex, 64, &tr) != FR_OK) {
            err = "The file could not be written";
        }
        hexout(hex, digest_log, 32);
        if (!err && (f->seek(12 + off_log) != FR_OK || f->write(hex, 64, &tr) != FR_OK)) {
            err = "The file could not be written";
        }
    }
    if (f) {
        fm->fclose(f);
    }
    if (!err) {
        *bytes = total;
        printf("[RING] .trxring: %d frames, %lu + %lu record bytes, %lu bytes in all\n", n,
               (unsigned long)parts.s0, (unsigned long)parts.s1, (unsigned long)total);
    }
    free(hdr);
    free(ents);
    free(ram);
    free(col);
    free(cart);
    buf_free(&buf);
    return err;
}

// ---- load ------------------------------------------------------------------------
bool trx_trxring_is_one(const char *path)
{
    File *f = NULL;
    if (FileManager::getFileManager()->fopen(path, FA_READ, &f) != FR_OK) {
        return false;
    }
    char magic[8];
    uint32_t got = 0;
    bool ok = f->read(magic, 8, &got) == FR_OK && got == 8 && !memcmp(magic, "TRXRING1", 8);
    FileManager::getFileManager()->fclose(f);
    return ok;
}

// What a load needs between opening the file and the install's callback.
struct LoadJob {
    File *f;
    char *hdr;
    jsmntok_t *tok;
    int ntok;
    uint8_t *ram, *col, *cart;
    Buf buf, stage;
    TrxFrame *ents;
    int frames;
    uint32_t s0_bytes, s1_bytes, s0_first, s1_first;
    uint8_t digest_log[32];
    const char *log_hex;
};

static int64_t hget(const LoadJob *j, const char *a, const char *b, int64_t dflt)
{
    int o = b ? json_key(j->hdr, j->tok, j->ntok, json_key(j->hdr, j->tok, j->ntok, 0, a), b)
              : json_key(j->hdr, j->tok, j->ntok, 0, a);
    return tok_int_or(j->hdr, j->tok, o, dflt);
}

// The install's callback: the records out of the file and into the areas, at
// the offsets the rebased counts give (the newest entry's are 0, so the oldest
// record sits len * size below it, wrapped).
static int areas_back(void *ctx)
{
    LoadJob *j = (LoadJob *)ctx;
    TrxSha256 sh;
    trx_sha_init(&sh);
    for (int s = 0; s < 2; s++) {
        uint32_t left = s ? j->s1_bytes : j->s0_bytes;
        uint32_t rec = trx_ring_rec_size(s);
        uint32_t cnt = s ? j->s1_first : j->s0_first;
        while (left) {
            uint32_t want = left > CHUNK ? uint32_t(CHUNK) : left;
            want = (want / rec) * rec;
            uint32_t got = 0;
            if (j->f->read(j->buf.p, want, &got) != FR_OK || got != want) {
                return -1;
            }
            trx_sha_add(&sh, j->buf.p, want);
            int sn = s;
            TrxRecIo io = io_of(&sn);
            trx_recs_out(&io, cnt, j->buf.p, want, j->stage.p);
            cnt = (cnt + want / rec) & M28;
            left -= want;
        }
    }
    trx_sha_final(&sh, j->digest_log);
    return 0;
}

const char *trx_trxring_load(const char *path)
{
    FileManager *fm = FileManager::getFileManager();
    LoadJob j;
    memset(&j, 0, sizeof(j));
    if (fm->fopen(path, FA_READ, &j.f) != FR_OK) {
        return "The file could not be opened";
    }
    const char *err = NULL;
    uint8_t head[12];
    uint32_t got = 0, hl = 0;
    if (j.f->read(head, 12, &got) != FR_OK || got != 12 || memcmp(head, "TRXRING1", 8)) {
        err = "Not a .trxring";
    } else {
        hl = uint32_t(head[8]) | (uint32_t(head[9]) << 8) | (uint32_t(head[10]) << 16) |
             (uint32_t(head[11]) << 24);
        if (hl < 2 || hl > HDR_MAX) {
            err = "The header's length is not right";
        }
    }
    if (!err) {
        j.hdr = (char *)malloc(hl + 1);
        if (!j.hdr || j.f->read(j.hdr, hl, &got) != FR_OK || got != hl) {
            err = "The header could not be read";
        } else {
            j.hdr[hl] = 0;
            jsmn_parser p;
            jsmn_init(&p);
            int need = jsmn_parse(&p, j.hdr, hl, NULL, 0);
            if (need < 1) {
                err = "The header is not JSON";
            } else {
                j.tok = (jsmntok_t *)malloc(sizeof(jsmntok_t) * need);
                jsmn_init(&p);
                j.ntok = j.tok ? jsmn_parse(&p, j.hdr, hl, j.tok, need) : -1;
                if (j.ntok < 1) {
                    err = "The header is not JSON";
                }
            }
        }
    }
    uint32_t tl_bytes = 0, ram_bytes = 0, col_bytes = 0, cart_bytes = 0;
    if (!err) {
        if (hget(&j, "version", NULL, -1) != VERSION) {
            err = "A .trxring of another version";
        } else if (hget(&j, "blockWords", NULL, -1) != TRX_BLOCK_WORDS ||
                   hget(&j, "frameBytes", NULL, -1) != int64_t(sizeof(TrxFrame))) {
            err = "A .trxring of another build (the timeline's shape differs)";
        } else if (uint32_t(hget(&j, "bitstreamId", NULL, 0)) != trx_io_read32(TRX_ID)) {
            err = "A .trxring of another bitstream";
        } else {
            int rb = json_key(j.hdr, j.tok, j.ntok, 0, "recBytes");
            uint8_t want[2] = { 0, 0 };
            if (rb < 0 || tok_bytes(j.hdr, j.tok, j.ntok, rb, want, 2) != 2 ||
                want[0] != trx_ring_rec_size(0) || want[1] != trx_ring_rec_size(1)) {
                err = "A .trxring whose records are not this engine's size";
            }
        }
    }
    if (!err) {
        j.frames = int(hget(&j, "frames", NULL, 0));
        tl_bytes = uint32_t(hget(&j, "lengths", "timeline", 0));
        ram_bytes = uint32_t(hget(&j, "lengths", "ram", 0));
        col_bytes = uint32_t(hget(&j, "lengths", "color", 0));
        cart_bytes = uint32_t(hget(&j, "lengths", "cart", 0));
        j.s0_bytes = uint32_t(hget(&j, "lengths", "s0", 0));
        j.s1_bytes = uint32_t(hget(&j, "lengths", "s1", 0));
        j.s0_first = uint32_t(hget(&j, "s0First", NULL, 0));
        j.s1_first = uint32_t(hget(&j, "s1First", NULL, 0));
        if (j.frames < 1 || tl_bytes != uint32_t(j.frames) * sizeof(TrxFrame) || ram_bytes != 65536 ||
            col_bytes != 1024 || (cart_bytes && cart_bytes != TRX_CART_RAM_BYTES)) {
            err = "The header's lengths do not fit";
        }
    }
    if (!err) {
        j.ents = (TrxFrame *)malloc(tl_bytes);
        j.ram = (uint8_t *)malloc(ram_bytes);
        j.col = (uint8_t *)malloc(col_bytes);
        j.cart = cart_bytes ? (uint8_t *)malloc(cart_bytes) : NULL;
        buf_alloc(&j.buf, CHUNK + 32);
        buf_alloc(&j.stage, CHUNK + 32);
        if (!j.ents || !j.ram || !j.col || !j.buf.p || !j.stage.p || (cart_bytes && !j.cart)) {
            err = "Out of memory";
        }
    }
    if (!err) {
        TrxSha256 sh;
        trx_sha_init(&sh);
        struct {
            uint8_t *p;
            uint32_t len;
        } state[4] = { { (uint8_t *)j.ents, tl_bytes }, { j.ram, ram_bytes }, { j.col, col_bytes },
                       { j.cart, cart_bytes } };
        for (int i = 0; !err && i < 4; i++) {
            if (!state[i].len) {
                continue;
            }
            if (j.f->read(state[i].p, state[i].len, &got) != FR_OK || got != state[i].len) {
                err = "The file ends inside the state";
            } else {
                trx_sha_add(&sh, state[i].p, state[i].len);
            }
        }
        uint8_t d[32];
        trx_sha_final(&sh, d);
        int sd = json_key(j.hdr, j.tok, j.ntok, 0, "sha256State");
        if (!err && (sd < 0 || !hexeq(j.hdr + j.tok[sd].start, d, 32))) {
            err = "The ring's state does not match its hash";
        }
    }
    int e = 0;
    if (!err) {
        int ld = json_key(j.hdr, j.tok, j.ntok, 0, "sha256Log");
        j.log_hex = ld >= 0 ? j.hdr + j.tok[ld].start : NULL;
        TrxLoadMem mem = { j.ram, j.col, j.cart, NULL, 0 };
        e = trx_ring_install(j.ents, j.frames, &mem, areas_back, &j);
        if (e == -1) {
            err = "The file ends inside the log";
        } else if (e) {
            err = trx_ring_error(e);
        }
    }
    if (!err && j.log_hex && !hexeq(j.log_hex, j.digest_log, 32)) {
        printf("[RING] the .trxring's log does not match its hash: the machine is where the file says, "
               "but a rewind may not be\n");
    }
    if (!err) {
        printf("[RING] .trxring loaded: %d frames, %lu + %lu record bytes\n", j.frames,
               (unsigned long)j.s0_bytes, (unsigned long)j.s1_bytes);
    }
    fm->fclose(j.f);
    free(j.hdr);
    free(j.tok);
    free(j.ents);
    free(j.ram);
    free(j.col);
    free(j.cart);
    buf_free(&j.buf);
    buf_free(&j.stage);
    return err;
}

const char *trx_trxring_check(const char *path)
{
    FileManager *fm = FileManager::getFileManager();
    File *f = NULL;
    if (fm->fopen(path, FA_READ, &f) != FR_OK) {
        return "The file could not be opened";
    }
    uint8_t head[12];
    uint32_t got = 0;
    const char *err = NULL;
    uint32_t hl = 0;
    if (f->read(head, 12, &got) != FR_OK || got != 12 || memcmp(head, "TRXRING1", 8)) {
        err = "Not a .trxring";
    } else {
        hl = uint32_t(head[8]) | (uint32_t(head[9]) << 8) | (uint32_t(head[10]) << 16) |
             (uint32_t(head[11]) << 24);
        if (hl < 2 || hl > HDR_MAX) {
            err = "The header's length is not right";
        }
    }
    char *hdr = NULL;
    jsmntok_t *tok = NULL;
    int ntok = 0;
    if (!err) {
        hdr = (char *)malloc(hl + 1);
        if (!hdr || f->read(hdr, hl, &got) != FR_OK || got != hl) {
            err = "The header could not be read";
        } else {
            hdr[hl] = 0;
            jsmn_parser p;
            jsmn_init(&p);
            int need = jsmn_parse(&p, hdr, hl, NULL, 0);
            tok = need > 0 ? (jsmntok_t *)malloc(sizeof(jsmntok_t) * need) : NULL;
            if (tok) {
                jsmn_init(&p);
                ntok = jsmn_parse(&p, hdr, hl, tok, need);
            }
            if (ntok < 1) {
                err = "The header is not JSON";
            }
        }
    }
    if (!err) {
        LoadJob j;
        memset(&j, 0, sizeof(j));
        j.hdr = hdr;
        j.tok = tok;
        j.ntok = ntok;
        if (hget(&j, "version", NULL, -1) != VERSION) {
            err = "A .trxring of another version";
        } else if (hget(&j, "blockWords", NULL, -1) != TRX_BLOCK_WORDS ||
                   hget(&j, "frameBytes", NULL, -1) != int64_t(sizeof(TrxFrame))) {
            err = "A .trxring of another build (the timeline's shape differs)";
        } else if (uint32_t(hget(&j, "bitstreamId", NULL, 0)) != trx_io_read32(TRX_ID)) {
            err = "A .trxring of another bitstream";
        } else if (hget(&j, "frames", NULL, 0) < 1) {
            err = "The .trxring holds no frame";
        }
    }
    if (!err) {
        err = trx_ring_reu_refusal();
    }
    free(hdr);
    free(tok);
    fm->fclose(f);
    return err;
}

// ---- the deferred load from the file browser -------------------------------------
static char *pending_path;

void trx_trxring_set_pending(const char *path)
{
    free(pending_path);
    pending_path = NULL;
    if (!path) {
        return;
    }
    pending_path = (char *)malloc(strlen(path) + 1);
    if (pending_path) {
        strcpy(pending_path, path);
    }
}

bool trx_trxring_pending(void)
{
    return pending_path != NULL;
}

void trx_trxring_unfreeze(void)
{
    char *p = pending_path;
    pending_path = NULL;
    if (!p) {
        return;
    }
    const char *err = trx_trxring_load(p);
    if (err) {
        // the menu is gone by now: the console is all there is
        printf("[RING] loading %s failed: %s\n", p, err);
    }
    free(p);
}

#endif
