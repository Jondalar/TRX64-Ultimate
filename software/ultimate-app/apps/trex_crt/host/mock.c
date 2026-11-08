/*
 * mock.c - the u2p_api_t the app is linked against on the host (see mock.h).
 */
#include "mock.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <sys/stat.h>

mock_cfg_t mock_cfg;
mock_state_t mock;
cart_t *mock_cart;
FILE *mock_trace;
mock_ui_t mock_ui;
char mock_fsroot[256] = ".";

static uint32_t s_tick;
static uint8_t s_saved_pdir, s_saved_pout;
static int s_saved_ext, s_saved_int;
static uint8_t s_last_bus = 0xFF;
static uint8_t s_dummy_regs[16];

static void T(const char *fmt, ...)
{
    if (!mock_trace)
        return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(mock_trace, fmt, ap);
    va_end(ap);
    fputc('\n', mock_trace);
}

// ---- the bus ----------------------------------------------------------------------------------------------
enum { R_RAM, R_KERNAL, R_BASIC, R_CHAR, R_ROMH, R_ROML, R_IO, R_NONE };

// freeze_resp.v's pla(), the PLA as the arbiter decodes it
static int pla(int game, int exrom, int charen, int hiram, int loram, uint16_t addr, int we)
{
    int n = addr >> 12;
    int g = game, e = exrom, ch = charen, h = hiram, l = loram;
    if (!g && e) // Ultimax
        return (n == 0xF || n == 0xE) ? R_ROMH : n == 0xD ? R_IO : (n == 9 || n == 8) ? R_ROML : n == 0 ? R_RAM : R_NONE;
    // 10111 11111 00111: charen, hiram, loram = 111
    if (ch && h && l) {
        if (we)
            return n == 0xD ? R_IO : R_RAM;
        if (n == 0xF || n == 0xE)
            return R_KERNAL;
        if (n == 0xD)
            return R_IO;
        if (n == 0xB || n == 0xA)
            return g ? R_BASIC : R_ROMH;
        if ((n == 9 || n == 8) && !e)
            return R_ROML;
        return R_RAM;
    }
    // x011: hiram, loram, no charen
    if (!ch && h && l) {
        if (we)
            return R_RAM;
        if (n == 0xF || n == 0xE)
            return R_KERNAL;
        if (n == 0xD)
            return R_CHAR;
        if (n == 0xB || n == 0xA)
            return g ? R_BASIC : R_ROMH;
        if ((n == 9 || n == 8) && !e)
            return R_ROML;
        return R_RAM;
    }
    if (!g && !e && ch && h && !l) { // 00110
        if (we)
            return n == 0xD ? R_IO : R_RAM;
        if (n == 0xF || n == 0xE)
            return R_KERNAL;
        if (n == 0xD)
            return R_IO;
        if (n == 0xB || n == 0xA)
            return R_ROMH;
        return R_RAM;
    }
    if (!g && !e && !ch && h && !l) { // 00010
        if (we)
            return R_RAM;
        if (n == 0xF || n == 0xE)
            return R_KERNAL;
        if (n == 0xD)
            return R_CHAR;
        if (n == 0xB || n == 0xA)
            return R_ROMH;
        return R_RAM;
    }
    if (ch && !h && l) // x101
        return n == 0xD ? R_IO : R_RAM;
    if (!ch && !h && l)
        return we ? R_RAM : n == 0xD ? R_CHAR : R_RAM;
    if (ch && h && !l) // x110
        return we ? (n == 0xD ? R_IO : R_RAM) : (n == 0xF || n == 0xE) ? R_KERNAL : n == 0xD ? R_IO : R_RAM;
    if (!ch && h && !l)
        return we ? R_RAM : (n == 0xF || n == 0xE) ? R_KERNAL : n == 0xD ? R_CHAR : R_RAM;
    return R_RAM;
}

static uint8_t s_ram[65536];

static int pins(void)
{
    return mock_cart ? mock_cart->pins(mock_cart) : 3;
}

static int region(uint16_t a, int we)
{
    int p = pins();
    int game = p & 1, exrom = (p >> 1) & 1;
    if (mock.app_ultimax) { // the freeze's Ultimax is internal: the real port serves nothing
        game = 0;
        exrom = 1;
    }
    uint8_t lhc = (uint8_t)((mock.pout & mock.pdir) | ~mock.pdir);
    return pla(game, exrom, (lhc >> 2) & 1, (lhc >> 1) & 1, lhc & 1, a, we);
}

static void count_access(void)
{
    mock.accesses++;
    if (mock_cfg.fail_at && mock.accesses >= mock_cfg.fail_at && mock_cart)
        mock_cart->broken = true;
    if (mock_cfg.max_accesses && mock.accesses > mock_cfg.max_accesses) {
        mock.hang = 1;
        fprintf(stderr, "FAIL: hang guard at %ld accesses\n", mock.accesses);
        exit(3);
    }
}

static uint8_t bus_read(uint16_t a)
{
    int r = region(a, 0);
    // undriven: what the bus holds. Fixed 0x3C (the VIC's last byte, never a value a test writes), or with
    // open_echo the byte of the last bus cycle, e.g. the value the app just wrote to the same address
    uint8_t v = mock_cfg.open_echo ? s_last_bus : 0x3C;
    int driven = 0;
    switch (r) {
    case R_RAM:
        if (a == 0)
            v = mock.pdir;
        else if (a == 1)
            v = (uint8_t)((mock.pout & mock.pdir) | ~mock.pdir);
        else
            v = s_ram[a];
        driven = 1;
        break;
    case R_KERNAL:
        v = (uint8_t)(0xE0 ^ (a & 0xFF) ^ (a >> 8));
        driven = 1;
        break;
    case R_BASIC:
        v = (uint8_t)(0xB0 ^ (a & 0xFF));
        driven = 1;
        break;
    case R_CHAR:
        v = (uint8_t)(0xC0 ^ (a & 0xFF));
        driven = 1;
        break;
    case R_ROML:
    case R_ROMH:
        if (mock_cart && !mock.app_ultimax)
            driven = mock_cart->rom_rd(mock_cart, r == R_ROMH, a & 0x1FFF, &v);
        break;
    case R_IO:
        if (a >= 0xDE00) {
            if (mock_cart)
                driven = mock_cart->io_rd(mock_cart, a, &v);
        } else {
            v = 0x00; // VIC, SID, CIA: not modelled
            driven = 1;
        }
        break;
    default:
        break;
    }
    (void)driven;
    s_last_bus = v;
    return v;
}

static void bus_write(uint16_t a, uint8_t v)
{
    int r = region(a, 1);
    s_last_bus = v;
    switch (r) {
    case R_RAM:
        if (a == 0)
            mock.pdir = v;
        else if (a == 1)
            mock.pout = v;
        s_ram[a] = v;
        break;
    case R_IO:
        if (a >= 0xDE00 && mock_cart)
            mock_cart->io_wr(mock_cart, a, v);
        break;
    case R_ROML:
    case R_ROMH:
        // only Ultimax maps ROML/ROMH for a write cycle
        if (mock.app_ultimax)
            break;
        if (mock_cfg.trx && !mock.wr_en) {
            mock.unguarded_rom_writes++;
            mock.blocked_rom_writes++;
            break;
        }
        mock.rom_writes++;
        if (mock_cart)
            mock_cart->rom_wr(mock_cart, r == R_ROMH, a & 0x1FFF, v);
        break;
    default:
        break;
    }
}

// ---- TRX registers ------------------------------------------------------------------------------------------
uint32_t mock_status_word(void)
{
    uint32_t p = (uint32_t)pins();
    return (p & 3) | 4u | ((uint32_t)(mock.ext_mask & 15) << 8) | ((uint32_t)(mock.int_mask & 15) << 12) |
           (mock_cfg.armed ? 1u << 16 : 0) | (mock_cfg.fault ? 1u << 17 : 0) | (mock.wr_en ? 1u << 18 : 0) |
           ((uint32_t)(mock_cfg.fault_cause & 15) << 20) | (mock_cfg.has_slot ? 1u << 31 : 0);
}

static uint32_t s_status_latch, s_ctrl_buf;

uint8_t mock_trx_rd8(uint32_t off)
{
    mock.trx_reads++;
    if (off < 4)
        return s_dummy_regs[off]; // the ID the scenario configured ("TRX2", or bad_id)
    if (off == 4)
        return 0xFF;
    if (off >= 0x7000 && off < 0x7004)
        return (uint8_t)(((mock.wr_en ? 2u : 0u) >> (8 * (off & 3))) & 0xFF);
    if (off >= 0x7004 && off < 0x7008) {
        if (off == 0x7004)
            s_status_latch = mock_status_word();
        return (uint8_t)(s_status_latch >> (8 * (off & 3)));
    }
    return 0;
}

void mock_trx_wr8(uint32_t off, uint8_t v)
{
    mock.trx_writes++;
    if (off >= 0x7000 && off < 0x7004) {
        int b = (int)(off & 3);
        s_ctrl_buf = (s_ctrl_buf & ~(0xFFu << (8 * b))) | ((uint32_t)v << (8 * b));
        if (b == 3) { // a write of byte 3 commits (trx_sys.v)
            bool want = (s_ctrl_buf & 2) != 0;
            if (want && mock_cfg.wr_stuck)
                want = false;
            if (!want && mock.wr_en && mock_cfg.wr_sticky)
                want = true;
            mock.wr_en = want;
        }
    }
}

// ---- api: sys -----------------------------------------------------------------------------------------------
char mock_logbuf[1 << 16];
size_t mock_loglen;
static int a_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(mock.last_log, sizeof(mock.last_log), fmt, ap);
    va_end(ap);
    {   // every line of the run, the last 64 KB
        size_t n = strlen(mock.last_log);
        if (mock_loglen + n >= sizeof(mock_logbuf)) {
            memmove(mock_logbuf, mock_logbuf + sizeof(mock_logbuf) / 2, mock_loglen - sizeof(mock_logbuf) / 2);
            mock_loglen -= sizeof(mock_logbuf) / 2;
        }
        memcpy(mock_logbuf + mock_loglen, mock.last_log, n);
        mock_loglen += n;
        mock_logbuf[mock_loglen] = 0;
    }
    if (getenv("TREX_LOG"))
        fputs(mock.last_log, stderr);
    return 0;
}
static void a_delay(uint32_t ms) { s_tick += ms; }
static uint32_t a_tick(void) { return s_tick += 5; }
static int a_flash_read(uint32_t addr, void *buf, int len) { (void)addr; (void)buf; return len; }
static void a_handover(void) {}

static const u2p_sys_api_t sys_api = { sizeof(u2p_sys_api_t), a_log, a_delay, a_tick, a_flash_read, a_handover };

// ---- mem ------------------------------------------------------------------------------------------------------
// mock_alloc_max: a request above it fails (a heap without a big contiguous block); mock_alloc_big counts the
// largest request that was made
size_t mock_alloc_max, mock_alloc_big;
// mock_alloc_n: only that many more allocations succeed (0 = no limit, -1 = none); mock_alloc_live: allocations not freed yet
int mock_alloc_n, mock_alloc_live;
static void *m_malloc(size_t n)
{
    if (n > mock_alloc_big) mock_alloc_big = n;
    if (mock_alloc_max && n > mock_alloc_max) return NULL;
    if (mock_alloc_n < 0) return NULL;
    if (mock_alloc_n > 0 && --mock_alloc_n == 0) mock_alloc_n = -1; // this one is the last
    void *p = malloc(n);
    if (p) mock_alloc_live++;
    return p;
}
static void m_free(void *p) { if (p) mock_alloc_live--; free(p); }
static void *m_realloc(void *p, size_t n)
{
    if (n > mock_alloc_big) mock_alloc_big = n;
    return (mock_alloc_max && n > mock_alloc_max) ? NULL : realloc(p, n);
}
static const u2p_mem_api_t mem_api = { sizeof(u2p_mem_api_t), m_malloc, m_free, m_realloc };

// ---- ui -------------------------------------------------------------------------------------------------------
static void u_popup(const char *t, const char *m)
{
    (void)t;
    mock.popups++;
    snprintf(mock.last_popup, sizeof(mock.last_popup), "%s", m);
}
static int u_confirm(const char *m) { (void)m; return 1; }
int mock_retry_popups, mock_retry_yes;
static int u_popup_buttons(const char *m, uint8_t b)
{
    mock.popups++;
    snprintf(mock.last_popup, sizeof(mock.last_popup), "%s", m);
    if (strstr(m, "Saving failed")) { // the dumper's retry: YES mock_retry_yes times, then NO
        mock_retry_popups++;
        return mock_retry_popups <= mock_retry_yes ? BUTTON_YES : BUTTON_NO;
    }
    if (b & BUTTON_YES)
        return mock_ui.popup_yes ? mock_ui.popup_yes : BUTTON_YES;
    return BUTTON_OK;
}
static int u_menu(const char *t, const char **i, int n) { (void)t; (void)i; (void)n; return mock_ui.menu_choice; }
static int u_menu_keys(const char *t, const char **i, int n, const char *k) { (void)k; return u_menu(t, i, n); }
static int u_string_input(const char *t, char *buf, int max)
{
    (void)t;
    (void)max;
    if (mock_ui.name[0])
        snprintf(buf, 64, "%s", mock_ui.name);
    return (int)strlen(buf) ? (int)strlen(buf) : -1;
}
static int u_path_input(char *buf, int max)
{
    snprintf(buf, (size_t)max, "%s", mock_ui.dir);
    return 0;
}
static int u_getkey(void) { return -1; }
static int u_getch(void)
{
    if (mock_ui.keypos < mock_ui.nkeys)
        return mock_ui.keys[mock_ui.keypos++];
    return -2;
}
static int u_fkey(int n)
{
    static const int f[8] = { 0x85, 0x89, 0x86, 0x8A, 0x87, 0x8B, 0x88, 0x8C };
    return (n >= 1 && n <= 8) ? f[n - 1] : -1;
}
static int u_color(int w) { return w; }
static void *u_progress_open(const char *t, int n) { (void)t; (void)n; return (void *)1; }
static void u_progress_set(void *h, int v, const char *m) { (void)h; (void)v; (void)m; }
static void u_progress_close(void *h) { (void)h; }
static char s_win;
static void *u_screen_open(const char *t, int w, int h) { (void)t; (void)w; (void)h; return &s_win; }
static void *u_screen_open_full(void) { return &s_win; }
static void u_screen_close(void *h) { (void)h; }
static void u_screen_puts(void *h, int x, int y, const char *s, int c) { (void)h; (void)x; (void)y; (void)s; (void)c; }
static void u_win0(void *h) { (void)h; }
static void u_win_i(void *h, int i) { (void)h; (void)i; }
static void u_win_ii(void *h, int a, int b) { (void)h; (void)a; (void)b; }
static void u_win_c(void *h, char c) { (void)h; (void)c; }
static void u_win_s(void *h, const char *s) { (void)h; (void)s; }
static void u_win_rep(void *h, char c, int n) { (void)h; (void)c; (void)n; }
static void u_win_chat(void *h, int x, int y, char c) { (void)h; (void)x; (void)y; (void)c; }
static int u_win_size(void *h) { (void)h; return 22; }

static const u2p_ui_api_t ui_api = {
    sizeof(u2p_ui_api_t), u_popup, u_confirm, u_popup_buttons, u_menu, u_menu_keys, u_string_input, u_path_input,
    u_getkey, u_getch, u_fkey, u_color, u_progress_open, u_progress_set, u_progress_close, u_screen_open,
    u_screen_open_full, u_screen_close, u_screen_puts, u_win0, u_win_i, u_win_i, u_win_i, u_win_ii, u_win_c, u_win_s,
    u_win_rep, u_win_chat, u_win0, u_win0, u_win_size, u_win_size
};

// ---- fs -------------------------------------------------------------------------------------------------------
int mock_open_files; // files the app has open now
static void hostpath(char *out, size_t n, const char *p)
{
    snprintf(out, n, "%s%s", mock_fsroot, p);
}
// The file API as the firmware gives it: newlib's FILE on top of ult_syscalls.cc. Its _lseek has no SEEK_CUR, and
// __sflush_r (newlib 4.2, fflush.c) asks for exactly that when fseek finds unread bytes in the read buffer of a file
// whose offset it does not know yet (__SOFF clear): the error is not ESPIPE/EINVAL, so the fseek fails with EOF and
// the position stays where it was. Only a file that is read to the end of its buffer, or has not been read, seeks.
// mock_seekcur_works = 1 models a firmware whose _lseek tracks the offset (then every fseek works).
int mock_seekcur_works;
long mock_failed_seeks; // fseeks that returned EOF
typedef struct {
    FILE *h; // the descriptor: its position is the file offset of _lseek
    unsigned char buf[1024];
    int p, r; // newlib's _p offset and _r: unread bytes in buf
    long off; // fp->_offset, valid when soff
    int soff;
    long limit, written; // limit >= 0: the drive drops out after that many written bytes (writes then return 0)
} nfile_t;
// The next mock_wfail_opens files opened for writing lose their drive after mock_wfail_after bytes
int mock_wfail_opens, mock_wfail_skip;
long mock_wfail_after;
static void *f_fopen(const char *path, const char *mode)
{
    char h[512];
    hostpath(h, sizeof(h), path);
    FILE *f = fopen(h, mode);
    if (!f) return NULL;
    nfile_t *n = calloc(1, sizeof(*n));
    n->h = f;
    n->limit = -1;
    if (strchr(mode, 'w')) {
        if (mock_wfail_skip > 0) mock_wfail_skip--;
        else if (mock_wfail_opens > 0) {
            mock_wfail_opens--;
            n->limit = mock_wfail_after;
        }
    }
    mock_open_files++;
    return n;
}
static int f_fread(void *b, int s, int n, void *vf)
{
    nfile_t *f = vf;
    size_t want = (size_t)s * (size_t)n, got = 0;
    while (got < want) {
        if (f->r == 0) {
            size_t k = fread(f->buf, 1, sizeof(f->buf), f->h);
            if (k == 0) break;
            if (f->soff) f->off += (long)k;
            f->p = 0;
            f->r = (int)k;
        }
        size_t take = want - got < (size_t)f->r ? want - got : (size_t)f->r;
        memcpy((char *)b + got, f->buf + f->p, take);
        f->p += (int)take;
        f->r -= (int)take;
        got += take;
    }
    return s ? (int)(got / (size_t)s) : 0;
}
static int f_fwrite(const void *b, int s, int n, void *vf)
{
    nfile_t *f = vf;
    long len = (long)s * n;
    if (f->limit >= 0 && f->written + len > f->limit) return 0;
    f->written += len;
    return (int)fwrite(b, (size_t)s, (size_t)n, f->h);
}
static int f_fseek(void *vf, long o, int w)
{
    nfile_t *f = vf;
    if (w != 0) { // SEEK_END/SEEK_CUR are not used by the app on this path
        return fseek(f->h, o, w);
    }
    if (f->r > 0) { // __sflush_r: the physical offset is needed to drop the read buffer
        long cur;
        if (f->soff) cur = f->off;
        else if (mock_seekcur_works) cur = ftell(f->h);
        else { mock_failed_seeks++; return -1; } // _lseek(SEEK_CUR) = -1, errno set
        cur -= f->r;
        fseek(f->h, cur, SEEK_SET);
        f->soff = 1;
        f->off = cur;
        f->r = 0;
    }
    if (fseek(f->h, o, SEEK_SET)) return -1;
    f->soff = 1;
    f->off = o;
    f->p = f->r = 0;
    return 0;
}
static long f_ftell(void *vf) { return ftell(((nfile_t *)vf)->h) - ((nfile_t *)vf)->r; }
static int f_fclose(void *vf)
{
    mock_open_files--;
    int r = fclose(((nfile_t *)vf)->h);
    free(vf);
    return r;
}
static int f_exists(const char *path, const char *name)
{
    char h[512], p[300];
    snprintf(p, sizeof(p), "%s/%s", path, name);
    hostpath(h, sizeof(h), p);
    struct stat st;
    return stat(h, &st) == 0;
}
static int f_mkdir(const char *p) { (void)p; return 0; }
static const u2p_fs_api_t fs_api = { sizeof(u2p_fs_api_t), f_fopen, f_fread, f_fwrite, f_fseek, f_ftell, f_fclose,
                                     f_exists, f_mkdir };

// ---- c64 ------------------------------------------------------------------------------------------------------
static uint8_t c_peek(uint16_t a)
{
    count_access();
    uint8_t v = bus_read(a);
    T("PEEK %04X", a);
    return v;
}
static void c_poke(uint16_t a, uint8_t v)
{
    count_access();
    T("POKE %04X %02X", a, v);
    bus_write(a, v);
}
static int c_dma_read(uint16_t a, void *dst, int len)
{
    count_access();
    T("READ %04X %d", a, len);
    uint8_t *d = dst;
    for (int i = 0; i < len; i++)
        d[i] = bus_read((uint16_t)(a + i));
    return len;
}
static int c_dma_write(uint16_t a, const void *src, int len)
{
    count_access();
    T("WRITE %04X %d", a, len);
    const uint8_t *s = src;
    for (int i = 0; i < len; i++)
        bus_write((uint16_t)(a + i), s[i]);
    return len;
}
static void c_setup(void)
{
    count_access();
    T("SETUP_BUS");
    s_saved_pdir = mock.pdir;
    s_saved_pout = mock.pout;
    s_saved_ext = mock.ext_mask;
    s_saved_int = mock.int_mask;
    if (!mock_cfg.masks_stuck) {
        mock.ext_mask = 15;
        mock.int_mask = 0;
    }
    mock.pdir = 0x2F;
    mock.pout = 0x37;
    mock.bus_open = true;
}
static void c_restore(void)
{
    count_access();
    T("RESTORE_BUS");
    mock.pdir = s_saved_pdir;
    mock.pout = s_saved_pout;
    mock.ext_mask = s_saved_ext;
    mock.int_mask = s_saved_int;
    mock.bus_open = false;
}
static void c_mode(uint8_t m)
{
    count_access();
    T("MODE %d", m);
    mock.app_ultimax = (m == 2);
}
static uint8_t c_detect(void)
{
    T("DETECT");
    return (uint8_t)pins();
}
static int c_pref(void)
{
    T("PREF");
    return mock_cfg.pref;
}
static const u2p_c64_api_t c64_api = { sizeof(u2p_c64_api_t), c_peek, c_poke, c_dma_read, c_dma_write, c_dma_read,
                                       c_setup, c_restore, c_mode, c_detect, c_pref };

// ---- media ----------------------------------------------------------------------------------------------------
static int md_source(int r, char *b, int l) { (void)r; (void)b; (void)l; return 0; }
static int md_mount(int d, const char *p, int m) { (void)d; (void)p; (void)m; return 0; }
static int md_unmount(int d) { (void)d; return 0; }
static int md_run(const char *p) { (void)p; return 0; }
static int md_reset(void)
{
    T("MEDIA_RESET");
    mock.reset_requested++;
    return 0;
}
static const u2p_media_api_t media_api = { sizeof(u2p_media_api_t), md_source, md_mount, md_unmount, md_run, md_reset };
static const u2p_media_api_t media_api_noreset = { offsetof(u2p_media_api_t, reset), md_source, md_mount, md_unmount,
                                                   md_run, NULL };

// ---- trx --------------------------------------------------------------------------------------------------------
static const u2p_trx_api_t trx_api = { sizeof(u2p_trx_api_t), s_dummy_regs, 0xFF, NULL, NULL, NULL, NULL, NULL };

static u2p_api_t s_api;

void mock_ctx(u2p_app_context_t *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->size = sizeof(*ctx);
    ctx->app_path = "/Usb0/apps/trex_crt.u2a";
    ctx->has_ui = 1;
}

void mock_init(cart_t *cart, const mock_cfg_t *cfg)
{
    mock_cfg = *cfg;
    memset(&mock, 0, sizeof(mock));
    memset(s_ram, 0, sizeof(s_ram));
    mock_cart = cart;
    if (cart) {
        cart->broken = false;
        cart->reset(cart);
    }
    mock.ext_mask = cfg->ext_mask;
    mock.int_mask = cfg->int_mask;
    mock.pdir = cfg->prog_pdir;
    mock.pout = cfg->prog_pout;
    mock.app_ultimax = cfg->app_ultimax;
    mock.wr_en = cfg->wr0;
    s_ctrl_buf = cfg->wr0 ? 2 : 0;
    memcpy(s_dummy_regs, cfg->bad_id ? "\0\0\0\0" : "TRX2", 4);
    s_tick = 1000;
    s_last_bus = 0xFF;
    memset(&mock_ui, 0, sizeof(mock_ui));
    mock_loglen = 0;
    mock_logbuf[0] = 0;
    snprintf(mock_ui.dir, sizeof(mock_ui.dir), "/out");
    s_api.abi_major = 2;
    s_api.abi_minor = 1;
    s_api.size = sizeof(s_api);
    s_api.firmware_version = "3.15";
    s_api.sys = &sys_api;
    s_api.mem = &mem_api;
    s_api.ui = &ui_api;
    s_api.fs = &fs_api;
    s_api.c64 = &c64_api;
    s_api.media = cfg->has_media_reset ? &media_api : &media_api_noreset;
    s_api.config = NULL;
    s_api.diag = NULL;
    s_api.net = NULL;
    s_api.trx = cfg->trx ? &trx_api : NULL;
}

const u2p_api_t *mock_api(void) { return &s_api; }
