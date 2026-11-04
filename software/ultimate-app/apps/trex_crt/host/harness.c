/*
 * harness.c - host tests of trex_crt (T57 spec 8.1). The app's real C files run against mock.c and the cart
 * models of carts.c; app_main is called the way the loader calls it.
 *
 *   trex_host_test             run everything, exit 0 = all PASS
 *   trex_host_test --golden    print the Gideon traffic digests (writes golden/stock.txt when redirected)
 *   trex_host_test --trace DIR write every scenario's access trace to DIR
 */
#define _GNU_SOURCE
#include "mock.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

extern int app_main(const u2p_api_t *api, const u2p_app_context_t *ctx);

#define KF1 0x85
#define KF3 0x86
#define KF5 0x87
#define KF7 0x88
#define KUP 0x91
#define KDOWN 0x12
#define KLEFT 0x9D
#define KRIGHT 0x1D

static int fails, checks;
static char root[256];
static const char *trace_dir;

#define CHECK(cond, ...) do { checks++; if (!(cond)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

// ---- helpers ---------------------------------------------------------------------------------------------
static void remove_file(const char *rel)
{
    char p[512];
    snprintf(p, sizeof(p), "%s%s", root, rel);
    remove(p);
}
static void mkpath(const char *rel)
{
    char p[512];
    snprintf(p, sizeof(p), "%s%s", root, rel);
    mkdir(p, 0755);
}

static uint64_t fnv(const char *s, size_t n, uint64_t h)
{
    for (size_t i = 0; i < n; i++) {
        h ^= (uint8_t)s[i];
        h *= 1099511628211ull;
    }
    return h;
}

typedef struct { char *buf; size_t len; } trace_t;
static trace_t s_tr;
static void trace_begin(void)
{
    free(s_tr.buf);
    s_tr.buf = NULL;
    s_tr.len = 0;
    mock_trace = open_memstream(&s_tr.buf, &s_tr.len);
}
static void trace_end(const char *name, uint64_t *hash, long *lines)
{
    fflush(mock_trace);
    fclose(mock_trace);
    mock_trace = NULL;
    *hash = fnv(s_tr.buf, s_tr.len, 1469598103934665603ull);
    long n = 0;
    for (size_t i = 0; i < s_tr.len; i++)
        n += s_tr.buf[i] == '\n';
    *lines = n;
    if (trace_dir) {
        char p[512];
        snprintf(p, sizeof(p), "%s/%s.trace", trace_dir, name);
        FILE *f = fopen(p, "w");
        if (f) {
            fwrite(s_tr.buf, 1, s_tr.len, f);
            fclose(f);
        }
    }
}

// ---- CRT building ------------------------------------------------------------------------------------------
typedef struct { uint8_t *d; size_t n; } crtbuf_t;

static void put16(uint8_t *p, unsigned v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put32(uint8_t *p, uint32_t v) { put16(p, v >> 16); put16(p + 2, v & 0xFFFF); }

static void crt_head(crtbuf_t *b, int type, int exrom, int game, const char *name)
{
    b->d = calloc(1, 9 * 1024 * 1024);
    b->n = 0x40;
    memcpy(b->d, "C64 CARTRIDGE   ", 16);
    put32(b->d + 0x10, 0x40);
    put16(b->d + 0x14, 0x0100);
    put16(b->d + 0x16, (unsigned)type);
    b->d[0x18] = (uint8_t)exrom;
    b->d[0x19] = (uint8_t)game;
    strncpy((char *)b->d + 0x20, name, 32);
}
static void crt_chip(crtbuf_t *b, int bank, unsigned load, const uint8_t *data, unsigned size)
{
    uint8_t *h = b->d + b->n;
    memcpy(h, "CHIP", 4);
    put32(h + 4, 0x10 + size);
    put16(h + 8, 0);
    put16(h + 10, (unsigned)bank);
    put16(h + 12, load);
    put16(h + 14, size);
    memcpy(h + 16, data, size);
    b->n += 16 + size;
}
static void crt_save(crtbuf_t *b, const char *rel)
{
    char p[512];
    snprintf(p, sizeof(p), "%s%s", root, rel);
    FILE *f = fopen(p, "wb");
    fwrite(b->d, 1, b->n, f);
    fclose(f);
    free(b->d);
}

// A mostly erased 8K chunk with a little data (a write then programs few bytes, a verify still compares 8K)
static void sparse(uint8_t *d, uint32_t seed, int first_ff)
{
    memset(d, 0xFF, 0x2000);
    uint32_t x = seed * 2654435761u + 1;
    for (int i = 0; i < 48; i++) {
        x = x * 1664525u + 1013904223u;
        d[(x >> 8) % 0x2000] = (uint8_t)(x >> 24);
    }
    if (first_ff)
        d[0] = (uint8_t)(0x40 + (seed & 0x3F));
}

static void make_write_crt(const char *cart, const char *rel)
{
    crtbuf_t b;
    uint8_t d[0x2000];
    if (!strcmp(cart, "ef1") || !strcmp(cart, "ef3")) {
        static const int banks[] = { 0, 5, 63 };
        crt_head(&b, 32, 1, 0, "TEST EF");
        for (int i = 0; i < 3; i++) {
            sparse(d, 100u + (unsigned)banks[i], 1);
            if (banks[i] == 0) {
                memcpy(d + 0x1800, "eapi", 4); // the EAPI of the CRT is replaced (replace_eapi)
            }
            crt_chip(&b, banks[i], 0x8000, d, 0x2000);
            sparse(d, 200u + (unsigned)banks[i], 1);
            if (banks[i] == 0)
                memcpy(d + 0x1800, "eapi", 4);
            crt_chip(&b, banks[i], 0xE000, d, 0x2000);
        }
    } else if (!strcmp(cart, "megabyter") || !strcmp(cart, "megacart") || !strcmp(cart, "gmod2")) {
        int type = !strcmp(cart, "megabyter") ? 86 : !strcmp(cart, "megacart") ? 88 : 60;
        static const int mb[] = { 0, 1, 127 }, mc[] = { 0, 200, 255 }, gm[] = { 0, 3, 63 };
        const int *banks = type == 86 ? mb : type == 88 ? mc : gm;
        crt_head(&b, type, 0, 1, "TEST");
        for (int i = 0; i < 3; i++) {
            sparse(d, 300u + (unsigned)banks[i], 1);
            crt_chip(&b, banks[i], 0x8000, d, 0x2000);
        }
        if (type == 60) {
            uint8_t ee[2048];
            for (int i = 0; i < 2048; i++)
                ee[i] = (uint8_t)(i * 3 + 7);
            crt_chip(&b, 0, 0xDE00, ee, 2048);
        }
    } else {
        crt_head(&b, 0, 0, 0, "ROM");
        sparse(d, 1, 1);
        crt_chip(&b, 0, 0x8000, d, 0x2000);
        crt_chip(&b, 0, 0xA000, d, 0x2000);
    }
    crt_save(&b, rel);
}

// ---- running the app ---------------------------------------------------------------------------------------
typedef struct {
    const char *cart;
    mock_cfg_t cfg;
    const char *action;
    const char *file;
    int keys[16];
    int nkeys;
    const char *uiname;
    int menu_choice;
    int popup;             // answer of YES/NO popups: BUTTON_YES or BUTTON_NO, 0 = YES
    void (*pre)(cart_t *); // after the machine is set up: what a game left in the cartridge
} scen_t;

static cart_t *make_cart(const char *name)
{
    if (!name || !strcmp(name, "none")) return NULL;
    cart_t *c = !strcmp(name, "rom16") ? cart_rom16() : !strcmp(name, "ef1") ? cart_ef1() :
                !strcmp(name, "ef3") ? cart_ef3() : !strcmp(name, "megabyter") ? cart_megabyter() :
                !strcmp(name, "megacart") ? cart_megacart() : cart_gmod2();
    cart_fill_pattern(c, 7);
    return c;
}

static mock_cfg_t gideon_cfg(const char *tool_is_dump)
{
    mock_cfg_t c;
    memset(&c, 0, sizeof(c));
    c.trx = false;
    c.pref = 1;
    c.has_media_reset = true;
    c.prog_pdir = 0x2F;
    c.prog_pout = 0x35;
    c.app_ultimax = tool_is_dump != NULL;
    c.max_accesses = 60000000;
    return c;
}

static mock_cfg_t trx_cfg(void)
{
    mock_cfg_t c = gideon_cfg(NULL);
    c.trx = true;
    c.has_slot = true;
    c.armed = true;
    c.ext_mask = 15;
    c.int_mask = 0;
    c.pref = 0;
    c.app_ultimax = true;
    return c;
}

static int run_app(cart_t *cart, scen_t *s)
{
    mock_init(cart, &s->cfg);
    if (s->pre)
        s->pre(cart);
    mock_ui.menu_choice = s->menu_choice;
    mock_ui.popup_yes = s->popup;
    if (s->uiname)
        snprintf(mock_ui.name, sizeof(mock_ui.name), "%s", s->uiname);
    mock_ui.nkeys = s->nkeys;
    for (int i = 0; i < s->nkeys; i++)
        mock_ui.keys[i] = s->keys[i];
    u2p_app_context_t ctx;
    mock_ctx(&ctx);
    ctx.action = s->action;
    ctx.selected_file = s->file;
    return app_main(mock_api(), &ctx);
}


// ---- content checks -----------------------------------------------------------------------------------------
static uint8_t *slurp(const char *rel, size_t *n)
{
    char p[512];
    snprintf(p, sizeof(p), "%s%s", root, rel);
    FILE *f = fopen(p, "rb");
    if (!f) { *n = 0; return NULL; }
    uint8_t *d = malloc(9 * 1024 * 1024);
    *n = fread(d, 1, 9 * 1024 * 1024, f);
    fclose(f);
    return d;
}

static unsigned be16(const uint8_t *p) { return (unsigned)(p[0] << 8 | p[1]); }
static uint32_t be32(const uint8_t *p) { return (uint32_t)be16(p) << 16 | be16(p + 2); }

// A dump equals the cart's content: every CHIP packet byte for byte, the type as the model says
static int check_dump(const char *what, cart_t *c, const char *rel, int slot)
{
    size_t n;
    uint8_t *d = slurp(rel, &n);
    if (!d || n < 0x40) { CHECK(0, "%s: no dump file %s", what, rel); free(d); return 1; }
    int bad = 0, chips = 0;
    c->view_slot = slot;
    CHECK((int)be16(d + 0x16) == c->crt_type, "%s: CRT type %d, want %d", what, (int)be16(d + 0x16), c->crt_type);
    size_t pos = be32(d + 0x10);
    int seen_lo[256] = { 0 }, seen_hi[256] = { 0 };
    while (pos + 16 <= n && !memcmp(d + pos, "CHIP", 4)) {
        uint32_t len = be32(d + pos + 4);
        unsigned bank = be16(d + pos + 10), load = be16(d + pos + 12), size = be16(d + pos + 14);
        const uint8_t *data = d + pos + 16;
        if (load == 0xDE00) {
            for (unsigned i = 0; i < size; i++)
                if (!c->eeprom || c->eeprom[i] != data[i]) bad++;
        } else {
            int half = load == 0x8000 ? 0 : 1;
            for (unsigned i = 0; i < size; i++)
                if (c->bank_byte(c, (int)bank, half, (int)i) != data[i]) bad++;
            if (bank < 256) (half ? seen_hi : seen_lo)[bank] = 1;
            chips++;
        }
        pos += len;
    }
    int want_hi = strcmp(c->name, "megabyter") && strcmp(c->name, "gmod2") && strcmp(c->name, "megacart");
    int missing = 0;
    for (int b = 0; b < c->banks; b++) {
        if (!seen_lo[b]) missing++;
        if (want_hi && !seen_hi[b]) missing++;
    }
    CHECK(bad == 0, "%s: %d dumped bytes differ from the cartridge", what, bad);
    CHECK(missing == 0, "%s: %d chip packets missing (%d found)", what, missing, chips);
    free(d);
    return bad + missing;
}

// After a write the flash holds the CRT (banks the CRT lacks are erased; an EasyFlash's EAPI is replaced)
static int check_written(const char *what, cart_t *c, const char *crtrel, int slot)
{
    size_t n;
    uint8_t *d = slurp(crtrel, &n);
    if (!d) { CHECK(0, "%s: no CRT", what); return 1; }
    c->view_slot = slot;
    int ef = !strcmp(c->name, "ef1") || !strcmp(c->name, "ef3");
    int single = !ef;
    static uint8_t exp_lo[256][0x2000], exp_hi[256][0x2000];
    static int has_lo[256], has_hi[256];
    memset(has_lo, 0, sizeof(has_lo));
    memset(has_hi, 0, sizeof(has_hi));
    size_t pos = be32(d + 0x10);
    while (pos + 16 <= n && !memcmp(d + pos, "CHIP", 4)) {
        uint32_t len = be32(d + pos + 4);
        unsigned bank = be16(d + pos + 10), load = be16(d + pos + 12), size = be16(d + pos + 14);
        if (load != 0xDE00 && bank < 256) {
            int half = load == 0x8000 ? 0 : 1;
            memset(half ? exp_hi[bank] : exp_lo[bank], 0xFF, 0x2000);
            memcpy(half ? exp_hi[bank] : exp_lo[bank], d + pos + 16, size);
            (half ? has_hi : has_lo)[bank] = 1;
        }
        pos += len;
    }
    int bad = 0;
    for (int b = 0; b < c->banks; b++)
        for (int half = 0; half < (single ? 1 : 2); half++)
            for (int i = 0; i < 0x2000; i++) {
                uint8_t want = (half ? has_hi : has_lo)[b] ? (half ? exp_hi : exp_lo)[b][i] : 0xFF;
                if (ef && b == 0 && half == 1 && i >= 0x1800 && i < 0x1B00) continue; // the EAPI
                if (c->bank_byte(c, b, half, i) != want) {
                    if (bad < 3) printf("  %s: bank %d half %d +%04X is %02X, want %02X\n", what, b, half, i, c->bank_byte(c, b, half, i), want);
                    bad++;
                }
            }
    CHECK(bad == 0, "%s: %d flash bytes differ from the CRT", what, bad);
    if (c->eeprom) {
        int eb = 0;
        pos = be32(d + 0x10);
        while (pos + 16 <= n && !memcmp(d + pos, "CHIP", 4)) {
            if (be16(d + pos + 12) == 0xDE00)
                for (int i = 0; i < 2048; i++)
                    if (c->eeprom[i] != d[pos + 16 + i]) eb++;
            pos += be32(d + pos + 4);
        }
        CHECK(eb == 0, "%s: %d EEPROM bytes differ from the CRT", what, eb);
    }
    free(d);
    return bad;
}

// ---- golden: the stock traffic ------------------------------------------------------------------------------
typedef struct { const char *name; uint64_t hash; long lines; } digest_t;
static digest_t digests[64];
static int ndigests;

static void stock_scenario(const char *name, const char *cart_name, const char *action, const char *file,
                           const int *keys, int nkeys, const char *uiname, int pref, int menu)
{
    scen_t s;
    memset(&s, 0, sizeof(s));
    s.cart = cart_name;
    s.cfg = gideon_cfg(!strcmp(action, "dump") ? "d" : NULL);
    s.cfg.pref = pref;
    s.action = action;
    s.file = file;
    s.nkeys = nkeys;
    memcpy(s.keys, keys, (size_t)nkeys * sizeof(int));
    s.uiname = uiname;
    s.menu_choice = menu;
    cart_t *c = make_cart(cart_name);
    trace_begin();
    run_app(c, &s);
    uint64_t h;
    long n;
    trace_end(name, &h, &n);
    digests[ndigests].name = strdup(name);
    digests[ndigests].hash = h;
    digests[ndigests].lines = n;
    ndigests++;
    CHECK(!mock.hang, "%s hangs", name);
    if (c && !strcmp(action, "dump") && nkeys && keys[0] == KF3 && strcmp(name, "ef1_auto_dump"))
        check_dump(name, c, !strcmp(c->name, "ef3") ? "/out/Game One.crt" : "/out/cartridge.crt", 1);
    if (c && !strcmp(action, "write") && pref == 1 && strcmp(c->name, "rom16"))
        check_written(name, c, file, 1);
    CHECK(mock.reset_requested == 0, "%s: Gideon path asked for a reset", name);
    CHECK(mock.trx_reads == 0 && mock.trx_writes == 0, "%s: Gideon path touched the TRX block", name);
    if (c)
        cart_free(c);
}

static const char *carts6[] = { "rom16", "ef1", "megabyter", "gmod2", "megacart", "ef3" };

static void run_stock(void)
{
    mkpath("/out");
    mkpath("/crt");
    static const int k_dump[] = { KF3 };
    static const int k_scan[] = { KF1 };
    static const int k_write[] = { KF3, KF5 };
    static const int k_insp[] = { KF1, KDOWN, KRIGHT, KF3, KF5, KF7, KLEFT, KUP };
    for (int i = 0; i < 6; i++) {
        const char *c = carts6[i];
        char name[64], crt[128];
        snprintf(crt, sizeof(crt), "/crt/%s.crt", c);
        make_write_crt(c, crt);
        snprintf(name, sizeof(name), "%s_dump", c);
        stock_scenario(name, c, "dump", NULL, k_dump, 1, NULL, 1, 0);
        snprintf(name, sizeof(name), "%s_scan", c);
        stock_scenario(name, c, "dump", NULL, k_scan, 1, NULL, 1, 0);
        snprintf(name, sizeof(name), "%s_inspect", c);
        stock_scenario(name, c, "inspect", NULL, k_insp, 8, "DE00", 1, 0);
        snprintf(name, sizeof(name), "%s_write", c);
        stock_scenario(name, c, "write", crt, k_write, 2, NULL, 1, 0);
        snprintf(name, sizeof(name), "%s_flash", c);
        stock_scenario(name, c, "flash", NULL, k_scan, 1, NULL, 1, 0);
    }
    stock_scenario("nocart_dump", "none", "dump", NULL, k_dump, 1, NULL, 1, 0);
    stock_scenario("nocart_write", "none", "write", "/crt/ef1.crt", k_write, 2, NULL, 1, 0);
    stock_scenario("ef1_auto_dump", "ef1", "dump", NULL, k_dump, 1, NULL, 0, 0);
    stock_scenario("ef1_ext_write", "ef1", "write", "/crt/ef1.crt", k_write, 2, NULL, 2, 0);
}

static void compare_golden(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        printf("FAIL: no golden file %s\n", path);
        fails++;
        return;
    }
    char line[256];
    int seen = 0;
    while (fgets(line, sizeof(line), f)) {
        char name[64];
        unsigned long long h;
        long n;
        if (sscanf(line, "%63s %lld %llx", name, (long long *)&n, &h) != 3)
            continue;
        for (int i = 0; i < ndigests; i++)
            if (!strcmp(digests[i].name, name)) {
                seen++;
                CHECK(digests[i].lines == n && digests[i].hash == h,
                      "H1 %s: stock traffic changed (%ld lines %016llx, golden %ld lines %016llx)", name,
                      digests[i].lines, (unsigned long long)digests[i].hash, n, h);
            }
    }
    fclose(f);
    CHECK(seen == ndigests, "H1: %d of %d scenarios have a golden entry", seen, ndigests);
}

#ifndef STOCK_ONLY
#include "tests_trx.inc"
#else
static void run_trx_tests(void) {}
#endif

int main(int argc, char **argv)
{
    char tmpl[] = "/tmp/trexhostXXXXXX";
    if (!mkdtemp(tmpl)) {
        perror("mkdtemp");
        return 2;
    }
    snprintf(root, sizeof(root), "%s", tmpl);
    snprintf(mock_fsroot, sizeof(mock_fsroot), "%s", root);
    const char *golden = "golden/stock.txt";
    int gen = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--golden")) gen = 1;
        else if (!strcmp(argv[i], "--trace") && i + 1 < argc) trace_dir = argv[++i];
        else if (!strcmp(argv[i], "--golden-file") && i + 1 < argc) golden = argv[++i];
    }
    run_stock();
    if (gen) {
        for (int i = 0; i < ndigests; i++)
            printf("%s %ld %016llx\n", digests[i].name, digests[i].lines, (unsigned long long)digests[i].hash);
        return fails ? 1 : 0;
    }
    compare_golden(golden);
    run_trx_tests();
    printf("%s: %d checks, %d failures\n", fails ? "FAILED" : "PASS", checks, fails);
    char cmd[300];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", root);
    if (system(cmd)) {}
    return fails ? 1 : 0;
}
