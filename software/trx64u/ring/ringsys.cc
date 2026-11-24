// The ring library's helpers over the base (ringsys.h).
#include "ringsys.h"
#include "libfmt.h"

void *r_calloc(uint32_t n, uint32_t size)
{
    if (size && n > 0xFFFFFFFFu / size) {
        return 0;
    }
    void *p = r_malloc(n * size);
    if (p) {
        memset(p, 0, n * size);
    }
    return p;
}

// ---- text ------------------------------------------------------------------------------------------------------------------------------------------------
struct Out {
    char *dst;
    int cap, n;
};

static void out_c(Out *o, char c)
{
    if (o->n < o->cap - 1) {
        o->dst[o->n] = c;
    }
    o->n++;
}

static void out_pad(Out *o, char c, int k)
{
    while (k-- > 0) {
        out_c(o, c);
    }
}

int r_vsnprintf(char *dst, int cap, const char *fmt, va_list ap)
{
    Out o = { dst, cap, 0 };
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') {
            out_c(&o, *p);
            continue;
        }
        p++;
        bool left = false, zero = false;
        for (;; p++) {
            if (*p == '-') {
                left = true;
            } else if (*p == '0') {
                zero = true;
            } else {
                break;
            }
        }
        int width = 0, prec = -1;
        while (*p >= '0' && *p <= '9') {
            width = width * 10 + (*p++ - '0');
        }
        if (*p == '.') {
            p++;
            prec = 0;
            if (*p == '*') {
                prec = va_arg(ap, int);
                p++;
            }
            while (*p >= '0' && *p <= '9') {
                prec = prec * 10 + (*p++ - '0');
            }
        }
        int lng = 0;
        while (*p == 'l') {
            lng++;
            p++;
        }
        char tmp[24];
        int tn = 0;
        bool neg = false;
        const char *s = tmp;
        int sl = 0;
        switch (*p) {
        case 'd':
        case 'i':
        case 'u':
        case 'x':
        case 'X': {
            uint64_t u;
            if (*p == 'd' || *p == 'i') {
                int64_t v = lng >= 2 ? va_arg(ap, long long) : lng ? va_arg(ap, long) : va_arg(ap, int);
                neg = v < 0;
                u = neg ? uint64_t(-v) : uint64_t(v);
            } else {
                u = lng >= 2 ? va_arg(ap, unsigned long long) : lng ? va_arg(ap, unsigned long) : va_arg(ap, unsigned);
            }
            const char *digits = *p == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
            unsigned base = (*p == 'x' || *p == 'X') ? 16 : 10;
            do {
                tmp[tn++] = digits[base == 16 ? unsigned(u & 15) : unsigned(u % 10)];
                u = base == 16 ? u >> 4 : u / 10;
            } while (u);
            int len = tn + (neg ? 1 : 0);
            if (!left && !zero) {
                out_pad(&o, ' ', width - len);
            }
            if (neg) {
                out_c(&o, '-');
            }
            if (!left && zero) {
                out_pad(&o, '0', width - len);
            }
            while (tn) {
                out_c(&o, tmp[--tn]);
            }
            if (left) {
                out_pad(&o, ' ', width - len);
            }
            continue;
        }
        case 'c':
            tmp[0] = char(va_arg(ap, int));
            sl = 1;
            break;
        case 's':
            s = va_arg(ap, const char *);
            if (!s) {
                s = "(null)";
            }
            while (s[sl] && (prec < 0 || sl < prec)) {
                sl++;
            }
            break;
        case '%':
            tmp[0] = '%';
            sl = 1;
            break;
        default:                                    // not one of ours: written as it stands
            out_c(&o, '%');
            if (!*p) {
                p--;
            } else {
                out_c(&o, *p);
            }
            continue;
        }
        if (!left) {
            out_pad(&o, ' ', width - sl);
        }
        for (int i = 0; i < sl; i++) {
            out_c(&o, s[i]);
        }
        if (left) {
            out_pad(&o, ' ', width - sl);
        }
    }
    if (cap > 0) {
        dst[o.n < cap ? o.n : cap - 1] = 0;
    }
    return o.n;
}

int r_snprintf(char *dst, int cap, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = r_vsnprintf(dst, cap, fmt, ap);
    va_end(ap);
    return n;
}

void r_log(const char *fmt, ...)
{
    char line[200];
    va_list ap;
    va_start(ap, fmt);
    r_vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    size_t n = strlen(line);
    if (n && line[n - 1] == '\n') {
        line[n - 1] = 0;
    }
    g_api->log(line);
}

static int lower(int c)
{
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

int r_strcasecmp(const char *a, const char *b)
{
    while (*a && lower((unsigned char)*a) == lower((unsigned char)*b)) {
        a++;
        b++;
    }
    return lower((unsigned char)*a) - lower((unsigned char)*b);
}

const char *r_strstr(const char *hay, const char *needle)
{
    size_t n = strlen(needle);
    for (; *hay; hay++) {
        if (!strncmp(hay, needle, n)) {
            return hay;
        }
    }
    return n ? 0 : hay;
}

int64_t r_strtoll(const char *s)
{
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    bool neg = *s == '-';
    if (*s == '-' || *s == '+') {
        s++;
    }
    uint64_t v = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + uint64_t(*s++ - '0');
    }
    return neg ? -int64_t(v) : int64_t(v);
}

int r_atoi(const char *s)
{
    return int(r_strtoll(s));
}

void r_copy(char *out, int outlen, const char *s, size_t n)
{
    if (outlen <= 0) {
        return;
    }
    size_t l = 0;
    while (l < n && s[l]) {
        l++;
    }
    if (l > size_t(outlen) - 1) {
        l = size_t(outlen) - 1;
    }
    memcpy(out, s, l);
    out[l] = 0;
}

// ---- the registers ---------------------------------------------------------------------------------------------------------------------------------
uint8_t trx_io_caps(void)
{
    return HW_RD(ident_caps);
}

bool trx_io_slim(void)
{
    static int slim = -1;                           // not looked yet
    if (slim < 0 && g_api->io) {
        slim = (trx_io_caps() & DRV_ident_caps_video_MASK) &&
               ((HW_RD(trx_text_ctrl) & DRV_trx_text_ctrl_version_MASK) >> DRV_trx_text_ctrl_version_SHIFT) >= 2;
    }
    return slim > 0;
}

// ---- the C64 machine's registers -------------------------------------------------------------------------------------------------------------------------
uint8_t io_cart_rd(uint32_t off)
{
    const SvcIo *io = g_api->io;
    return io ? io->cart_rd(off) : 0;
}

void io_cart_wr(uint32_t off, uint8_t v)
{
    if (const SvcIo *io = g_api->io) {
        io->cart_wr(off, v);
    }
}

void io_stop(bool on)
{
    if (const SvcIo *io = g_api->io) {
        io->stop(on);
    }
}

void io_memonly(bool on)
{
    if (const SvcIo *io = g_api->io) {
        io->ram_memonly(on);
    }
}

// ---- time ------------------------------------------------------------------------------------------------------------------------------------------------
uint16_t r_ms_timer(void)
{
    const SvcSys *s = g_api->sys;
    return SVC_HAS(SvcSys, s, ms_timer) ? s->ms_timer() : uint16_t(s->ms());
}

// ---- the exports of the shared libraries -----------------------------------------------------------------------------------------------------------------
static const char *const rx_name[RX_COUNT] = { "c64cart", "cbm1541", "uci" };
static const void *rx_table[RX_COUNT];
static uint32_t rx_size[RX_COUNT];

const void *r_export(RExportId id, uint32_t off, uint32_t len)
{
    if (unsigned(id) >= unsigned(RX_COUNT)) {
        return 0;
    }
    if (!rx_table[id] && g_api->lib) {
        uint32_t size = 0;
        const void *t = g_api->lib->find(rx_name[id], U2A3_VERSION(1, 0), &size);
        if (t && size >= sizeof(uint32_t)) {
            rx_size[id] = size;
            rx_table[id] = t;
        }
    }
    const uint8_t *t = (const uint8_t *)rx_table[id];
    if (!t || rx_size[id] < off + len) {
        return 0;
    }
    uintptr_t member = 0;                           // a function pointer in every table this library uses
    memcpy(&member, t + off, len < sizeof(member) ? len : sizeof(member));
    return member ? t : 0;
}

// ---- files -----------------------------------------------------------------------------------------------------------------------------------------------
bool rf_open(RFile *f, const char *path)
{
    f->pos = 0;
    f->h = g_api->fs ? g_api->fs->open(path, false) : 0;
    return f->h != 0;
}

// SvcFs::open does not make a file: save() makes it empty (or replaces it), then it is opened for writing.
bool rf_create(RFile *f, const char *path)
{
    f->pos = 0;
    f->h = 0;
    if (!g_api->fs || !g_api->fs->save(path, "", 0)) {
        return false;
    }
    f->h = g_api->fs->open(path, true);
    if (f->h && !g_api->fs->writable(f->h)) {
        g_api->fs->close(f->h);
        f->h = 0;
    }
    return f->h != 0;
}

bool rf_read(RFile *f, void *buf, uint32_t len)
{
    if (!len) {
        return true;
    }
    if (!g_api->fs->read(f->h, f->pos, buf, len)) {
        return false;
    }
    f->pos += len;
    return true;
}

bool rf_write(RFile *f, const void *buf, uint32_t len)
{
    if (!len) {
        return true;
    }
    if (!g_api->fs->write(f->h, f->pos, buf, len)) {
        return false;
    }
    f->pos += len;
    return true;
}

uint64_t rf_size(RFile *f)
{
    return g_api->fs->fsize(f->h);
}

void rf_close(RFile *f)
{
    if (f->h) {
        g_api->fs->close(f->h);
        f->h = 0;
    }
}

bool rf_remove(const char *path)
{
    return g_api->fs && g_api->fs->remove(path);
}

bool rf_exists(const char *path)
{
    uint64_t size;
    bool dir;
    return g_api->fs && g_api->fs->stat(path, &size, &dir) && !dir;
}
