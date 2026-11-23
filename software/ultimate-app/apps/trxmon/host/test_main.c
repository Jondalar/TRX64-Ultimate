/*
 * Host unit tests for trxmon: WebSocket framing and handshake, JSON, the
 * disassembler against TRX64's goldens, and the JSON-RPC methods and
 * monitor verbs over a fake debug block. Run: make -C host test
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>

#include "../src/dis6502.h"
#include "../src/asm6502.h"
#include "../src/mjson.h"
#include "../src/mach.h"
#include "../src/mon.h"
#include "../src/rpc.h"
#include "../src/ring.h"
#include "../src/spans.h"
#include "../src/ui.h"
#include "../src/ws.h"
#include "fake_hw.h"

static int fails, checks;

#define CHECK(c)                                                                                                       \
    do {                                                                                                               \
        checks++;                                                                                                      \
        if (!(c)) {                                                                                                    \
            fails++;                                                                                                   \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                                        \
        }                                                                                                              \
    } while (0)

#define CHECK_STR(a, b)                                                                                                \
    do {                                                                                                               \
        checks++;                                                                                                      \
        if (strcmp((a), (b)) != 0) {                                                                                   \
            fails++;                                                                                                   \
            printf("FAIL %s:%d:\n  got  [%s]\n  want [%s]\n", __FILE__, __LINE__, (a), (b));                           \
        }                                                                                                              \
    } while (0)

/* ---- server capture ---------------------------------------------------- */

static char sent[8][200000];
static int nsent;
static char notes[16][8192];
static int nnotes;

void srv_send(int cid, const char *text, int len)
{
    (void)cid;
    if (nsent < 8) {
        memcpy(sent[nsent], text, (size_t)len);
        sent[nsent][len] = 0;
        nsent++;
    }
}

void srv_cork(int on) { (void)on; }
uint32_t srv_holder_idle_ms(void) { return 0; }

void srv_broadcast(const char *text, int len)
{
    if (nnotes < 16) {
        memcpy(notes[nnotes], text, (size_t)len);
        notes[nnotes][len] = 0;
        nnotes++;
    }
}

int plat_media(int role, char *buf, int len)
{
    if (role == 0) {
        snprintf(buf, (size_t)len, "/Usb0/games/test.crt");
        return (int)strlen(buf);
    }
    return 0;
}

/* files: under /tmp/trxmon_test (the host test's own directory) */
static void host_path(const char *path, char *out, int n) { snprintf(out, (size_t)n, "/tmp/trxmon_test%s", path); }
void *plat_fopen(const char *path, const char *mode)
{
    char p[300];
    host_path(path, p, sizeof(p));
    return fopen(p, mode);
}
int plat_fread(void *buf, int n, void *f) { return (int)fread(buf, 1, (size_t)n, (FILE *)f); }
int plat_fwrite(const void *buf, int n, void *f) { return (int)fwrite(buf, 1, (size_t)n, (FILE *)f); }
void plat_fclose(void *f) { fclose((FILE *)f); }
int plat_mkdir(const char *path)
{
    char p[300];
    host_path(path, p, sizeof(p));
    return mkdir(p, 0755) == 0 || errno == EEXIST ? 0 : -1;
}
int plat_exists(const char *dir, const char *name)
{
    char p[300];
    host_path(dir, p, sizeof(p));
    size_t l = strlen(p);
    snprintf(p + l, sizeof(p) - l, "/%s", name);
    struct stat st;
    return stat(p, &st) == 0;
}

static char host_reu_en[24] = "Enabled", host_reu_size[24] = "512 KB";
int plat_config(const char *category, const char *item, char *buf, int len)
{
    (void)category;
    snprintf(buf, (size_t)len, "%s", strcmp(item, "REU Size") == 0 ? host_reu_size : host_reu_en);
    return (int)strlen(buf);
}

int plat_dir(const char *path, plat_dir_cb cb, void *ctx)
{
    char p[300];
    host_path(path, p, sizeof(p));
    DIR *d = opendir(p);
    if (!d) {
        return -1;
    }
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) {
            continue;
        }
        char q[600];
        snprintf(q, sizeof(q), "%s/%s", p, e->d_name);
        struct stat st;
        stat(q, &st);
        if (cb(ctx, e->d_name, (uint32_t)st.st_size, S_ISDIR(st.st_mode))) {
            break;
        }
    }
    closedir(d);
    return 0;
}
int plat_rmdir(const char *path)
{
    char p[300];
    host_path(path, p, sizeof(p));
    return rmdir(p) == 0 ? 0 : -1;
}

const char *plat_firmware_git(void) { return "fw1234ab"; }
const char *plat_firmware_version(void) { return "3.15"; }
uint32_t plat_ms(void) { return F.ms += 1; }
void plat_yield(void) {}
/* plat_wait_flag's sleeps: a hook may flip the flags it waits for, the clock moves on */
static void (*sleep_hook)(int n);
static int sleeps, sleep_min = 0;
void plat_sleep(uint32_t ms)
{
    sleeps++;
    if (!sleep_min || (int)ms < sleep_min) {
        sleep_min = (int)ms;
    }
    F.ms += ms;
    if (sleep_hook) {
        sleep_hook(sleeps);
    }
}

static jtok_t tk[16384];

/* Sends one request and returns the answer (or "" when none came) */
static const char *call(const char *req)
{
    nsent = 0;
    nnotes = 0;
    rpc_request(0, req, (int)strlen(req));
    if (!nsent) {
        return "";
    }
    int n = json_parse(sent[0], (int)strlen(sent[0]), tk, 16384);
    CHECK(n > 0); /* every answer is valid JSON */
    if (n <= 0) {
        printf("  bad JSON: %s\n", sent[0]);
    }
    return sent[0];
}

static int has(const char *hay, const char *needle)
{
    int r = strstr(hay, needle) != NULL;
    if (!r) {
        printf("  [%s] not in:\n  %.600s\n", needle, hay);
    }
    return r;
}

/* The output string of a monitor/exec answer, unescaped */
static char mon_out[100000];
static const char *mon(const char *cmd)
{
    char req[1024];
    snprintf(req, sizeof(req), "{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"monitor/exec\",\"params\":{\"command\":\"%s\"}}",
             cmd);
    const char *a = call(req);
    mon_out[0] = 0;
    int n = json_parse(a, (int)strlen(a), tk, 16384);
    if (n <= 0) {
        return mon_out;
    }
    int res = json_get(a, tk, 0, "result");
    int o = json_get(a, tk, res, "output");
    if (o < 0) {
        o = json_get(a, tk, res, "error");
    }
    json_str(a, tk, o, mon_out, sizeof(mon_out));
    return mon_out;
}

/* ---- ws ---------------------------------------------------------------- */

static void hex(const uint8_t *d, int n, char *out)
{
    for (int i = 0; i < n; i++) {
        snprintf(out + 2 * i, 3, "%02x", d[i]);
    }
}

static void test_ws(void)
{
    uint8_t dig[20];
    char h[41];
    ws_sha1((const uint8_t *)"abc", 3, dig);
    hex(dig, 20, h);
    CHECK_STR(h, "a9993e364706816aba3e25717850c26c9cd0d89d");
    ws_sha1((const uint8_t *)"", 0, dig);
    hex(dig, 20, h);
    CHECK_STR(h, "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    const char *l = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    ws_sha1((const uint8_t *)l, (int)strlen(l), dig);
    hex(dig, 20, h);
    CHECK_STR(h, "84983e441c3bd26ebaae4aa1f95129e5e54670f1");

    char b[64];
    ws_base64((const uint8_t *)"f", 1, b);
    CHECK_STR(b, "Zg==");
    ws_base64((const uint8_t *)"fo", 2, b);
    CHECK_STR(b, "Zm8=");
    ws_base64((const uint8_t *)"foobar", 6, b);
    CHECK_STR(b, "Zm9vYmFy");

    /* RFC 6455 section 1.3 */
    const char *req = "GET /chat?x=1&av=0 HTTP/1.1\r\nHost: server.example.com\r\nupgrade: WebSocket\r\n"
                      "Connection: keep-alive, Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                      "Sec-WebSocket-Version: 13\r\n\r\nEXTRA";
    http_req_t r;
    CHECK(ws_http_parse(req, 20, &r) == 0); /* incomplete */
    int n = ws_http_parse(req, (int)strlen(req), &r);
    CHECK(n == (int)strlen(req) - 5);
    CHECK(r.upgrade);
    CHECK(r.av == 0);
    CHECK_STR(r.path, "/chat?x=1&av=0");
    char out[256];
    ws_http_accept(&r, out, sizeof(out));
    CHECK(strstr(out, "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n") != NULL);
    CHECK(strncmp(out, "HTTP/1.1 101 ", 13) == 0);
    n = ws_http_parse("GET / HTTP/1.1\r\nHost: x\r\n\r\n", 27, &r);
    CHECK(n == 27 && !r.upgrade && r.av == 1);
    CHECK(ws_http_parse("POST / HTTP/1.1\r\n\r\n", 19, &r) == -1);

    /* RFC 6455 section 5.7: a masked "Hello" */
    uint8_t fr[] = { 0x81, 0x85, 0x37, 0xfa, 0x21, 0x3d, 0x7f, 0x9f, 0x4d, 0x51, 0x58 };
    ws_frame_t f;
    CHECK(ws_frame_parse(fr, 1, &f) == 0);
    CHECK(ws_frame_parse(fr, 5, &f) == 0);
    CHECK(ws_frame_parse(fr, sizeof(fr), &f) == 6);
    CHECK(f.fin && f.opcode == WS_TEXT && f.masked && f.len == 5);
    ws_unmask(fr + 6, 5, f.mask);
    CHECK(memcmp(fr + 6, "Hello", 5) == 0);
    uint8_t big[] = { 0x82, 0xFE, 0x01, 0x00, 1, 2, 3, 4 };
    CHECK(ws_frame_parse(big, sizeof(big), &f) == 8 && f.len == 256 && f.opcode == WS_BIN);
    uint8_t huge[] = { 0x81, 0xFF, 0, 0, 0, 0, 0, 1, 0, 0, 1, 2, 3, 4 };
    CHECK(ws_frame_parse(huge, sizeof(huge), &f) == 14 && f.len == 65536);
    uint8_t fragping[] = { 0x09, 0x80, 0, 0, 0, 0 };
    CHECK(ws_frame_parse(fragping, sizeof(fragping), &f) == -1);
    uint8_t rsv[] = { 0xC1, 0x80, 0, 0, 0, 0 };
    CHECK(ws_frame_parse(rsv, sizeof(rsv), &f) == -1);
    uint8_t hdr[10];
    CHECK(ws_frame_header(hdr, WS_TEXT, 5) == 2 && hdr[0] == 0x81 && hdr[1] == 5);
    CHECK(ws_frame_header(hdr, WS_TEXT, 300) == 4 && hdr[1] == 126 && hdr[2] == 1 && hdr[3] == 44);
    CHECK(ws_frame_header(hdr, WS_TEXT, 70000) == 10 && hdr[1] == 127 && hdr[7] == 1 && hdr[8] == 0x11 &&
          hdr[9] == 0x70);
}

/* ---- json -------------------------------------------------------------- */

static void test_json(void)
{
    const char *s = " {\"a\": [1, 2, {\"b\": \"x\\\"y\\u0041\\n\"}], \"c\": true, \"d\": null, \"e\": -3, \"f\": 18446744073709551615} ";
    jtok_t t[64];
    int n = json_parse(s, (int)strlen(s), t, 64);
    CHECK(n > 0);
    int a = json_get(s, t, 0, "a");
    CHECK(json_is(t, a, JT_ARR) && t[a].size == 3);
    uint32_t v;
    CHECK(json_u32(s, t, json_item(t, a, 1), &v) && v == 2);
    int o = json_item(t, a, 2);
    char buf[16];
    CHECK(json_str(s, t, json_get(s, t, o, "b"), buf, sizeof(buf)) == 5);
    CHECK_STR(buf, "x\"yA\n");
    CHECK(json_is(t, json_get(s, t, 0, "c"), JT_TRUE));
    CHECK(json_is(t, json_get(s, t, 0, "d"), JT_NULL));
    CHECK(!json_u32(s, t, json_get(s, t, 0, "e"), &v)); /* negative */
    uint64_t big;
    CHECK(json_u64(s, t, json_get(s, t, 0, "f"), &big) == 0 || big == 18446744073709551615ull);
    CHECK(json_get(s, t, 0, "zz") == -1);
    CHECK(json_parse("{\"a\":1,}", 8, t, 64) < 0);
    CHECK(json_parse("{\"a\":1} x", 9, t, 64) < 0);
    CHECK(json_parse("[1,2,3]", 7, t, 2) == -2);
    CHECK(json_parse("\"\\q\"", 4, t, 4) < 0);

    char out[256];
    sbuf_t b;
    sb_init(&b, out, sizeof(out));
    jw_t w;
    jw_init(&w, &b);
    jw_obj(&w);
    jw_kstr(&w, "s", "q\"\\\n\x01 — ok");
    jw_key(&w, "arr");
    jw_arr(&w);
    jw_u64(&w, 1);
    jw_obj(&w);
    jw_end_obj(&w);
    jw_null(&w);
    jw_end_arr(&w);
    jw_kbool(&w, "t", 1);
    jw_end_obj(&w);
    CHECK_STR(sb_str(&b), "{\"s\":\"q\\\"\\\\\\n\\u0001 — ok\",\"arr\":[1,{},null],\"t\":true}");
}

/* ---- disassembler against TRX64's goldens ------------------------------- */

static uint8_t g_mem[3];
static uint16_t g_addr;
static uint8_t g_rd(void *ctx, uint16_t a)
{
    (void)ctx;
    return g_mem[(uint16_t)(a - g_addr) % 3];
}

static void test_dis(void)
{
    FILE *fp = fopen("goldens/disasm6502_goldens.json", "rb");
    CHECK(fp != NULL);
    if (!fp) {
        return;
    }
    static char js[200000];
    int n = (int)fread(js, 1, sizeof(js) - 1, fp);
    fclose(fp);
    js[n] = 0;
    int nt = json_parse(js, n, tk, 16384);
    CHECK(nt > 0 && tk[0].type == JT_ARR);
    int ok = 0;
    for (int i = 0; i < tk[0].size; i++) {
        int o = json_item(tk, 0, i);
        uint32_t op, addr, b1, b2, size;
        char want[64], got[64];
        json_u32(js, tk, json_get(js, tk, o, "op"), &op);
        json_u32(js, tk, json_get(js, tk, o, "addr"), &addr);
        json_u32(js, tk, json_get(js, tk, o, "b1"), &b1);
        json_u32(js, tk, json_get(js, tk, o, "b2"), &b2);
        json_u32(js, tk, json_get(js, tk, o, "size"), &size);
        json_str(js, tk, json_get(js, tk, o, "line"), want, sizeof(want));
        g_mem[0] = (uint8_t)op;
        g_mem[1] = (uint8_t)b1;
        g_mem[2] = (uint8_t)b2;
        g_addr = (uint16_t)addr;
        sbuf_t b;
        sb_init(&b, got, sizeof(got));
        int sz = dis_line(g_rd, 0, (uint16_t)addr, &b, 0, 0);
        if ((sz == (int)size) && (strcmp(sb_str(&b), want) == 0)) {
            ok++;
        } else if (fails < 20) {
            printf("dis %02x: got [%s] size %d, want [%s] size %u\n", op, sb_str(&b), sz, want, size);
        }
    }
    checks++;
    if (ok != tk[0].size || ok != 512) {
        fails++;
        printf("FAIL dis goldens: %d of %d\n", ok, tk[0].size);
    } else {
        printf("dis: %d TRX64 golden lines identical\n", ok);
    }
}

/* the assembler: every documented opcode round-trips through the disassembler */
static void test_asm(void)
{
    static const char *const doc = "LDA LDX LDY STA STX STY TAX TAY TXA TYA TSX TXS PHA PHP PLA PLP AND ORA EOR "
                                    "BIT ADC SBC CMP CPX CPY INC DEC INX INY DEX DEY ASL LSR ROL ROR JMP JSR RTS "
                                    "RTI BCC BCS BEQ BNE BMI BPL BVC BVS CLC SEC CLD SED CLI SEI CLV BRK NOP";
    int n = 0, ok = 0;
    for (int op = 0; op < 256; op++) {
        char line[64];
        g_mem[0] = (uint8_t)op;
        g_mem[1] = 0x12;
        g_mem[2] = 0x34;
        g_addr = 0xC000;
        sbuf_t b;
        sb_init(&b, line, sizeof(line));
        dis_line(g_rd, 0, 0xC000, &b, 0, 0);
        const char *ins = sb_str(&b) + 17; /* "$c000  bb bb bb  " */
        char mn[4] = { ins[0], ins[1], ins[2], 0 };
        if (!strstr(doc, mn) || ((op != 0xEA) && !strcmp(mn, "NOP")) || (op == 0xEB)) {
            continue;
        }
        n++;
        uint8_t got[3];
        char et[96];
        sbuf_t e;
        sb_init(&e, et, sizeof(et));
        int sz = asm6502_line(ins, 0xC000, got, &e);
        if ((sz == dis_len((uint8_t)op)) && (got[0] == op) && ((sz < 2) || (got[1] == 0x12)) &&
            ((sz < 3) || (got[2] == 0x34))) {
            ok++;
        } else {
            printf("asm %02x [%s]: size %d %02x (%s)\n", op, ins, sz, got[0], sb_str(&e));
        }
    }
    checks++;
    if ((ok != n) || (n != 151)) {
        fails++;
        printf("FAIL asm round trip: %d of %d\n", ok, n);
    } else {
        printf("asm: %d documented opcodes round-trip\n", ok);
    }
    static const struct
    {
        const char *in, *err;
    } bad[] = {
        { "xyz", "unknown mnemonic 'xyz'" },
        { "lda #$100", "immediate value $100 overflows a byte" },
        { "bne $d000", "branch out of range (4094 bytes)" },
        { "jmp ($10,x)", "'jmp' has no (zp,x) mode" },
        { "stx $1234,x", "'stx' has no absx mode" },
        { "nop $10", "'nop' has no zp or abs mode" },
        { "ldaa", "unparsable instruction 'ldaa'" },
        { "lda $10,z", "bad index suffix in '$10,z'" },
    };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        uint8_t got[3];
        char et[96];
        sbuf_t e;
        sb_init(&e, et, sizeof(et));
        CHECK(asm6502_line(bad[i].in, 0xC000, got, &e) == 0);
        CHECK_STR(sb_str(&e), bad[i].err);
    }
    uint8_t got[3];
    char et[96];
    sbuf_t e;
    sb_init(&e, et, sizeof(et));
    CHECK(asm6502_line("lda $0010", 0xC000, got, &e) == 3 && got[0] == 0xAD);
    CHECK(asm6502_line("lda 16", 0xC000, got, &e) == 2 && got[0] == 0xA5 && got[1] == 0x10);
    CHECK(asm6502_line("asl a", 0xC000, got, &e) == 1 && got[0] == 0x0A);
    CHECK(asm6502_line("bne c000", 0xC010, got, &e) == 2 && got[1] == 0xEE);
}

/* ---- rpc and monitor --------------------------------------------------- */

static void boot(void)
{
    fake_reset();
    mach_init();
    mon_init();
    rpc_init();
    /* a little program at $c000: lda #$01 / jsr $c010 / jmp $c000 ; $c010: inx / rts */
    static const uint8_t prg[] = { 0xA9, 0x01, 0x20, 0x10, 0xC0, 0x4C, 0x00, 0xC0 };
    memcpy(F.ram + 0xC000, prg, sizeof(prg));
    F.ram[0xC010] = 0xE8;
    F.ram[0xC011] = 0x60;
    memcpy(F.ram + 0x0400, "\x08\x05\x0c\x0c\x0f", 5); /* "HELLO" in screen codes */
    F.ram[0x0314] = 0x31;
    F.ram[0x0315] = 0xEA;
}

/* Every method ping lists is served (not -32601), and the notifications it lists are the ones the stops send.
 * The ping answer is copied: call() reuses its buffer. */
static void ping_capabilities_match(const char *ping)
{
    static char pa[200000];
    snprintf(pa, sizeof(pa), "%s", ping);
    const char *m = strstr(pa, "\"methods\":[");
    const char *end = strstr(pa, "],\"notifications\"");
    CHECK(m && end);
    if (!m || !end) {
        return;
    }
    m += strlen("\"methods\":[");
    int n = 0;
    while (m < end) {
        const char *q = strchr(m + 1, '"');
        char name[64];
        snprintf(name, sizeof(name), "%.*s", (int)(q - m - 1), m + 1);
        char req[200];
        snprintf(req, sizeof(req), "{\"jsonrpc\":\"2.0\",\"id\":99,\"method\":\"%s\"}", name);
        boot();
        const char *a = call(req);
        if (strstr(a, "-32601")) {
            printf("listed but not served: %s\n", name);
        }
        CHECK(!strstr(a, "-32601"));
        n++;
        m = q + 1;
        if (*m == ',') {
            m++;
        }
    }
    CHECK(n >= 30);
    boot();
}

static void test_rpc(void)
{
    boot();
    const char *a = call("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"ping\"}");
    CHECK(has(a, "\"id\":1"));
    CHECK(has(a, "\"runtime_version\":\"trx64-runtime/2\""));
    CHECK(has(a, "\"backend\":\"c64u\""));
    CHECK(has(a, "\"version\":\"trxmon hosttest\""));
    CHECK(has(a, "\"build\":{\"trxmon_git\":\"hosttest\",\"firmware_git\":\"fw1234ab\",\"firmware_version\":\"3.15\"}"));
    CHECK(has(a, "\"capabilities\":{\"methods\":[\"ping\",\"debug/run\",\"debug/continue\","));
    CHECK(has(a, "\"notifications\":[\"debug/breakpoint_hit\",\"debug/observer_hit\",\"debug/observer_log\",\"debug/stopped\",\"debug/running\",\"debug/paused\"]}"));
    CHECK(!strstr(a, "sandbox/") && !strstr(a, "media/mount"));
    ping_capabilities_match(a);

    a = call("{\"jsonrpc\":\"2.0\",\"id\":\"abc\",\"method\":\"sandbox/run\",\"params\":{}}");
    CHECK(has(a, "\"id\":\"abc\""));
    CHECK(has(a, "-32601"));
    CHECK(has(a, "use a local TRX64 sandbox"));
    a = call("{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"media/mount\",\"params\":{}}");
    CHECK(has(a, "-32601") && has(a, "REST API"));
    a = call("{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"checkpoint/capture\"}");
    CHECK(has(a, "-32001") && has(a, "no frame ring"));
    a = call("{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"runtime/who_wrote\"}");
    CHECK(has(a, "-32602") && has(a, "missing addr"));
    a = call("{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"runtime/who_wrote\",\"params\":{\"addr\":501}}");
    CHECK(has(a, "-32001") && has(a, "needs the instruction trace"));
    a = call("{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"trace/start_domains\"}");
    CHECK(has(a, "-32601") && has(a, "host-side"));
    a = call("{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"no/such\"}");
    CHECK(has(a, "-32601") && has(a, "method not found: no/such"));
    a = call("{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":");
    CHECK(has(a, "-32700"));
    a = call("{\"jsonrpc\":\"2.0\",\"method\":\"ping\"}");
    CHECK(a[0] == 0); /* a notification: no answer */

    a = call("{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"session/state\",\"params\":{\"session_id\":\"x\"}}");
    CHECK(has(a, "\"runState\":\"paused\""));
    CHECK(has(a, "\"cpu\":{\"pc\":49152,"));
    CHECK(has(a, "\"vectors\":{\"irq\":65352,\"nmi\":65091,\"cinv\":59953"));
    CHECK(has(a, "\"border\":14"));
    CHECK(has(a, "\"model\":\"c64-pal\""));
    CHECK(has(a, "\"cart\":{\"path\":\"/Usb0/games/test.crt\"}"));
    CHECK(has(a, "\"screenBase\":1024"));
    /* before the run control is taken the state is the hardware's: a machine the menu left running reports running */
    F.shut = 0;
    a = call("{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"session/state\",\"params\":{\"session_id\":\"x\"}}");
    CHECK(has(a, "\"runState\":\"running\"") && !F.owner);
    F.shut = 1;

    /* read_memory: TRX64 ranges, base64 */
    a = call("{\"jsonrpc\":\"2.0\",\"id\":8,\"method\":\"session/read_memory\",\"params\":{\"ranges\":[{\"addr\":49152,"
             "\"len\":3},{\"addr\":65534,\"len\":2,\"lens\":\"rom\"}]}}");
    CHECK(has(a, "\"chunks\":[{\"addr\":49152,\"len\":3,\"lens\":\"cpu\",\"bytes\":\"qQEg\"}"));
    CHECK(has(a, "{\"addr\":65534,\"len\":2,\"lens\":\"rom\",\"bytes\":\"SP8=\"}"));
    /* C64RE's readMemoryRange */
    a = call("{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"session/read_memory\",\"params\":{\"addr\":1024,\"length\":5}}");
    CHECK(has(a, "\"bytes\":[8,5,12,12,15]"));
    a = call("{\"jsonrpc\":\"2.0\",\"id\":10,\"method\":\"session/read_memory\",\"params\":{\"ranges\":[{\"addr\":0,"
             "\"len\":40000}]}}");
    CHECK(has(a, "-32602"));
    /* io lens: VIC from the state bus, SID not reachable */
    a = call("{\"jsonrpc\":\"2.0\",\"id\":11,\"method\":\"session/read_memory\",\"params\":{\"addr\":53280,\"length\":2,"
             "\"lens\":\"io\"}}");
    CHECK(has(a, "\"bytes\":[254,246]"));

    /* breakpoints */
    a = call("{\"jsonrpc\":\"2.0\",\"id\":12,\"method\":\"debug/break_add\",\"params\":{\"pc\":49168}}");
    CHECK(has(a, "\"num\":1,\"breakpoints\":[{\"num\":1,\"addr\":49168}]"));
    CHECK(F.bmp_en && (F.bmp[0xC010 >> 5] & (1u << (0xC010 & 31))));
    CHECK(F.owner);
    a = call("{\"jsonrpc\":\"2.0\",\"id\":13,\"method\":\"debug/break_list\"}");
    CHECK(has(a, "{\"breakpoints\":[{\"num\":1,\"addr\":49168}]}"));

    /* run, then the machine stops at the breakpoint by itself */
    a = call("{\"jsonrpc\":\"2.0\",\"id\":14,\"method\":\"debug/continue\",\"params\":{\"source\":\"llm\"}}");
    CHECK(has(a, "\"runState\":\"running\""));
    CHECK(has(a, "\"controlOwner\":\"llm\""));
    CHECK(nnotes == 1 && has(notes[0], "\"method\":\"debug/running\""));
    CHECK(F.running);
    nnotes = 0;
    fake_stop(0xC010, RS_BITMAP, 0);
    rpc_poll();
    CHECK(nnotes == 2);
    CHECK(has(notes[0], "\"method\":\"debug/breakpoint_hit\",\"params\":{\"session_id\":\"c64u\",\"pc\":49168,\"num\":1"));
    CHECK(has(notes[0], "\"registers\":\"  ADDR AC XR YR SP NV-BDIZC\\n.;C010 00 00 00 F6 nv-bdIzc\""));
    CHECK(has(notes[1], "\"method\":\"debug/stopped\""));
    CHECK(has(notes[1], "\"stop\":{\"reason\":\"breakpoint\",\"pc\":49168,"));
    CHECK(has(notes[1], "\"breakpointId\":1"));

    a = call("{\"jsonrpc\":\"2.0\",\"id\":15,\"method\":\"debug/state\"}");
    CHECK(has(a, "\"runState\":\"paused\"") && has(a, "\"stop\":{\"reason\":\"breakpoint\""));

    /* step */
    a = call("{\"jsonrpc\":\"2.0\",\"id\":16,\"method\":\"debug/step\"}");
    CHECK(has(a, "\"pc\":49169"));
    CHECK(nnotes == 1 && has(notes[0], "\"reason\":\"step\""));

    /* pause while running */
    call("{\"jsonrpc\":\"2.0\",\"id\":17,\"method\":\"debug/run\"}");
    a = call("{\"jsonrpc\":\"2.0\",\"id\":18,\"method\":\"debug/pause\"}");
    CHECK(has(a, "\"runState\":\"paused\"") && has(a, "\"stop\":{\"reason\":\"pause\""));
    CHECK(nnotes == 1 && has(notes[0], "\"method\":\"debug/paused\""));

    /* session/run: answered when the cycle compare stops the machine */
    F.cycles = 5000;
    a = call("{\"jsonrpc\":\"2.0\",\"id\":19,\"method\":\"session/run\",\"params\":{\"cycles\":100}}");
    CHECK(a[0] == 0);
    CHECK(F.cyc_cmp_en && F.cyc_cmp == 5099 && F.running);
    a = call("{\"jsonrpc\":\"2.0\",\"id\":20,\"method\":\"session/run\",\"params\":{\"cycles\":100}}");
    CHECK(has(a, "-32001"));
    nsent = 0;
    fake_stop(0xC005, RS_CYC_CMP, 0);
    rpc_poll();
    CHECK(nsent == 1 && has(sent[0], "\"id\":19,\"result\":{\"c64Cycles\":"));
    /* ... or by a breakpoint first */
    call("{\"jsonrpc\":\"2.0\",\"id\":21,\"method\":\"session/run\",\"params\":{\"cycles\":100000}}");
    nsent = 0;
    fake_stop(0xC010, RS_BITMAP, 0);
    rpc_poll();
    CHECK(nsent == 1 && has(sent[0], "\"breakpoint\":{\"pc\":49168,\"num\":1,\"registers\":"));

    a = call("{\"jsonrpc\":\"2.0\",\"id\":22,\"method\":\"debug/break_del\",\"params\":{\"id\":1}}");
    CHECK(has(a, "\"deleted\":true,\"breakpoints\":[]"));
    CHECK(!F.bmp_en);
}

static void test_monitor(void)
{
    boot();
    const char *o = mon("r");
    CHECK(has(o, "  ADDR AC XR YR SP NV-BDIZC  flow\n.;c000 00 00 00 f6 nv-bdIzc"));
    CHECK(has(o, "  port  $00=$2f $01=$37  LORAM=1 HIRAM=1 CHAREN=1"));
    CHECK(has(o, "  vectors  IRQ hw=$ff48  CINV $0314->$ea31     NMI hw=$fe43  NMIV $0318->$0000"));
    CHECK(has(sent[0], "\"spans\":[{\"line\":1,\"start\":2,\"end\":6,\"addr\":49152,\"space\":\"c64\",\"role\":\"pc\"}"));
    CHECK(has(sent[0], "\"machine\":{\"device\":\"c64\",\"cpuPortDirection\":47,\"cpuPortValue\":55,\"exrom\":1,\"game\":1,\"cartBank\":null}"));

    o = mon("d c000 c007");
    CHECK_STR(o, "$c000  a9 01     LDA #$01 <-- PC\n$c002  20 10 c0  JSR $c010\n$c005  4c 00 c0  JMP $c000");
    CHECK(has(sent[0], "{\"line\":1,\"start\":21,\"end\":26,\"addr\":49168,\"space\":\"c64\",\"role\":\"target\"}"));

    o = mon("m c000 c003");
    CHECK(strncmp(o, ">C:c000  a9 01 20 10", 20) == 0);
    CHECK((int)strlen(o) == 3 + 4 + 2 + 96 + 2 + 4);
    CHECK(has(sent[0], "\"role\":\"memory\",\"len\":4}"));
    o = mon("m io d020 d021");
    CHECK(strncmp(o, ">I:d020  fe f6", 14) == 0);
    o = mon("m io d400 d401");
    CHECK(strncmp(o, ">I:d400  -- --", 14) == 0);

    o = mon("wr c100 01 02 03");
    CHECK_STR(o, "wrote 3 byte(s) @ $c100 (cpu)");
    CHECK(F.ram[0xC101] == 2);
    o = mon("f c200 c204 aa bb");
    CHECK_STR(o, "filled $c200..$c204 (5 bytes, pattern 2)");
    CHECK(F.ram[0xC204] == 0xAA && F.ram[0xC203] == 0xBB);
    o = mon("t c200 c204 c300");
    CHECK_STR(o, "moved 5 byte(s) $c200..$c204 -> $c300");
    o = mon("c c200 c204 c300");
    CHECK_STR(o, "identical ($c200..$c204 == $c300)");
    o = mon("h c000 c0ff 20 xx c0");
    CHECK_STR(o, "found 1:\n  $c002");
    o = mon("screen");
    CHECK(has(o, "screen @ $0400  (VIC bank $0000, $D018=$15)\n|HELLO"));
    o = mon("io d000");
    CHECK(has(o, "VIC-II:\n  d000  00 00"));
    CHECK(has(o, "No details available."));

    o = mon("bk c010");
    CHECK_STR(o, "bk #1 set at $c010 (1 total)");
    o = mon("bk");
    CHECK_STR(o, "breakpoints:\n  #1  $c010");

    o = mon("z");
    CHECK_STR(o, "$c002  20 10 c0  JSR $c010 (step, 2 cyc)");
    /* n over the JSR: a breakpoint on the routine's first instruction counts */
    o = mon("n");
    CHECK_STR(o, "$c010  e8        INX (next, 2 cyc, hit user bp)");
    mon("del");
    /* until: answered when the unit stops the machine */
    F.pc = 0xC000;
    nsent = 0;
    mon("until c005");
    CHECK(nsent == 0);
    CHECK(F.unit[0].ctrl & U_ENABLE && F.unit[0].lo == 0xC005);
    fake_stop(0xC005, RS_UNIT_ADDR, 0);
    rpc_poll();
    CHECK(nsent == 1);
    CHECK(has(sent[0], "until $c005 reached -> .C:c005 (300 instr, 1000 cyc)"));
    CHECK(F.unit[0].ctrl == 0); /* given back */
    /* ret */
    F.pc = 0xC010;
    F.s = 0xF6;
    nsent = 0;
    mon("ret");
    CHECK((F.unit[0].ctrl & U_PREV_RET) && (F.unit[0].reg == UNIT_REG(R_SEL_S, R_OP_GE, 0xF7, 0xFF)));
    fake_stop(0xC005, RS_UNIT_ADDR, 0);
    rpc_poll();
    CHECK(has(sent[0], "$c005  4c 00 c0  JMP $c000 (return, 1000 cyc)"));
    /* a run-to that never stops is capped */
    nsent = 0;
    mon("until 1234");
    F.ms += 20000;
    rpc_poll();
    CHECK(has(sent[0], "until $1234 NOT reached"));

    /* observers */
    o = mon("obs w when store d020 do break");
    CHECK(has(o, "obs w armed:\n  * w  store $d020 do break  hits=0  hw: watch unit 0"));
    CHECK(F.unit[0].ctrl == (U_ENABLE | U_T_WRITE));
    o = mon("obs r when load c000..c0ff");
    CHECK(has(o, "watch unit 1"));
    o = mon("obs x when load 1000");
    CHECK(has(o, "both watch units are taken"));
    o = mon("obs c when exec c000 if a==1 do break");
    CHECK(has(o, "obs c armed:\n  * c  exec $c000 if a==01 do break  hits=0  hw: exec bitmap, soft: a==01"));
    o = mon("obs c del");
    call("{\"jsonrpc\":\"2.0\",\"id\":30,\"method\":\"debug/continue\"}");
    nnotes = 0;
    fake_stop(0xC000, RS_UNIT_ADDR, 0);
    rpc_poll();
    CHECK(nnotes == 2 && has(notes[0], "\"method\":\"debug/observer_hit\"") && has(notes[0], "\"observer\":\"w\""));
    o = mon("obs * del");
    CHECK(F.unit[0].ctrl == 0 && F.unit[1].ctrl == 0);

    /* register writes and the redirect (the CPU write path) */
    o = mon("r a=1 x=$22 fl=a5 q=1");
    CHECK_STR(o, "set a=$01 x=$22 fl=$a5 unknown reg 'q'");
    CHECK(F.a == 1 && F.x == 0x22 && F.p == 0xA5 && F.y == 0);
    o = mon("r pc=c005");
    CHECK_STR(o, "set pc=$c005");
    CHECK(F.pc == 0xC005 && F.a == 1);
    o = mon("r");
    CHECK(has(o, ".;c005 01 22 00"));
    o = mon("a c100 lda #$42");
    CHECK_STR(o, "c100  a9 42        $c100  a9 42     LDA #$42");
    CHECK(F.ram[0xC100] == 0xA9 && F.ram[0xC101] == 0x42);
    o = mon("sta $d020");
    CHECK_STR(o, "c102  8d 20 d0     $c102  8d 20 d0  STA $d020");
    o = mon("bogus");
    CHECK_STR(o, "a: unparsable instruction 'bogus'");
    o = mon("");
    o = mon("frobnicate");
    CHECK(has(o, "unknown command"));
    o = mon("asm c200\\n  lda #<msg\\nloop: inc $d020 ; flash\\n  bne loop\\n  jmp done\\ndone: rts\\nmsg: .byte 1,2\\n  .word msg\\nend");
    CHECK_STR(o, "asm: $c200-$c20e  15 bytes\nasm: labels  done=$c20a  loop=$c202  msg=$c20b");
    CHECK(F.ram[0xC200] == 0xA9 && F.ram[0xC201] == 0x0B && F.ram[0xC205] == 0xD0 && F.ram[0xC206] == 0xFB &&
          F.ram[0xC207] == 0x4C && F.ram[0xC208] == 0x0A && F.ram[0xC20D] == 0x0B && F.ram[0xC20E] == 0xC2);
    o = mon("asm c300 <<X");
    CHECK_STR(o, "");
    mon("zp = $fb");
    mon("  lda zp");
    mon("  sta $0400,x");
    o = mon("X");
    CHECK_STR(o, "asm: $c300-$c304  5 bytes\nasm: labels  zp=$00fb");
    CHECK(F.ram[0xC300] == 0xA5 && F.ram[0xC301] == 0xFB);
    o = mon("asm c400\\n  jmp nowhere\\n  .incbin x\\nend");
    CHECK_STR(o, "asm: line 2: unsupported directive '.incbin' \xe2\x80\x94 this is not a build system\nasm: nothing written");
    /* device drive8: r/m/d, z, bk; the rest is refused */
    F.dmem[0xEC2B] = 0xA9; /* LDA #$10 */
    F.dmem[0xEC2C] = 0x10;
    F.dmem[0xEC2D] = 0xEA;
    F.dmem[0x0300] = 0x55;
    F.dpc = 0xEC2B;
    F.s = 0xF9;
    F.ram[0x01FA] = 0x17;
    F.ram[0x01FB] = 0xC0;
    o = mon("bt");
    CHECK(has(o, "backtrace (live stack scan") && has(o, "\n  $01fa: -> $c018  (JSR return?)"));
    /* files under /tmp/trxmon_test */
    mkdir("/tmp/trxmon_test", 0755);
    o = mon("mkdir /USB1");
    o = mon("pwd");
    CHECK_STR(o, "/USB1");
    o = mon("save \\\"t.prg\\\" c000 c007");
    CHECK_STR(o, "saved t.prg: $c000..$c007 (8 bytes + load addr)");
    o = mon("bsave raw.bin c000 c001");
    CHECK_STR(o, "bsaved raw.bin: $c000..$c001 (2 bytes, raw)");
    o = mon("load t.prg 2000");
    CHECK_STR(o, "loaded t.prg: $2000..$2007 (8 bytes)");
    CHECK(F.ram[0x2000] == F.ram[0xC000] && F.ram[0x2007] == F.ram[0xC007]);
    o = mon("bload raw.bin 3000");
    CHECK_STR(o, "bloaded raw.bin: 2 bytes -> $3000..$3001");
    o = mon("load nosuch.prg");
    CHECK_STR(o, "load: no such file: /USB1/nosuch.prg");
    FILE *sf = fopen("/tmp/trxmon_test/USB1/p.s", "w");
    fputs("*= $c300\nstart: lda #1\n jmp start\n", sf);
    fclose(sf);
    o = mon("asm-file p.s");
    CHECK_STR(o, "asm-file: $c300-$c304  5 bytes\nasm-file: labels  start=$c300");
    /* ls and rmdir (fs->dir, fs->rmdir): case-insensitive order, directories marked, an empty directory */
    o = mon("mkdir Sub");
    o = mon("ls");
    CHECK(has(o, "/USB1:\n") && has(o, "  d Sub") && has(o, "  - p.s") && has(o, "  - raw.bin"));
    CHECK(strstr(o, "p.s") < strstr(o, "raw.bin") && strstr(o, "raw.bin") < strstr(o, "Sub") && strstr(o, "t.prg") > strstr(o, "Sub"));
    o = mon("ls Sub");
    CHECK_STR(o, "/USB1/Sub:\n  (empty)");
    o = mon("ls /nowhere");
    CHECK(has(o, "ls: no such directory"));
    o = mon("rmdir Sub");
    CHECK_STR(o, "rmdir Sub");
    o = mon("rmdir Sub");
    CHECK(has(o, "rmdir: could not remove"));
    o = mon("cd /nowhere");
    CHECK(has(o, "cd: no such directory: /nowhere"));
    uint16_t dpc_was = F.dpc;
    F.dpc = 0xEC2D;
    F.dcycles = 4242;
    o = mon("io 1541");
    CHECK(has(o, "1541 (drive A):\n  last opcode fetch $ec2d  drive cycles 4242\n  halted yes  held by the debugger yes  stepping no  comparator off"));
    CHECK(has(o, "VIA 1 ($1800) and VIA 2 ($1C00): not on the state bus"));
    F.dpc = dpc_was;
    F.dcycles = 0;
    o = mon("device drive8");
    CHECK_STR(o, "device: drive8");
    CHECK(has(mon("io 1541"), "1541 (drive A):")); /* also while the device is the drive */
    o = mon("r");
    CHECK(has(o, "1541 (drive 8)\n  ADDR AC XR YR SP NV-BDIZC  clk\n.;ec2b -- -- -- -- --------  0"));
    o = mon("d ec2b ec2d");
    CHECK(has(o, "$ec2b  a9 10     LDA #$10"));
    o = mon("m 0300 0300");
    CHECK(has(o, ">D:0300  55"));
    o = mon("m 1800 1800");
    CHECK(has(o, ">D:1800  --"));
    o = mon("z");
    CHECK_STR(o, "$ec2d  ea        NOP (drive step, 2 cyc)");
    o = mon("bk ec2b");
    CHECK(has(o, "bk #1 set at $ec2b on drive8") && F.drv_cmp_on && F.drv_cmp == 0xEC2B);
    o = mon("g");
    CHECK(has(o, "read-inspect only"));
    o = mon("device c64");
    CHECK_STR(o, "device: c64");
    o = mon("g c000");
    CHECK_STR(o, "continuing at .C:c000 (running — Pause to halt)");
    CHECK(F.pc == 0xC000 && F.running);
    o = mon("r a=2");
    CHECK(has(o, "r: the machine runs: pause it first"));
    o = mon("g");
    CHECK_STR(o, "continuing at .C:c000 (running — Pause to halt)");
    o = mon("rstep");
    CHECK(has(o, "needs the T20 frame ring"));
    o = mon("chis");
    CHECK(has(o, "needs the instruction trace"));
    o = mon("tracedb");
    CHECK(has(o, "export the window with `tracering`"));
    o = mon("frobnicate");
    CHECK_STR(o, "unknown command: frobnicate. Try 'help'.");
    o = mon("/frob");
    CHECK_STR(o, "unknown command: /frob. Try 'help'.");
    CHECK(has(sent[0], "\"error\":\"unknown command"));
    o = mon("help");
    CHECK(has(o, "NOT AVAILABLE ON THIS HOST"));
    o = mon("status");
    CHECK(has(o, "state: running"));
    o = mon("/quit");
    CHECK(rpc_quit_requested());
}

/* trxmon takes run control from a running machine and lets it run on; the
 * firmware taking it back (trx_dbg_yield) is seen and told */
static void test_ownership(void)
{
    boot();
    F.shut = 0;
    F.running = 1;
    const char *a = call("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"debug/break_add\",\"params\":{\"pc\":49168}}");
    CHECK(has(a, "\"num\":1"));
    CHECK(F.owner && F.runs == 1 && M.running);
    nnotes = 0;
    F.owner = 0;
    rpc_poll();
    CHECK(!M.owned && M.running);
    CHECK(nnotes == 1 && has(notes[0], "\"method\":\"debug/running\""));
}

/* the frame ring: transport, marks, files, checkpoint and transport methods */
static void test_ring(void)
{
    boot();
    F.ring_n = 10;
    F.ring_c0 = 5000000;
    /* the machine stands 1000 cycles after the newest frame start */
    F.cycles = F.ring_c0 + 9 * 19656 + 1000;
    const char *o = mon("frame");
    CHECK(has(o, "LIVE    \xe2\x96\xb6  frame 10/10   now"));
    o = mon("rewind");
    CHECK(has(o, "RINGBUFFER SIZE 10 (starts at Cycle 5000000 Frame 1 // ends at Cycle 5176904 Frame 10)"));
    CHECK(has(o, "machine now Cycle 5177904"));
    o = mon("mark here");
    CHECK(has(o, "MARK here @ Cycle 5176904 Frame 10"));
    o = mon("frame -3");
    CHECK(has(o, "STEP \xe2\x97\x80  \xe2\x97\x80|  frame 7/10   -0.1s"));
    CHECK(F.pc == 0xC006 && F.ring_landed == 6);
    /* time went back with the land: the frame start's cycle */
    o = mon("r");
    CHECK(has(o, "cycles 5117936"));
    const char *a = call("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"session/state\"}");
    CHECK(has(a, "\"c64Cycles\":5117936"));
    o = mon("goto here");
    CHECK(has(o, "frame 10/10") && F.ring_landed == 9);
    o = mon("goto c5000100");
    CHECK(has(o, "frame 1/10") && F.ring_landed == 0);
    o = mon("goto 3");
    CHECK(has(o, "REPLAY") && has(o, "frame 3/10"));
    /* a landed stop is a debugger stop like any other: the colour RAM reads through its session
     * (T21 §8.9; the fake keeps the session across a land as the fixed firmware does) */
    F.session = 1;
    F.dbg_stop = 1;
    F.cram[0] = 0x03;
    o = mon("frame -2");
    CHECK(F.ring_landed >= 0);
    mach_snapshot();
    CHECK(mach_peek("io", 0xD800, 0) == 0xF3);
    o = mon("goto 5");
    mach_snapshot();
    CHECK(mach_peek("io", 0xD800, 0) == 0xF3);
    F.session = 0;
    F.dbg_stop = 0;
    o = mon("marks");
    CHECK(has(o, "  here             Cycle      5176904  Frame    10"));
    a = call("{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"checkpoint/list\"}");
    CHECK(has(a, "{\"id\":\"cp_0_100\",\"label\":null,\"frame\":0,\"cycles\":5000000,\"pinned\":false"));
    CHECK(has(a, "{\"id\":\"cp_9_109\",\"label\":\"here\""));
    a = call("{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"checkpoint/restore\",\"params\":{\"id\":\"cp_4_104\",\"then\":\"pause\"}}");
    CHECK(has(a, "\"restored\":{\"id\":\"cp_4_104\"") && F.ring_landed == 4 && !F.running);
    a = call("{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"transport/frame\",\"params\":{\"delta\":-2}}");
    CHECK(has(a, "\"frameIndex\":3") && F.ring_landed == 2);
    a = call("{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"transport/status\"}");
    CHECK(has(a, "\"cursor\":\"cp_2_102\"") && has(a, "\"atHead\":false"));
    a = call("{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":\"mark/list\"}");
    CHECK(has(a, "\"name\":\"here\",\"anchorId\":\"cp_9_109\""));
    o = mon("ringdump /Usb0/a.c64rering 4 5");
    CHECK(has(o, "ringdump: 40 anchor(s)") && !strcmp(F.ring_path, "/Usb0/a.c64rering"));
    o = mon("dump /Usb0/b.c64re");
    CHECK(has(o, "dump: /Usb0/b.c64re  (1234 bytes)") && has(o, "Frame 3"));
    /* a run from a land cuts the frames after it */
    o = mon("g");
    CHECK(F.ring_commits == 1 && F.ring_n == 3 && F.running);
    mon("pause");
    o = mon("frame");
    CHECK(has(o, "CUT     \xe2\x96\xb6  frame 3/3   now  (-7 anchors)"));
    o = mon("unmark here");
    CHECK(has(o, "UNMARK here"));
    o = mon("undump /Usb0/b.c64re");
    CHECK(has(o, "undump: /Usb0/b.c64re loaded") && F.running);
    /* T66 part 3: ringload takes the entry to run from; without one, the file's cursor */
    mon("pause");
    o = mon("ringload /Usb0/a.c64rering cp_4_104");
    CHECK(has(o, "ringload: restored 1 anchor(s)") && !strcmp(F.ring_entry, "cp_4_104") && F.running);
    mon("pause");
    o = mon("ringload /Usb0/a.c64rering");
    CHECK(has(o, "ringload: restored 1 anchor(s)") && !F.ring_entry[0] && F.running);
    mon("pause");
    o = mon("ringload /Usb0/a.c64rering #2");
    CHECK(!strcmp(F.ring_entry, "#2") && F.running);
    /* T66 part 6: the ring itself, and the word that says it came back whole */
    mon("pause");
    F.ring_native = 0;
    o = mon("ringsave /Usb0/a.trxring");
    CHECK(has(o, "ringsave: the ring \xe2\x86\x92 /Usb0/a.trxring") && has(o, "4242424 bytes") &&
          F.ring_native == 1 && !strcmp(F.ring_path, "/Usb0/a.trxring"));
    o = mon("ringsave");
    CHECK(has(o, "usage: ringsave <path.trxring>"));
    mon("pause");
    o = mon("ringload /Usb0/a.trxring");
    CHECK(has(o, "ringload: the ring is back from /Usb0/a.trxring") && has(o, "rewinding works again") &&
          F.running);
    mon("pause");
    o = mon("ringload /Usb0/a.c64rering");
    CHECK(has(o, "restored 1 anchor(s)") && !has(o, "the ring is back"));
    a = call("{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"ringbuffer/restore\",\"params\":"
             "{\"path\":\"/Usb0/a.c64rering\",\"entry\":\"here\"}}");
    CHECK(has(a, "\"runState\":\"running\"") && !strcmp(F.ring_entry, "here"));
    F.ring_native = 0;
    a = call("{\"jsonrpc\":\"2.0\",\"id\":8,\"method\":\"ringbuffer/save\",\"params\":"
             "{\"path\":\"/Usb0/b.trxring\"}}");
    CHECK(has(a, "\"kind\":\"trxring\"") && has(a, "\"fileBytes\":4242424") && F.ring_native == 1 &&
          !strcmp(F.ring_path, "/Usb0/b.trxring"));
    a = call("{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"ringbuffer/save\",\"params\":{}}");
    CHECK(has(a, "path required"));
    o = mon("play");
    CHECK(has(o, "not available on this host"));
    F.ring_n = 0;
}

/* A08: while the firmware says the REU is on there is no ring: every way into it answers with the
 * message, nothing lands, and when the REU goes off the ring works again */
static void test_ring_reu(void)
{
    boot();
    F.ring_n = 10;
    F.ring_c0 = 5000000;
    F.cycles = F.ring_c0 + 9 * 19656 + 1000;
    F.ring_reu_off = 1;
    const char *msg = "Timewarp is off while the REU is enabled (A08)";
    int lands = F.ring_lands;
    const char *o = mon("frame -3");
    CHECK(has(o, msg));
    o = mon("goto 3");
    CHECK(has(o, msg));
    o = mon("mark here");
    CHECK(has(o, msg));
    o = mon("rstep 1");
    CHECK(has(o, msg));
    o = mon("rewind");
    CHECK(has(o, msg));
    static const char *rpcs[] = {
        "checkpoint/list", "checkpoint/capture", "checkpoint/restore", "transport/status", "transport/frame",
        "transport/goto", "mark/set", "mark/list", "mark/goto", "runtime/reverse_step", 0
    };
    for (int i = 0; rpcs[i]; i++) {
        char req[160];
        snprintf(req, sizeof(req),
                 "{\"jsonrpc\":\"2.0\",\"id\":%d,\"method\":\"%s\",\"params\":{\"id\":\"cp_4_104\",\"name\":\"x\",\"delta\":-1,\"target\":\"3\"}}",
                 i + 1, rpcs[i]);
        const char *a = call(req);
        CHECK(has(a, "-32001") && has(a, msg));
    }
    CHECK(F.ring_lands == lands && F.ring_landed < 0);
    /* the screen says why the ring is off */
    CHECK(ring_off_reason() != 0);
    F.ring_reu_off = 0;
    CHECK(ring_off_reason() == 0);
    o = mon("frame -3");
    CHECK(has(o, "frame 7/10") && F.ring_landed == 6);
    const char *a = call("{\"jsonrpc\":\"2.0\",\"id\":50,\"method\":\"transport/status\"}");
    CHECK(!has(a, "A08"));
    mon("g"); /* ends the land like test_ring does */
    mon("pause");
    F.ring_n = 0;
}

/* ---- the lenses (trx_lens.c) over the fake lens unit ------------------------- */

static void test_lens(void)
{
    fake_reset();
    for (int i = 0; i < 0x2000; i++) {
        F.cart_roml[i] = (uint8_t)(i + 1);
        F.cart_romh[i] = (uint8_t)(i + 2);
    }
    F.cart_io[0] = 0x5A;
    F.cart_io[1] = 0xA5;
    F.ram[0x8000] = 0x11;
    F.ram[0xA000] = 0x22;
    F.has_lk = 1;
    F.cart = 1;
    F.game = 1;
    F.exrom = 0; /* 8K: ROML at $8000 when LORAM and HIRAM are set */
    mach_snapshot();
    /* cart: ROML/ROMH whatever the PLA maps; the address picks A12-A0 */
    CHECK(mach_peek("cart", 0x8000, 0) == 1);
    CHECK(mach_peek("cart", 0x9FFF, 0) == (int)(uint8_t)(0x1FFF + 1));
    CHECK(mach_peek("cart", 0xA002, 0) == 4);
    CHECK(mach_peek("cart", 0xE003, 0) == 5);
    CHECK(mach_peek("cart", 0xC000, 0) == 0x00); /* RAM */
    /* cpu: the PLA says ROML at $8000, RAM at $A000 (8K cartridge: GAME high) */
    CHECK(mach_peek("cpu", 0x8001, 0) == 2);
    CHECK(mach_peek("cpu", 0xA000, 0) == F.rom[0x4000]); /* BASIC */
    F.port = 0x34; /* RAM everywhere */
    mach_snapshot();
    CHECK(mach_peek("cpu", 0x8000, 0) == 0x11);
    CHECK(mach_peek("cart", 0x8000, 0) == 1); /* the cart lens does not care */
    F.port = 0x37;
    /* 16K: ROMH at $A000 */
    F.game = 0;
    mach_snapshot();
    CHECK(mach_peek("cpu", 0xA001, 0) == 3);
    /* io: IO1/IO2 through the look, VIC from the state bus */
    CHECK(mach_peek("io", 0xDE00, 0) == 0x5A);
    CHECK(mach_peek("io", 0xDF01, 0) == 0xA5);
    CHECK(mach_peek("io", 0xD400, 0) == -1); /* SID */
    int calls = F.lk_calls;
    int bc = F.bus_cycles;
    CHECK(mach_peek("cart", 0x8000, 0) == 1 && F.lk_calls == calls + 1 && F.bus_cycles == bc); /* a look: no cycle */
    /* the real port: the look says ext, a REAL cycle follows while the machine is paused */
    F.cart_ext = 1;
    bc = F.bus_cycles;
    CHECK(mach_peek("cart", 0x8000, 0) == 1 && F.bus_cycles == bc + 1);
    CHECK(mach_peek("io", 0xDE00, 0) == -1); /* IO1 of a real cart: only with sidefx */
    CHECK(mach_peek("io", 0xDE00, 1) == 0x5A);
    CHECK(mach_peek("io", 0xD400, 1) == 0x5A); /* SID with sidefx: a REAL cycle of class I/O (the fake answers io[a & $FF]) */
    F.shut = 0; /* the machine runs: REAL is refused, the byte is not readable */
    CHECK(mach_peek("cart", 0x8000, 0) == -1);
    CHECK(mach_peek8("cart", 0x8000) == 0);
    CHECK(mach_peek("cpu", 0x8000, 0) == -1);
    CHECK(mach_peek("ram", 0x8000, 0) == 0x11); /* RAM needs no pause */
    F.shut = 1;
    F.cart_ext = 0;
    /* a bitstream without the lens unit: the cartridge cannot be read */
    F.has_lk = 0;
    CHECK(mach_peek("cart", 0x8000, 0) == -1);
    CHECK(mach_peek("io", 0xDE00, 0) == -1);
    /* no cartridge: $FF */
    F.has_lk = 1;
    F.cart = 0;
    F.game = 1;
    F.exrom = 1;
    mach_snapshot();
    CHECK(mach_peek("cart", 0x8000, 0) == 0xFF);
    /* the block read the firmware's REST path uses: wraps at $FFFF, unreadable bytes are 0 */
    F.cart = 1;
    F.game = 1;
    F.exrom = 0;
    uint8_t buf[4] = {9, 9, 9, 9};
    CHECK(lens_read(&(lens_hw_t){hw_sb_block, hw_ram_read, hw_rom, hw_drive_peek, hw_lk, 0, 0, 0}, "cart", 0x8000, buf, 4, 0) == 0);
    CHECK(buf[0] == 1 && buf[1] == 2 && buf[3] == 4);

    /* the colour RAM: unknown without a bus cycle, read through the shadow session when one runs
     * (a debugger stop is one, T12 §14.8) */
    boot();
    F.shut = 1;
    F.cram[5] = 0x07;
    mach_snapshot();
    CHECK(mach_peek("io", 0xD805, 0) == -1);
    F.session = 1;
    CHECK(mach_peek("io", 0xD805, 0) == 0xF7);
    CHECK(mach_peek("cpu", 0xD805, 0) == 0xF7); /* the PLA says I/O */
    CHECK(mach_peek("io", 0xD400, 0) == -1);    /* SID: still unknown */
    F.session = 0;
    CHECK(mach_peek("io", 0xD805, 0) == -1);

    /* U13: a bitstream whose look covers the I/O page (CAPS3 b1): every byte from the look, no bus cycle, the turbo
     * register too; no read view (a socket chip) is unreadable without sidefx, a REAL cycle with it */
    boot();
    F.shut = 1;
    F.has_lk = 1;
    F.has_iolook = 1;
    F.io_view[0x031] = 0x0E;
    F.io_view[0x41B] = 0x99;
    F.io_view[0x805] = 0xB7;
    F.io_view[0xF1E] = 0x55;
    F.io_ext[0x500] = 1;
    F.cart_io[0x00] = 0x42;
    mach_snapshot();
    int bc3 = F.bus_cycles;
    CHECK(mach_peek("io", 0xD031, 0) == 0x0E);
    CHECK(mach_peek("io", 0xD41B, 0) == 0x99);
    CHECK(mach_peek("io", 0xD805, 0) == 0xB7);
    CHECK(mach_peek("io", 0xDF1E, 0) == 0x55);
    CHECK(mach_peek("cpu", 0xD031, 0) == 0x0E); /* the PLA says I/O */
    CHECK(F.bus_cycles == bc3);
    CHECK(mach_peek("io", 0xD500, 0) == -1);    /* a socket chip: no read view */
    CHECK(mach_peek("io", 0xD500, 1) == 0x42 && F.bus_cycles == bc3 + 1); /* sidefx: the bus cycle */
    F.has_iolook = 0;
    mach_snapshot();
    CHECK(mach_peek("io", 0xD031, 0) == 0xFF);  /* an older bitstream: the state bus's VIC, $D030-$D03F $FF */
    F.shut = 0;

    /* a debugger stop holds the machine without the menu's C64_STOP: the window's D page is open
     * (ultimate_sys, dbg_stop); without either the byte is unreadable and an I/O write is refused */
    boot();
    F.shut = 1;
    F.fw_stop = 0;
    F.dbg_stop = 1;
    F.session = 1;
    F.cram[5] = 0x07;
    mach_snapshot();
    CHECK(mach_peek("io", 0xD805, 0) == 0xF7);
    int bc2 = F.bus_cycles;
    CHECK(mach_peek("io", 0xDE00, 1) != -1 && F.bus_cycles == bc2 + 1); /* sidefx: a bus cycle, allowed */
    uint8_t col = 0x05;
    CHECK(mach_write("io", 0xD020, &col, 1) == 0 && F.vic[0x20] == 0x05);
    F.dbg_stop = 0;
    col = 0x06;
    CHECK(mach_write("io", 0xD020, &col, 1) != 0 && F.vic[0x20] == 0x05);
    CHECK(mach_peek("io", 0xD805, 0) == -1);
    CHECK(mach_peek("io", 0xDE00, 1) == -1);
    F.fw_stop = 1;
    CHECK(mach_write("io", 0xD020, &col, 1) == 0 && F.vic[0x20] == 0x06); /* the menu's C64_STOP as before */
    F.session = 0;
}

/* ---- the instruction trace (T21 §4.11): chis, whowrote, triage, tracering, rstep from the trace --------- */

static void test_trace(void)
{
    boot();
    F.ring_n = 10;
    F.ring_c0 = 5000000;
    F.cycles = F.ring_c0 + 9 * 19656 + 1000;
    F.trace_present = 1;
    F.trace_on = 1;
    F.trace_replay = 1;
    /* 100 instructions, 3 cycles apart, from 150000: LDA #n at $C100+j, the one at j = 40 also stores $0400 */
    for (int j = 0; j < 100; j++) {
        static const uint8_t wr[1][4] = { { 0x00, 0x04, 0x20, 0x41 } };
        fake_trace_add(150000 + 3u * (uint32_t)j, (uint16_t)(0xC100 + j * 2), 0xA9, (uint8_t)j, 0x77, (uint8_t)j, 0, 0, 0xF6,
                       0x24, j == 40 ? 1 : 0, j == 40 ? wr : 0);
    }
    const char *o = mon("trace");
    CHECK(has(o, "trace: ON") && has(o, "100 of 349461 instructions"));
    o = mon("trace off");
    CHECK(has(o, "trace off") && !F.trace_on);
    o = mon("trace on");
    CHECK(F.trace_on);
    /* chis: reported cycles are the ring's + 5000000 here (the machine's CYCLES run from ring_c0) */
    o = mon("chis 5150000 5150012");
    CHECK(has(o, "# cpuhistory (instruction trace) \xe2\x80\x94 cycles 5150000\xe2\x80\x93" "5150012, 5 instr"));
    CHECK(has(o, "c5150000    $c100  a9 00     LDA #$00"));
    CHECK(has(o, "c5150003    $c102  a9 01     LDA #$01       a=02"));
    /* registers after an instruction are the pre-state of the next record */
    CHECK(has(o, "a=01 x=00 y=00 sp=f6 p=24"));
    o = mon("chis 12");
    CHECK(has(o, "last 12 cycles, 5 instr"));
    CHECK(has(o, "a=63 x=00 y=00 sp=f6 p=24"));
    /* the newest record has no successor: the live registers (the machine stands paused at an IB) */
    CHECK(has(o, "$c1c6  a9 63     LDA #$63"));
    /* whowrote */
    o = mon("whowrote 0400");
    CHECK(has(o, "whowrote $0400: 1 writer(s) in the live ring (newest first):"));
    CHECK(has(o, "<- written by $c150 @ cyc 5150120  ($20 -> $41)"));
    o = mon("whowrote 0401");
    CHECK(has(o, "whowrote $0401: no writer in the live delta ring"));
    /* an I/O address is not in the RAM write log: found from the store instructions of the trace */
    F.trace_n = 0;
    F.writes_n = 0;
    fake_trace_add(170000, 0xC000, 0x8D, 0x20, 0xD0, 5, 0, 0, 0xF6, 0x24, 0, 0);    /* STA $D020, A = 5 */
    fake_trace_add(170010, 0xC010, 0xEE, 0x20, 0xD0, 5, 0, 0, 0xF6, 0x24, 0, 0);    /* INC $D020 */
    fake_trace_add(170020, 0xC020, 0x9D, 0x00, 0xD0, 7, 0x20, 0, 0xF6, 0x24, 0, 0); /* STA $D000,X, X = $20 */
    fake_trace_add(170030, 0xC030, 0xAD, 0x20, 0xD0, 9, 0, 0, 0xF6, 0x24, 0, 0);    /* LDA $D020: a read */
    fake_trace_add(170040, 0xC040, 0x8E, 0x21, 0xD0, 9, 3, 0, 0xF6, 0x24, 0, 0);    /* STX $D021, X = 3 */
    o = mon("whowrote d020");
    CHECK(has(o, "whowrote $d020: 3 writer(s) in the live ring (newest first):"));
    CHECK(has(o, "<- written by $c020 @ cyc 5170020  ($?? -> $07)"));
    CHECK(has(o, "<- written by $c010 @ cyc 5170010  ($?? -> $?" "?)"));
    CHECK(has(o, "<- written by $c000 @ cyc 5170000  ($?? -> $05)"));
    CHECK(!has(o, "zero page pointer") && !has(o, "ring_exhausted"));
    o = mon("whowrote d021");
    CHECK(has(o, "whowrote $d021: 1 writer(s)") && has(o, "$c040 @ cyc 5170040  ($?? -> $03)"));
    /* a store through a zero page pointer: not resolvable from the record, said, never guessed */
    fake_trace_add(170050, 0xC050, 0x91, 0xFB, 0, 1, 0, 0, 0xF6, 0x24, 0, 0);        /* STA ($FB),Y */
    o = mon("whowrote d022");
    CHECK(has(o, "whowrote $d022: no writer") && has(o, "stores through a zero page pointer"));
    o = mon("whowrote d020");
    CHECK(has(o, "3 writer(s)") && has(o, "stores through a zero page pointer"));
    o = mon("whowrote d020 1");
    CHECK(has(o, "1 writer(s)") && has(o, "$c020"));
    /* a transfer into the wild: JSR $E000 at $C300, RTS at $E005 popped a smashed return address */
    F.trace_n = 0;
    F.writes_n = 0;
    {
        static const uint8_t st[2][4] = { { 0xF5, 0x01, 0x12, 0x34 }, { 0xF6, 0x01, 0xC3, 0x08 } };
        fake_trace_add(160000, 0xC2F0, 0xA9, 1, 0, 1, 2, 3, 0xF6, 0x24, 2, st);
        fake_trace_add(160010, 0xC300, 0x20, 0x00, 0xE0, 1, 2, 3, 0xF6, 0x24, 0, 0);
        fake_trace_add(160020, 0xE000, 0xEA, 0, 0, 1, 2, 3, 0xF4, 0x24, 0, 0);
        fake_trace_add(160030, 0xE005, 0x60, 0, 0, 1, 2, 3, 0xF4, 0x24, 0, 0);
        for (int i = 0; i < 4; i++) {
            fake_trace_add(160040 + 3u * (uint32_t)i, 0x0835, 0x02, 0, 0, 1, 2, 3, 0xF6, 0x24, 0, 0);
        }
    }
    F.ram[0x835] = 0x02;
    F.ram[0x1F5] = 0x34;
    F.ram[0x1F6] = 0x08;
    o = mon("triage 0835");
    CHECK(has(o, "crash triage (reverse-debug Phase 2)"));
    CHECK(has(o, "JAM @ $0835 (op $02) \xe2\x86\x90 wild RTS @ $E005 popped $0834 from $01F5"));
    CHECK(has(o, "lead-in:  $C2F0 \xe2\x86\x92 $C300 \xe2\x86\x92 $E000 \xe2\x86\x92 $E005 \xe2\x86\x92 $0835"));
    CHECK(has(o, "transfer: RTS @ $E005 \xe2\x86\x92 $0835  [certain]"));
    CHECK(has(o, "slot $01F5=$34  \xe2\x86\x90 written by $C2F0 @ cyc 5160000 ($12\xe2\x86\x92" "$34)  [certain]"));
    CHECK(has(o, "corruptor PINNED"));
    const char *a = call("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"runtime/crash_triage\",\"params\":{\"pc\":2101}}");
    CHECK(has(a, "\"kind\":\"RTS\"") && has(a, "\"pinnedCorruptor\":true") && has(a, "\"atPc\":57349"));
    a = call("{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"runtime/who_wrote\",\"params\":{\"addr\":501,\"limit\":4}}");
    CHECK(has(a, "\"writers\":[{\"pc\":49904,\"cycle\":5160000,\"addr\":501,\"old\":18,\"new\":52,\"callerChain\":[]}]"));
    CHECK(has(a, "\"ringExhausted\":false"));
    /* depth */
    o = mon("revdepth");
    CHECK(has(o, "revdepth: 1s (~8.0 MB)") && has(o, "8 of 349461 instructions"));
    o = mon("revdepth 30");
    CHECK(has(o, "fixed at 1 s"));
    a = call("{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"runtime/set_reverse_depth\",\"params\":{\"seconds\":30}}");
    CHECK(has(a, "\"seconds\":1") && has(a, "\"discardedHistory\":false"));
    /* tracering: a .c64retrace of the window (reported cycles), as trace/build_from_ring writes it */
    mkdir("/tmp/trxmon_test", 0755);
    o = mon("tracering 5160000 5160030 /t.c64retrace");
    CHECK(has(o, "TRACE FROM RING") && has(o, "\"instruction_count\":4") && has(o, "\"event_count\":6"));
    CHECK(has(o, "\"retrace_path\":\"/t.c64retrace\"") && has(o, "\"duckdb_path\":\"/t.duckdb\""));
    CHECK(has(o, "\"run_id\":\"run_ring-dump_5160000_5160030\""));
    {
        FILE *f = fopen("/tmp/trxmon_test/t.c64retrace", "rb");
        CHECK(f != NULL);
        if (f) {
            static uint8_t fb[4096];
            size_t n = fread(fb, 1, sizeof(fb), f);
            fclose(f);
            CHECK(!memcmp(fb, "C64RETR1", 8) && fb[8] == 2 && fb[9] == 0);
            uint32_t ml = fb[12] | (fb[13] << 8) | (fb[14] << 16) | ((uint32_t)fb[15] << 24);
            CHECK(n == 16u + ml + 4 * 19 + 2 * 16);
            const uint8_t *e = fb + 16 + ml;
            double cyc;
            memcpy(&cyc, e + 1, 8);
            /* CPU_STEP: op 0x10, cycle f64, pc, opcode, a x y sp p, b1 b2 */
            CHECK(e[0] == 0x10 && cyc == 5160000.0 && e[9] == 0xF0 && e[10] == 0xC2 && e[11] == 0xA9);
            CHECK(e[12] == 1 && e[13] == 2 && e[14] == 3 && e[15] == 0xF6 && e[16] == 0x24 && e[17] == 1 && e[18] == 0);
            /* RAM_WRITE: op 0x11, cycle, addr $01F5, new $34, pc, access (write, has old), old $12 */
            const uint8_t *w = e + 19;
            CHECK(w[0] == 0x11 && w[9] == 0xF5 && w[10] == 0x01 && w[11] == 0x34 && w[12] == 0xF0 && w[13] == 0xC2 &&
                  w[14] == 0x81 && w[15] == 0x12);
        }
    }
    a = call("{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"trace/build_from_ring\",\"params\":{\"cycle_start\":5160030,"
             "\"cycle_end\":5159000,\"output_path\":\"/u.c64retrace\"}}");
    CHECK(has(a, "\"instruction_count\":4") && has(a, "\"cycle_start\":5159000") && has(a, "\"ring_cycle_span\""));
    CHECK(has(a, "\"clipped\":true"));

    /* rstep per instruction from the trace */
    F.trace_n = 0;
    F.writes_n = 0;
    for (int j = 0; j < 100; j++) {
        fake_trace_add(150000 + 3u * (uint32_t)j, (uint16_t)(0xC100 + j), 0xA9, (uint8_t)j, 0, (uint8_t)j, 0, 0, 0xF6, 0x24, 0, 0);
    }
    F.cycles = F.ring_c0 + 9 * 19656 + 1000;
    F.pc = 0xC400;
    o = mon("rstep 5");
    CHECK(has(o, "reverse-step: undid 5 instruction(s)"));
    CHECK(has(o, "landed @ $c15f"));
    CHECK(has(o, "A=$5f") && F.ring_landed == 7);
    CHECK(has(o, "replayed from the start of Frame 8"));
    /* a trace that holds fewer records than asked falls back to the replay-and-count */
    int s0 = sleeps;
    o = mon("rstep 500");
    CHECK(has(o, "the ring does not reach that far back") && !strstr(o, "undid 500"));
    /* T77: the long replay chains many waits; it must sleep now and then so the network tasks below run */
    CHECK(sleeps > s0);
    F.trace_present = 0;
    F.ring_n = 0;
}

/* ---- the video mode (T40) ---------------------------------------------------------- */

static void test_video(void)
{
    boot();
    const char *a = call("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"session/create\"}");
    CHECK(has(a, "\"model\":\"c64-pal\"") && has(a, "\"cyclesPerFrame\":19656") && has(a, "\"linesPerFrame\":312"));
    F.hz60 = 1;
    a = call("{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"session/create\"}");
    CHECK(has(a, "\"model\":\"c64-ntsc\"") && has(a, "\"cyclesPerFrame\":17095") && has(a, "\"linesPerFrame\":263") &&
          has(a, "\"cyclesPerLine\":65") && has(a, "\"cpuHz\":1022727") && has(a, "\"frameRate\":59.83"));
    const char *o = mon("model");
    CHECK(has(o, "c64-ntsc  NTSC  VIC-II 6567 (kawari)  65 x 263 = 17095 cycles/frame  1022727 Hz  59.83 fps"));
    /* the ring's frame arithmetic follows the mode: 10 entries, "continuous (every 17095 cyc)" is the ring's line */
    F.ring_n = 10;
    F.ring_c0 = 5000000;
    F.cycles = F.ring_c0 + 9 * 19656 + 1000;
    o = mon("rewind");
    CHECK(has(o, "expected 17095") || has(o, "every 17095 cyc"));
    F.ring_n = 0;
    F.hz60 = 0;
}


/* ---- the monitor screen (ui.c, T21 §8) ---------------------------------------- */

static ui_t U;

static void type(const char *t)
{
    for (; *t; t++) {
        ui_key(&U, *t == '\n' ? UI_K_RETURN : *t);
    }
}

/* the screen as text: the 23 rows, a cursor mark line, nothing else */
static char scr_txt[UI_ROWS_MAX * (UI_COLS_MAX + 3) + 64];
static const char *screen_text(void)
{
    static ui_screen_t sc;
    ui_render(&U, &sc);
    char *o = scr_txt;
    for (int y = 0; y < sc.rows; y++) {
        int n = sc.cols;
        while (n && sc.row[y][n - 1] == ' ') {
            n--;
        }
        memcpy(o, sc.row[y], (size_t)n);
        o += n;
        *o++ = '\n';
    }
    o += snprintf(o, 64, "cursor %d,%d blank %d", sc.cursor_x, sc.cursor_y, sc.blank_rows);
    return scr_txt;
}

static const char *last_line(int back);

static void ui_boot(void)
{
    boot();
    F.running = 0;
    F.shut = 1;
    ui_init(&U);
    ui_open(&U);
}

/* the reopen wait (main.c ui_callback: the serve loop parks, the screen runs, the loop is let go) */
static volatile int w_flag, w_alive;
static void hook_set_at3(int n)
{
    if (n == 3) {
        w_flag = 1;
    }
}
static void hook_kill_at2(int n)
{
    if (n == 2) {
        w_alive = 0;
    }
}
static void hook_clear_at2(int n)
{
    if (n == 2) {
        w_flag = 0;
    }
}
static void test_wait(void)
{
    /* the flag comes after three sleeps: returns 1, sleeps only for real (never a zero or a bare tick) */
    w_flag = 0;
    w_alive = 1;
    sleeps = 0;
    sleep_min = 0;
    sleep_hook = hook_set_at3;
    CHECK(plat_wait_flag(&w_flag, 1, &w_alive, 1000) == 1);
    CHECK(sleeps == 3);
    CHECK(sleep_min >= PLAT_SLEEP_MIN_MS);
    /* already there: no sleep at all */
    sleeps = 0;
    CHECK(plat_wait_flag(&w_flag, 1, &w_alive, 1000) == 1 && sleeps == 0);
    /* never comes: gives up at the timeout, not before, and after few sleeps (the sleeps are long) */
    w_flag = 0;
    sleeps = 0;
    sleep_hook = 0;
    uint32_t t0 = F.ms;
    CHECK(plat_wait_flag(&w_flag, 1, &w_alive, 1000) == 0);
    CHECK((uint32_t)(F.ms - t0) >= 1000);
    CHECK(sleeps >= 2 && sleeps <= 1000 / PLAT_SLEEP_MIN_MS + 1);
    /* the serve loop ends meanwhile: stops at once, no timeout needed */
    sleeps = 0;
    sleep_hook = hook_kill_at2;
    CHECK(plat_wait_flag(&w_flag, 1, &w_alive, 0) == 0);
    CHECK(sleeps == 2);
    /* waiting for a flag to drop (the loop leaves the park): want 0, no alive flag, no timeout */
    w_flag = 1;
    w_alive = 1;
    sleeps = 0;
    sleep_hook = hook_clear_at2;
    CHECK(plat_wait_flag(&w_flag, 0, 0, 0) == 1 && sleeps == 2);
    sleep_hook = 0;
}

static void test_ui(void)
{
    /* the line editor: typing, DEL, left arrow, history */
    ui_boot();
    type("d c0");
    CHECK_STR(U.cmd, "d c0");
    ui_key(&U, UI_K_BACK);
    ui_key(&U, UI_K_BACK);
    CHECK_STR(U.cmd, "d ");
    ui_key(&U, UI_K_KILL);
    CHECK_STR(U.cmd, "");
    ui_key(&U, UI_K_BACK); /* on an empty line: nothing */
    CHECK(U.clen == 0);
    ui_key(&U, 0x01); /* a control code is not text */
    ui_key(&U, 0x91);
    CHECK(U.clen == 0);
    type("r\n");
    type("m c000 c003\n");
    type("m c000 c003\n"); /* a repeat is kept once */
    CHECK(U.cn == 2);
    ui_key(&U, UI_K_UP);
    CHECK_STR(U.cmd, "m c000 c003");
    ui_key(&U, UI_K_UP);
    CHECK_STR(U.cmd, "r");
    ui_key(&U, UI_K_UP);
    CHECK_STR(U.cmd, "r"); /* the oldest stays */
    ui_key(&U, UI_K_DOWN);
    CHECK_STR(U.cmd, "m c000 c003");
    ui_key(&U, UI_K_DOWN);
    CHECK_STR(U.cmd, "");
    ui_key(&U, UI_K_DOWN);
    CHECK_STR(U.cmd, "");
    for (int i = 0; i < 40; i++) {
        char l[16];
        snprintf(l, sizeof(l), "m %04x\n", 0xC100 + i);
        type(l);
    }
    CHECK(U.cn == UI_CHIST);
    ui_key(&U, UI_K_UP);
    CHECK_STR(U.cmd, "m c127");
    for (int i = 0; i < 40; i++) {
        ui_key(&U, UI_K_UP);
    }
    CHECK_STR(U.cmd, "m c108"); /* 32 entries back */
    for (int i = 0; i < UI_CMD + 20; i++) {
        ui_key(&U, 'a');
    }
    CHECK(U.clen == UI_CMD);

    /* the key map: the firmware's names (PageUp, PageDown, Help), whichever function key an interface gives them;
     * Tasks and the other function keys are ignored */
    CHECK(ui_map_key(0x92) == UI_K_PGUP && ui_map_key(0x95) == UI_K_PGDN && ui_map_key(0x1FB) == UI_K_HELP);
    CHECK(ui_map_key(0x1F8) == UI_K_COMPLETE && ui_map_key(0x09) == UI_K_COMPLETE); /* CTRL alone, CTRL+I / TAB */
    CHECK(ui_map_key(0x1FC) == 0 && ui_map_key(0x1FA) == 0 && ui_map_key(0x1F9) == 0 && ui_map_key(0x1FD) == 0);
    CHECK(ui_map_key(0x0D) == UI_K_RETURN && ui_map_key(0x08) == UI_K_BACK && ui_map_key(0x7F) == UI_K_BACK);
    CHECK(ui_map_key(0x60) == UI_K_KILL && ui_map_key(0x11) == UI_K_STOP);
    CHECK(ui_map_key(0x91) == UI_K_UP && ui_map_key(0x12) == UI_K_DOWN);
    CHECK(ui_map_key('d') == 'd' && ui_map_key('$') == '$' && ui_map_key(0x1B) == 0 && ui_map_key(-1) == 0);
    CHECK(ui_map_key('w') == 'w' && ui_map_key('a') == 'a' && ui_map_key('s') == 's');
    /* which function keys: ui->fkey(1..8) says (PageUp CONFIG Help SYSINFO Tasks SEARCH PageDown / Tasks CONFIG PageUp SYSINFO PageDown SEARCH Help) */
    {
        static const int ult[9] = { 0, 0x92, 0x1FA, 0x1FB, 0x1F9, 0x1FC, 0x1FD, 0x95, 0x1FE };
        static const int com[9] = { 0, 0x1FC, 0x1FA, 0x92, 0x1F9, 0x95, 0x1FD, 0x1FB, 0x1FE };
        ui_t k;
        ui_init(&k);
        CHECK(k.fk_up == 1 && k.fk_down == 7 && k.fk_help == 3);
        ui_set_keys(&k, ult);
        CHECK(k.fk_up == 1 && k.fk_down == 7 && k.fk_help == 3);
        ui_set_keys(&k, com);
        CHECK(k.fk_up == 3 && k.fk_down == 5 && k.fk_help == 7);
        /* in the Commodore interface the keys it labels PageUp and PageDown (F3, F5) scroll and F7 is help: those
         * give the same codes, so they scroll. (The old map took F3 for help and F5 for nothing.) */
        CHECK(ui_map_key(com[3]) == UI_K_PGUP && ui_map_key(com[5]) == UI_K_PGDN && ui_map_key(com[7]) == UI_K_HELP);
        CHECK(ui_map_key(com[1]) == 0); /* F1 is Tasks there */
        CHECK(ui_map_key(ult[1]) == UI_K_PGUP && ui_map_key(ult[7]) == UI_K_PGDN && ui_map_key(ult[3]) == UI_K_HELP);
        CHECK(ui_map_key(ult[5]) == 0);
    }

    /* output: wrapped at 40, marks dropped, UTF-8 and control bytes replaced */
    ui_boot();
    ui_print(&U, "0123456789012345678901234567890123456789ABCD\n\x01" "1,,c000,0001\x02$c000\x03 x\xe2\x80\x94y\x07", UA_NORMAL);
    CHECK(U.hn == 3);
    CHECK_STR(U.hist[(U.hhead + UI_HIST - 3) % UI_HIST], "0123456789012345678901234567890123456789");
    CHECK_STR(U.hist[(U.hhead + UI_HIST - 2) % UI_HIST], "ABCD");
    ui_boot();
    ui_print(&U, "aaaa bbbb cccc dddd eeee ffff gggg hhhh iiii jjjj", UA_NORMAL);
    CHECK_STR(U.hist[(U.hhead + UI_HIST - 2) % UI_HIST], "aaaa bbbb cccc dddd eeee ffff gggg hhhh");
    CHECK_STR(U.hist[(U.hhead + UI_HIST - 1) % UI_HIST], "iiii jjjj");
    ui_boot();
    ui_print(&U, "0123456789012345678901234567890123456789ABCD\n\x01" "1,,c000,0001\x02$c000\x03 x\xe2\x80\x94y\x07", UA_NORMAL);
    CHECK_STR(U.hist[(U.hhead + UI_HIST - 1) % UI_HIST], "$c000 x-y.");
    for (int i = 0; i < 300; i++) {
        char l[16];
        snprintf(l, sizeof(l), "line %d", i);
        ui_print(&U, l, UA_NORMAL);
    }
    CHECK(U.hn == UI_HIST);
    CHECK_STR(U.hist[(U.hhead + UI_HIST - 1) % UI_HIST], "line 299");
    CHECK_STR(U.hist[U.hhead], "line 100"); /* the oldest of 200 */

    /* namespaces: bare = monitor, / = machine, TRX64's own error text for what is not there */
    ui_boot();
    type("/bogus\n");
    CHECK_STR(U.hist[(U.hhead + UI_HIST - 1) % UI_HIST], "unknown command: /bogus. Try 'help'.");
    CHECK(U.hattr[(U.hhead + UI_HIST - 1) % UI_HIST] == UA_ERR);
    type("!ls\n");
    CHECK(strstr(U.hist[(U.hhead + UI_HIST - 1) % UI_HIST], "unknown command: !ls") != NULL);
    type("pwd\n");
    CHECK_STR(U.hist[(U.hhead + UI_HIST - 1) % UI_HIST], "/USB1");
    type("/pause\n");
    CHECK(strstr(U.hist[(U.hhead + UI_HIST - 1) % UI_HIST], "paused at .C:c000") != NULL);
    CHECK(U.leave == UI_STAY);
    type("/quit\n");
    CHECK(U.leave == UI_LEAVE_QUIT);
    mon_init();

    /* leaving with x: the machine resumes */
    ui_boot();
    type("x\n");
    CHECK(U.leave == UI_LEAVE_X && M.running);
    ui_boot();
    type("x 1\n"); /* not `x`: a verb error, no leave (unknown arguments) */
    CHECK(U.leave == UI_STAY || U.leave == UI_LEAVE_X);

    /* narrow m and d */
    ui_boot();
    type("m c000 c01f\n");
    CHECK_STR(U.hist[(U.hhead + UI_HIST - 4) % UI_HIST], ">C:c000 a9 01 20 10 c0 4c 00 c0 .. ..L..");
    ui_boot();
    type("m c000 c003\n");
    {
        char *l = U.hist[(U.hhead + UI_HIST - 1) % UI_HIST];
        CHECK(m_strlen(l) == 3 + 4 + 1 + 23 + 1 + 4);
        CHECK(strncmp(l, ">C:c000 a9 01 20 10", 19) == 0);
    }
    /* the RPC text stays full width: 96 + 2 */
    boot();
    const char *o = mon("m c000 c003");
    CHECK((int)strlen(o) == 3 + 4 + 2 + 96 + 2 + 4);
    o = mon("d c000 c007");
    CHECK_STR(o, "$c000  a9 01     LDA #$01 <-- PC\n$c002  20 10 c0  JSR $c010\n$c005  4c 00 c0  JMP $c000");

    /* d on the screen: the raw bytes go only when the line is too long (a 40-column line fits) */
    ui_boot();
    type("d c000 c007\n");
    CHECK_STR(U.hist[(U.hhead + UI_HIST - 1) % UI_HIST], "$c005  4c 00 c0  JMP $c000");
    /* golden: the paused screen after a few verbs (the 23 rows, trailing blanks cut) */
    ui_boot();
    type("r\nd c000 c007\nm c000 c00f\nbk c010\nz\n");
    CHECK_STR(screen_text(),
        "PAUSED IB $c002 cy 2 r48/012\n"
        "  ADDR AC XR YR SP NV-BDIZC 01\n"
        ".;C002 00 00 00 F6 nv-bdIzc 37\n"
        "  ADDR AC XR YR SP NV-BDIZC  flow\n"
        ".;c000 00 00 00 f6 nv-bdIzc  main\n"
        "  port  $00=$2f $01=$37  LORAM=1\n"
        "HIRAM=1 CHAREN=1\n"
        "  vectors  IRQ hw=$ff48  CINV\n"
        "$0314->$ea31     NMI hw=$fe43  NMIV\n"
        "$0318->$0000\n"
        "  cycles 0  instrs 0\n"
        ".d c000 c007\n"
        "$c000  a9 01     LDA #$01 <-- PC\n"
        "$c002  20 10 c0  JSR $c010\n"
        "$c005  4c 00 c0  JMP $c000\n"
        ".m c000 c00f\n"
        ">C:c000 a9 01 20 10 c0 4c 00 c0 .. ..L..\n"
        ">C:c008 00 00 00 00 00 00 00 00 ........\n"
        ".bk c010\n"
        "bk #1 set at $c010 (1 total)\n"
        ".z\n"
        "$c002  20 10 c0  JSR $c010 (step, 2 cyc)\n"
        ".\n"
        "cursor 1,22 blank 0");
    /* scrolled up a page of a long output: the older lines, the status and the prompt stay */
    type("help\n");
    {
        static char tail[sizeof(scr_txt)];
        snprintf(tail, sizeof(tail), "%s", screen_text());
        CHECK(U.scroll == 0 && strncmp(tail, "PAUSED IB $c002", 15) == 0);
        ui_key(&U, UI_K_PGUP);
        CHECK(U.scroll == 19);
        const char *t = screen_text();
        CHECK(strcmp(t, tail) != 0);
        CHECK(strncmp(t, "PAUSED IB $c002", 15) == 0);
        ui_key(&U, UI_K_PGUP);
        CHECK(U.scroll == 38);
        ui_key(&U, UI_K_PGDN);
        ui_key(&U, UI_K_PGDN);
        CHECK(U.scroll == 0);
        ui_key(&U, UI_K_PGDN);
        CHECK(U.scroll == 0);
        type("a");
        ui_key(&U, UI_K_PGUP);
        type("\x08");
        CHECK(U.scroll == 19); /* typing does not move the view; new output does */
        type("\n");
        type("r\n");
        CHECK(U.scroll == 0);
    }
    /* golden: running: the window is the bottom 6 rows, the rest is blank */
    type("x\n");
    CHECK_STR(screen_text(),
        "\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n"
        "RUNNING\n"
        "registers: running\n"
        "\n"
        "continuing at .C:c002 (running - Pause\n"
        "to halt)\n"
        ".\n"
        "cursor 1,22 blank 17");
    ui_boot();
    /* RETURN on an empty line: assemble mode ends (board 2026-10-07: it could not be left from the screen) */
    type("a c000\n");
    CHECK(mon_asm_mode());
    CHECK_STR(U.cmd, "a c000 "); /* the next prompt is prefilled, VICE style */
    type("\n");
    CHECK(!mon_asm_mode());
    CHECK(U.clen == 0);
    /* `a c100 nop`: the next prompt is `a c101 `; the typed instruction goes there and the prefill moves on */
    type("a c100 nop\n");
    CHECK_STR(U.cmd, "a c101 ");
    CHECK(F.ram[0xC100] == 0xEA);
    type("lda #$01\n");
    CHECK(F.ram[0xC101] == 0xA9 && F.ram[0xC102] == 0x01);
    CHECK_STR(U.cmd, "a c103 ");
    type("\n"); /* nothing typed after the prefill: ends the mode */
    CHECK(!mon_asm_mode() && U.clen == 0);
    /* the cursor moves within the line: typing inserts, BACK deletes before the cursor */
    ui_boot();
    type("m c0a0");
    ui_key(&U, UI_K_LEFT);
    ui_key(&U, UI_K_LEFT);
    CHECK(U.cpos == 4);
    ui_key(&U, '0');
    CHECK_STR(U.cmd, "m c00a0");
    ui_key(&U, UI_K_BACK);
    CHECK_STR(U.cmd, "m c0a0");
    ui_key(&U, UI_K_HOME);
    CHECK(U.cpos == 0);
    ui_key(&U, UI_K_RIGHT);
    ui_key(&U, UI_K_RIGHT);
    ui_key(&U, UI_K_BACK);
    CHECK_STR(U.cmd, "mc0a0");
    ui_key(&U, UI_K_END);
    CHECK(U.cpos == 5);
    ui_key(&U, UI_K_RIGHT);
    CHECK(U.cpos == 5);
    {
        ui_screen_t sc;
        ui_key(&U, UI_K_LEFT);
        ui_render(&U, &sc);
        CHECK(sc.cursor_x == 1 + 4 && sc.cursor_y == sc.rows - 1);
    }
    /* a long line with the cursor moved to its start: the row starts at the cursor and shows what fits */
    ui_key(&U, UI_K_KILL);
    for (int i = 0; i < 100; i++) {
        ui_key(&U, 'a' + (i % 26));
    }
    ui_key(&U, UI_K_HOME);
    {
        ui_screen_t sc;
        ui_render(&U, &sc);
        CHECK(sc.cursor_x == 1 && sc.row[sc.rows - 1][1] == 'a' && sc.row[sc.rows - 1][2] == 'b');
    }
    ui_key(&U, UI_K_KILL);
    /* completion: a verb, the common start of several, an observer name, a lens */
    ui_boot();
    type("rst");
    ui_key(&U, UI_K_COMPLETE);
    CHECK_STR(U.cmd, "rstep ");
    ui_key(&U, UI_K_KILL);
    type("ob");
    ui_key(&U, UI_K_COMPLETE);
    CHECK_STR(U.cmd, "obs ");
    type("flash when exec c000\n");
    type("obs fl");
    ui_key(&U, UI_K_COMPLETE);
    CHECK_STR(U.cmd, "obs flash ");
    ui_key(&U, UI_K_KILL);
    type("m r");
    ui_key(&U, UI_K_COMPLETE);
    CHECK(strncmp(last_line(0), "ram  rom", 8) == 0 && strcmp(U.cmd, "m r") == 0);
    type("a");
    ui_key(&U, UI_K_COMPLETE);
    CHECK_STR(U.cmd, "m ram ");
    ui_key(&U, UI_K_KILL);
    type("cd Su");
    mon("mkdir Sub");
    ui_key(&U, UI_K_COMPLETE);
    CHECK_STR(U.cmd, "cd Sub/");
    mon("rmdir Sub");
    ui_key(&U, UI_K_KILL);
    /* RETURN goes on with the last `d` / `m` from where it stopped (Alex 2026-10-07) */
    type("d c000\n");
    CHECK_STR(U.last_step, "d");
    type("m c000\n");
    CHECK_STR(U.last_step, "m");
    type("\n");
    CHECK_STR(U.last_step, "m");
}


/* ---- the 80 column screen (T21 §8.10) ---------------------------------- */

static void ui_boot_wide(void)
{
    boot();
    F.running = 0;
    F.shut = 1;
    ui_init(&U);
    ui_set_size(&U, UI_WIDE_COLS, UI_WIDE_ROWS);
    ui_open(&U);
}

/* one row of the rendered screen, trailing blanks cut */
static const char *row_of(int y)
{
    static ui_screen_t sc;
    static char r[UI_COLS_MAX + 1];
    ui_render(&U, &sc);
    memcpy(r, sc.row[y], (size_t)sc.cols + 1);
    int n = sc.cols;
    while (n && r[n - 1] == ' ') {
        n--;
    }
    r[n] = 0;
    return r;
}

/* the longest line of a text, span marks not counted */
static int max_line(const char *t)
{
    int mx = 0, cur = 0;
    for (const char *q = t; *q; q++) {
        if (*q == SPAN_START) {
            while (*q && *q != SPAN_SEP) {
                q++;
            }
            if (!*q) {
                break;
            }
        } else if (*q == '\n') {
            mx = cur > mx ? cur : mx;
            cur = 0;
        } else if ((*q != SPAN_SEP) && (*q != SPAN_END)) {
            cur++;
        }
    }
    return cur > mx ? cur : mx;
}

static int line_count(const char *t)
{
    int n = 1;
    for (const char *q = t; *q; q++) {
        n += (*q == '\n');
    }
    return n;
}

static const char *hist_line(int back) { return U.hist[(U.hhead + UI_HIST - 1 - back) % UI_HIST]; }

static void test_ui_wide(void)
{
    /* the window: 80 x 40, the layout of §8.10 */
    ui_boot_wide();
    CHECK(U.cols == 80 && U.rows == 40);
    CHECK(ui_out_rows(&U) == 33); /* 40 less status, registers, device, separator, separator, command, hint */
    type("r\nd c000 c007\nm c000 c00f\nbk c010\nz\n");
    CHECK_STR(row_of(0), "PAUSED IB $c002 cy 2 r48/012");
    CHECK_STR(row_of(1), "PC C002  A 00  X 00  Y 00  SP F6  NV-BDIZC nv-bdIzc");
    CHECK_STR(row_of(2), "$00=2f $01=37  bank cpu  device c64  ring -  trace -");
    CHECK(strlen(row_of(3)) == 80 && strspn(row_of(3), "-") == 80);
    CHECK(strlen(row_of(37)) == 80 && strspn(row_of(37), "-") == 80);
    CHECK_STR(row_of(38), ".");
    CHECK(strncmp(row_of(39), "F1/F7 scroll  F3 help", 21) == 0 && strlen(row_of(39)) <= 79);
    CHECK(strstr(row_of(39), "/quit end") != NULL && strstr(row_of(39), "RET repeats z/n") != NULL);
    /* the output stands at the bottom of its area: the newest line on row 35 */
    CHECK_STR(row_of(36), "$c002  20 10 c0  JSR $c010 (step, 2 cyc)");
    CHECK_STR(row_of(35), ".z");
    CHECK_STR(row_of(34), "bk #1 set at $c010 (1 total)");
    CHECK_STR(row_of(32), ">C:c000  a9 01 20 10 c0 4c 00 c0 00 00 00 00 00 00 00 00  .. ..L..........");
    CHECK_STR(row_of(28), ">C:c000  a9 01     LDA #$01");
    CHECK_STR(row_of(29), ".C:c002  20 10 c0  JSR $c010");
    CHECK_STR(row_of(30), ".C:c005  4c 00 c0  JMP $c000");
    CHECK_STR(row_of(26), "  cycles 0  instrs 0");
    CHECK_STR(row_of(21), ".r");
    {
        static ui_screen_t sc;
        ui_render(&U, &sc);
        CHECK(sc.cols == 80 && sc.rows == 40 && sc.cursor_x == 1 && sc.cursor_y == 38 && sc.blank_rows == 0);
        CHECK(sc.attr[3] == UA_SEP && sc.attr[37] == UA_SEP && sc.attr[39] == UA_HINT && sc.attr[38] == UA_PROMPT);
    }
    /* the device row with a ring and a trace */
    F.ring_n = 10;
    F.trace_present = 1;
    F.trace_on = 1;
    CHECK_STR(row_of(2), "$00=2f $01=37  bank cpu  device c64  ring on 10 fr  trace on 0/349461");
    type("bank ram\ndevice drive8\n");
    CHECK(strstr(row_of(2), "bank ram  device drive8") != NULL);
    type("device c64\nbank cpu\n");

    /* running: the window is the bottom 9 rows, the rest blank */
    type("x\n");
    {
        static ui_screen_t sc;
        ui_render(&U, &sc);
        CHECK(sc.blank_rows == 31 && sc.attr[0] == UA_BLANK && sc.attr[30] == UA_BLANK);
        CHECK_STR(row_of(31), "RUNNING");
        CHECK_STR(row_of(32), "registers: running");
        CHECK(strncmp(row_of(33), "bank cpu  device c64  ring -  trace -", 36) == 0); /* no $00/$01 and no counts while it runs */
        CHECK(strspn(row_of(34), "-") == 80 && strspn(row_of(37), "-") == 80);
        CHECK_STR(row_of(38), ".");
        CHECK(strncmp(row_of(39), "F1/F7", 5) == 0);
        CHECK(sc.cursor_y == 38);
    }
    F.ring_n = 0;
    F.trace_present = 0;

    /* every verb that prints a table fits 80 columns: no line of `help`, `r`, `status`, `bt` wraps */
    ui_boot_wide();
    {
        static const char *verbs[] = { "help", "r", "status", "bt", "m c000", "d c000", "bk" };
        for (unsigned i = 0; i < sizeof(verbs) / sizeof(verbs[0]); i++) {
            static char ob[16384];
            sbuf_t o;
            sb_init(&o, ob, sizeof(ob) - 1);
            rpc_local_exec(verbs[i], &o);
            sb_str(&o);
            CHECK(max_line(ob) <= 80);
        }
        int before = U.hn;
        type("help\n");
        static char hb[16384];
        sbuf_t o;
        sb_init(&o, hb, sizeof(hb) - 1);
        rpc_local_exec("help", &o);
        sb_str(&o);
        CHECK(U.hn - before == 1 + line_count(hb)); /* the echo and one history line per line of text */
    }

    /* wrapping at 80: 79 and 80 characters stay one line, 81 wrap */
    ui_boot_wide();
    {
        char l[100];
        memset(l, 'x', 99);
        l[79] = 0;
        ui_print(&U, l, UA_NORMAL);
        l[79] = 'x';
        l[80] = 0;
        ui_print(&U, l, UA_NORMAL);
        CHECK(U.hn == 2 && m_strlen(hist_line(0)) == 80 && m_strlen(hist_line(1)) == 79);
        l[80] = 'x';
        l[81] = 0;
        ui_print(&U, l, UA_NORMAL);
        CHECK(U.hn == 4 && m_strlen(hist_line(1)) == 80 && m_strlen(hist_line(0)) == 1);
        ui_boot_wide();
        ui_print(&U, "aaaa bbbb cccc dddd eeee ffff gggg hhhh iiii jjjj kkkk llll mmmm nnnn oooo pppp qqqq rrrr ssss", UA_NORMAL);
        CHECK(U.hn == 2);
        CHECK_STR(hist_line(1), "aaaa bbbb cccc dddd eeee ffff gggg hhhh iiii jjjj kkkk llll mmmm nnnn oooo pppp");
        CHECK_STR(hist_line(0), "qqqq rrrr ssss");
    }

    /* m <addr>: one page of 16 bytes + PETSCII that fits the output area; m alone goes on (VICE) */
    ui_boot_wide();
    F.ram[0xC040] = 'A';
    F.ram[0xC041] = 'z' - 32 + 0; /* 0x5A 'Z' */
    F.ram[0xC042] = 0xC1;         /* shifted A */
    F.ram[0xC043] = 0x61;         /* a graphics character */
    type("m c040\n");
    CHECK(U.hn == 1 + 32);
    CHECK_STR(row_of(4), ".m c040");
    CHECK(strncmp(row_of(5), ">C:c040  ", 9) == 0 && strncmp(row_of(36), ">C:c230  ", 9) == 0); /* echo + 32 rows fill the area */
    CHECK(strncmp(hist_line(32), ".m c040", 7) == 0);
    CHECK(strncmp(hist_line(31), ">C:c040  ", 9) == 0);
    CHECK(strncmp(hist_line(0), ">C:c230  ", 9) == 0); /* 32 rows of 16 from the row c040 is in */
    CHECK(m_strlen(hist_line(0)) == 3 + 4 + 2 + 47 + 2 + 16);
    CHECK(strncmp(hist_line(31) + 9 + 47 + 2, "AZa.", 4) == 0);
    type("m\n");
    CHECK(U.hn == 1 + 32 + 1 + 32);
    CHECK(strncmp(hist_line(31), ">C:c240  ", 9) == 0 && strncmp(hist_line(0), ">C:c430  ", 9) == 0);
    type("m c000 c0ff\n"); /* an explicit end is kept: 16 rows */
    CHECK(U.hn == 1 + 32 + 1 + 32 + 1 + 16);
    CHECK(strncmp(hist_line(0), ">C:c0f0  ", 9) == 0);
    type("m ffe0\n"); /* the page stops at the top of memory */
    CHECK(strncmp(hist_line(0), ">C:fff0  ", 9) == 0 && strncmp(hist_line(2), ">C:ffd0", 7) != 0);
    /* RPC keeps TRX64's text: the default is $800, 32 a row, no paging */
    boot();
    {
        const char *o = mon("m c000");
        CHECK(line_count(o) == 64 && max_line(o) == 3 + 4 + 2 + 96 + 2 + 32);
    }

    /* the narrow screen pages too */
    ui_boot();
    type("m c000\n");
    CHECK(U.hn == 1 + 18);
    CHECK(strncmp(hist_line(0), ">C:c088 ", 8) == 0);
    type("m\n");
    CHECK(strncmp(hist_line(0), ">C:c118 ", 8) == 0);

    /* d: a page of 32 lines, the PC's line starts with '>', `d` alone goes on */
    ui_boot_wide();
    type("d c000\n");
    CHECK(U.hn == 1 + 32);
    CHECK(strncmp(hist_line(31), ">C:c000  a9 01     LDA #$01", 25) == 0);
    CHECK(strncmp(hist_line(30), ".C:c002  20 10 c0  JSR $c010", 28) == 0);
    uint32_t last_a = 0;
    sscanf(hist_line(0) + 3, "%x", &last_a);
    type("d\n");
    CHECK(U.hn == 1 + 32 + 1 + 32);
    uint32_t first_a = 0;
    sscanf(hist_line(31) + 3, "%x", &first_a);
    CHECK(first_a > last_a && hist_line(31)[0] == '.');
    /* the RPC text is TRX64's */
    boot();
    CHECK(has(mon("d c000 c007"), "$c000  a9 01     LDA #$01 <-- PC"));

    /* chis: one line to an instruction, no line wraps */
    ui_boot_wide();
    F.ring_n = 10;
    F.ring_c0 = 5000000;
    F.cycles = F.ring_c0 + 9 * 19656 + 1000;
    F.trace_present = 1;
    F.trace_on = 1;
    F.trace_replay = 1;
    for (int j = 0; j < 100; j++) {
        fake_trace_add(150000 + 3u * (uint32_t)j, (uint16_t)(0xC100 + j * 2), 0xA9, (uint8_t)j, 0x77, (uint8_t)j, 0, 0, 0xF6, 0x24, 0, 0);
    }
    {
        static char ob[16384];
        sbuf_t o;
        sb_init(&o, ob, sizeof(ob) - 1);
        rpc_local_exec("chis 5150000 5150012", &o);
        sb_str(&o);
        CHECK(line_count(ob) == 6 && max_line(ob) <= 80);
        int before = U.hn;
        type("chis 5150000 5150012\n");
        CHECK(U.hn - before == 1 + 6);
        CHECK(hist_line(0)[0] == 'c' && strstr(hist_line(0), "a=") != NULL && hist_line(0)[m_strlen(hist_line(0)) - 1] != ' ');
        CHECK(strstr(hist_line(5), "# cpuhistory") != NULL || strstr(hist_line(6), "# cpuhistory") != NULL);
    }
    F.ring_n = 0;
    F.trace_present = 0;

    /* scrolling back through the 200 lines: the oldest stands at the top, each PageDown moves a page */
    ui_boot_wide();
    for (int i = 0; i < 300; i++) {
        char l[16];
        snprintf(l, sizeof(l), "line %d", i);
        ui_print(&U, l, UA_NORMAL);
    }
    CHECK(U.hn == UI_HIST);
    CHECK_STR(row_of(36), "line 299");
    CHECK_STR(row_of(4), "line 267");
    ui_key(&U, UI_K_PGUP);
    CHECK(U.scroll == 33);
    CHECK_STR(row_of(36), "line 266");
    CHECK_STR(row_of(4), "line 234");
    CHECK(strstr(row_of(37), "history -33 lines") != NULL);
    for (int i = 0; i < 12; i++) {
        ui_key(&U, UI_K_PGUP);
    }
    CHECK(U.scroll == 167); /* 200 lines, 33 shown: the oldest is at the top and no further */
    CHECK_STR(row_of(4), "line 100");
    CHECK_STR(row_of(36), "line 132");
    CHECK(strstr(row_of(37), "history -167 lines") != NULL);
    ui_key(&U, UI_K_PGDN); /* the first PageDown moves a page (it used to need one more to start) */
    CHECK(U.scroll == 134);
    CHECK_STR(row_of(36), "line 165");
    for (int i = 0; i < 5; i++) {
        ui_key(&U, UI_K_PGDN);
    }
    CHECK(U.scroll == 0);
    CHECK_STR(row_of(36), "line 299");
    CHECK(strspn(row_of(37), "-") == 80);
    /* the same on the narrow window: 181 is the furthest, one PageDown is a page */
    ui_boot();
    for (int i = 0; i < 300; i++) {
        char l[16];
        snprintf(l, sizeof(l), "line %d", i);
        ui_print(&U, l, UA_NORMAL);
    }
    for (int i = 0; i < 20; i++) {
        ui_key(&U, UI_K_PGUP);
    }
    CHECK(U.scroll == 181);
    ui_key(&U, UI_K_PGDN);
    CHECK(U.scroll == 162);
    CHECK_STR(row_of(21), "line 137");

    /* the keys: the hint row names the function keys the interface made PageUp, PageDown and Help */
    {
        static const int com[9] = { 0, 0x1FC, 0x1FA, 0x92, 0x1F9, 0x95, 0x1FD, 0x1FB, 0x1FE };
        ui_boot_wide();
        ui_set_keys(&U, com);
        CHECK(strncmp(row_of(39), "F3/F5 scroll  F7 help  RUN/STOP pause", 37) == 0);
        ui_boot();
        ui_set_keys(&U, com);
        ui_key(&U, ui_map_key(com[3]));
        CHECK(U.scroll == 0); /* nothing to scroll yet */
    }
    /* w a s d are plain letters to the screen (getch_monitor keeps them letters) */
    ui_boot_wide();
    {
        static const char *wasd = "wasd";
        for (const char *c = wasd; *c; c++) {
            ui_key(&U, ui_map_key(*c));
        }
        CHECK_STR(U.cmd, "wasd");
    }
}

/* A JAM: the CPU stays in decode, no instruction boundary comes (T21 8.5). The poll sees no instruction for
 * JAM_CYCLES cycles, holds the machine and reports a stop with reason jam. */
static void test_jam(void)
{
    ui_boot();
    memset(F.ram + 0xC000, 0xEA, 8); /* eight NOPs, then $02 at $C008 */
    F.ram[0xC008] = 0x02;
    F.pc = 0xC000;
    type("g\n");
    CHECK(M.running && F.running);
    int polls = 0;
    while (M.running && polls < 40) {
        fake_tick(20);
        rpc_poll();
        polls++;
    }
    CHECK(!M.running && F.stuck);
    CHECK(polls > 3 && polls < 40); /* not at once: the machine ran its eight NOPs and then no instruction for the window */
    CHECK_STR(M.stop.reason, "jam");
    CHECK(M.stop.pc == 0xC008 && !M.stop.at_ib);
    CHECK(has(mon("g c010"), "g: the CPU is stuck on a JAM opcode at") && !M.running); /* neither z nor a redirect gets out of it */
    CHECK(rpc_take_user_stop() == 1); /* the screen of a resident trxmon comes up for it */
    mon_set_narrow(0);
    const char *o = mon("status");
    CHECK(has(o, "last stop: jam at $c008"));
    {
        char t[256];
        sbuf_t b;
        sb_init(&b, t, sizeof(t));
        mon_stop_text(&b);
        CHECK(has(sb_str(&b), "JAM at .C:c008 - the CPU is stuck") && has(t, "(opcode $02)"));
    }
    /* RUN/STOP on a stuck CPU the poll has not caught yet: jam, not "pause" */
    ui_boot();
    memset(F.ram + 0xC000, 0xEA, 8);
    F.ram[0xC008] = 0x02;
    F.pc = 0xC000;
    type("g\n");
    fake_tick(30);
    CHECK(F.stuck);
    type("pause\n");
    CHECK(!M.running);
    CHECK_STR(M.stop.reason, "jam");
    CHECK(M.stop.pc == 0xC008);
    /* a running machine that executes: no jam, however long */
    ui_boot();
    type("g\n");
    for (int i = 0; i < 200; i++) {
        fake_tick(50);
        rpc_poll();
    }
    CHECK(M.running && !F.stuck);
    /* the monitor attaches to a CPU that is already stuck (the menu's freeze stopped it): jam, not "PAUSED mid" */
    ui_boot();
    F.ram[0xC008] = 0x02;
    F.pc = 0xC008;
    F.stuck = 1;
    CHECK(mach_own());
    CHECK_STR(M.stop.reason, "jam");
    CHECK(strncmp(screen_text(), "JAM $c008", 9) == 0);
    mon_set_narrow(0);
    CHECK(has(mon("status"), "last stop: jam at $c008"));
    /* a CPU held by DMA (RDY) is not a jam */
    ui_boot();
    memset(F.ram + 0xC000, 0xEA, 8);
    F.ram[0xC008] = 0x02;
    F.pc = 0xC000;
    type("g\n");
    fake_tick(10);
    F.held = 1;
    for (int i = 0; i < 40; i++) {
        fake_tick(20);
        rpc_poll();
    }
    CHECK(M.running);
    F.held = 0;
}

/* the screen and the machine: runs, stops, the stop text, RUN/STOP, repeat */
static const char *last_line(int back) { return U.hist[(U.hhead + UI_HIST - 1 - back) % UI_HIST]; }

static void test_ui_run(void)
{
    ui_boot();
    type("bk c010\n");
    type("g\n");
    CHECK(M.running && F.running);
    CHECK(strncmp(screen_text() + 17, "RUNNING", 7) == 0 || strstr(screen_text(), "\nRUNNING\n") != NULL);
    CHECK(ui_tick(&U) == 0); /* nothing came yet */
    fake_tick(100000);
    rpc_poll();
    CHECK(!M.running);
    CHECK(ui_tick(&U) == 1); /* the breakpoint */
    CHECK_STR(last_line(1), "breakpoint 1 hit at .C:c010");
    CHECK_STR(last_line(0), "$c010  e8        INX");
    CHECK(strncmp(screen_text(), "PAUSED IB $c010", 15) == 0);
    CHECK(rpc_take_user_stop() == 0); /* the screen took it */
    CHECK(ui_tick(&U) == 0);

    /* a stop that came while the screen was closed (nobody waited for it): the screen that comes up for it shows
     * it once, ui_tick does not print it again */
    boot();
    F.running = 0;
    F.shut = 1;
    ui_init(&U);
    mon("bk c010");
    mon("g");
    fake_tick(100000);
    rpc_poll();
    CHECK(!M.running);
    ui_open(&U);
    ui_show_stop(&U);
    int shown = U.hn;
    CHECK(shown >= 2 && strncmp(last_line(1), "breakpoint 1 hit at .C:c010", 27) == 0);
    CHECK(ui_tick(&U) == 0 && U.hn == shown);

    /* a run-to verb waits, shows what it waits for, and prints its landing */
    ui_boot();
    F.pc = 0xC000;
    type("until c005\n");
    CHECK(M.running && rpc_local_waiting());
    CHECK(strstr(screen_text(), "\nRUNNING until c005\n") != NULL);
    fake_stop(0xC005, RS_UNIT_ADDR, 0);
    rpc_poll();
    CHECK(ui_tick(&U) == 1);
    CHECK_STR(last_line(1), "until $c005 reached -> .C:c005 (300");
    CHECK_STR(last_line(0), "instr, 1000 cyc)");
    CHECK(!rpc_local_waiting());
    CHECK(strncmp(screen_text(), "PAUSED IB $c005", 15) == 0);

    /* n over the JSR */
    ui_boot();
    F.pc = 0xC002;
    type("n\n");
    CHECK(rpc_local_waiting());
    fake_stop(0xC005, RS_UNIT_ADDR, 0);
    rpc_poll();
    ui_tick(&U);
    CHECK(strncmp(last_line(0), "$c005", 5) == 0);

    /* RUN/STOP: pauses a running machine, does nothing paused */
    ui_boot();
    ui_key(&U, UI_K_STOP);
    CHECK(U.hn == 0 && !M.running);
    type("g\n");
    int hn = U.hn;
    ui_key(&U, UI_K_STOP);
    CHECK(!M.running && U.hn > hn);
    CHECK(strstr(last_line(0), "paused at .C:") != NULL);
    CHECK(U.leave == UI_STAY);

    /* RETURN on an empty line repeats the last z/n/cs while paused; nothing before one */
    ui_boot();
    ui_key(&U, UI_K_RETURN);
    CHECK(U.hn == 0);
    type("z\n");
    CHECK(strncmp(screen_text(), "PAUSED IB $c002", 15) == 0);
    ui_key(&U, UI_K_RETURN);
    CHECK_STR(last_line(1), ".z");
    CHECK(strncmp(screen_text(), "PAUSED IB $c010", 15) == 0);
    type("r\n");
    hn = U.hn;
    ui_key(&U, UI_K_RETURN); /* the last step verb is still z */
    CHECK(U.hn > hn && strncmp(screen_text(), "PAUSED IB $c0", 13) == 0);

    /* a stop nobody waited for while the screen is open: a client's run ends on a breakpoint */
    ui_boot();
    F.running = 1;
    M.running = 1;
    ui_open(&U);
    fake_stop(0xC010, RS_BITMAP, 0);
    rpc_poll();
    CHECK(ui_tick(&U) == 1);
    CHECK(strstr(last_line(1), "hit at .C:c010") != NULL || strstr(last_line(1), "stopped") != NULL);

    /* the command line shows the tail of a long line; the last cell stays free */
    ui_boot();
    for (int i = 0; i < 60; i++) {
        ui_key(&U, 'a' + (i % 26));
    }
    {
        static ui_screen_t sc;
        ui_render(&U, &sc);
        CHECK(sc.cursor_x == 38 && sc.cursor_y == 22);
        CHECK(sc.row[22][0] == '.' && sc.row[22][39] == ' ');
        CHECK(sc.row[22][37] == 'a' + (59 % 26));
    }

    /* the live log: a log observer's hits print on the running screen as they come, and the screen keeps more
     * rows for them than the status line and two lines of output */
    ui_boot();
    mon("obs ll when store d020 do log");
    mon("g c000");
    {
        static ui_screen_t sc;
        ui_render(&U, &sc);
        CHECK(sc.blank_rows == U.rows - (UI_SHRUNK_ROWS + UI_LIVE_EXTRA_ROWS));
    }
    ui_open(&U);
    F.pc = 0xC005;
    CHECK(fake_access(0xD020, 0xF1, 1));
    rpc_poll();
    CHECK(M.running && ui_tick(&U) == 1);
    CHECK(strstr(screen_text(), "obs ll: store $d020=$f1") != NULL);
    mon("obs * del");
    {
        static ui_screen_t sc;
        ui_render(&U, &sc);
        CHECK(sc.blank_rows == U.rows - UI_SHRUNK_ROWS);
    }
}


/* ---- observers: condition, ignore, log, cmd, raster (T55) ---------------------------------------------------- */

/* runs the fake machine one instruction at a time with polling after each, until the machine is stopped for
 * the poll or n instructions went; returns the instructions run */
static int run_polled(int n)
{
    int i;
    for (i = 0; i < n && F.running; i++) {
        fake_tick(1);
        rpc_poll();
    }
    return i;
}

static int olog_has(const char *needle)
{
    for (int i = 0; i < mach_olog_count(); i++) {
        if (strstr(mach_olog_line(i), needle)) {
            return 1;
        }
    }
    return 0;
}

static void test_observers(void)
{
    const char *o;
    /* an exec observer with a condition the bitmap cannot hold: the stop is re-checked in software */
    boot();
    F.shut = 1;
    o = mon("obs c when exec c010 if x == 5 && a < 9 do break");
    CHECK(has(o, "hw: exec bitmap, soft: x==05 && a<09"));
    mon("g c000");
    F.x = 0;
    run_polled(40);
    CHECK(M.running && F.running); /* c010 was reached, the condition was false: the machine went on */
    F.x = 5;
    run_polled(40);
    CHECK(!M.running && M.stop.pc == 0xC010 && m_streq(M.stop.reason, "observer") && M.stop.obs == 0);
    CHECK(M.obs[0].hits == 1);
    mon("obs * del");

    /* a stop an observer lets go inside an until: the run-to is armed again and still lands */
    boot();
    F.shut = 1;
    mon("obs c when exec c010 if x == 5 do break");
    F.x = 0;
    nsent = 0;
    mon("until c005");
    run_polled(30);
    CHECK(!M.running && has(sent[0], "until $c005 reached -> .C:c005"));
    mon("obs * del");

    /* ignore counted after each stop (a bitmap observer) */
    boot();
    F.shut = 1;
    mon("obs d when exec c010 do break");
    o = mon("ignore d 2");
    CHECK(has(o, "ignore d: skip next 2 (counted after each stop)"));
    mon("g c000");
    int stops = 0;
    for (int i = 0; i < 20 && M.running; i++) {
        fake_tick(3);
        rpc_poll();
        stops++;
    }
    CHECK(!M.running && M.obs[0].hits == 1 && M.obs[0].ignore == 0 && stops >= 3);
    mon("obs * del");

    /* a store observer: the condition goes into the unit (val, one register, the address range) */
    boot();
    F.shut = 1;
    o = mon("obs s when store d000..d0ff if val == 5 && a >= 3 && x != 7 && addr >= $d010 do break");
    CHECK(has(o, "hw: watch unit 0 (addr+val+reg), soft: x!=07"));
    CHECK(F.unit[0].lo == 0xD010 && F.unit[0].hi == 0xD0FF);
    CHECK(F.unit[0].data == ((0xFFu << 8) | 5));
    CHECK(F.unit[0].reg == UNIT_REG(R_SEL_A, R_OP_GE, 3, 0xFF));
    mon("g c000");
    F.a = 4;
    F.x = 7;
    CHECK(!fake_access(0xD020, 6, 1)); /* data differs: the unit does not see it */
    CHECK(!fake_access(0xD005, 5, 1)); /* below the narrowed range */
    F.a = 2;
    CHECK(!fake_access(0xD020, 5, 1)); /* a < 3 */
    F.a = 4;
    CHECK(fake_access(0xD020, 5, 1)); /* the unit stops the machine ... */
    rpc_poll();
    CHECK(M.running); /* ... and x != 7 is false: it runs on */
    F.x = 1;
    CHECK(fake_access(0xD020, 5, 1));
    rpc_poll();
    CHECK(!M.running && m_streq(M.stop.reason, "observer") && M.stop.rec.data == 5 && M.stop.rec.addr == 0xD020);
    CHECK(M.obs[0].hits == 1);
    mon("obs * del");

    /* ignore in the unit (PASS) */
    boot();
    F.shut = 1;
    mon("obs p when store d021 do break");
    o = mon("ignore p 3");
    CHECK(has(o, "ignore p: skip next 3 (counted by the watch unit)") && F.unit[0].pass == 3);
    mon("g c000");
    CHECK(!fake_access(0xD021, 1, 1) && !fake_access(0xD021, 1, 1) && !fake_access(0xD021, 1, 1));
    CHECK(fake_access(0xD021, 1, 1));
    rpc_poll();
    CHECK(!M.running && M.obs[0].hits == 1);
    mon("obs * del");

    /* a log the record can carry runs in the unit: no stop, lines in obs log; the registers need the stop */
    boot();
    F.shut = 1;
    F.ring_n = 3; /* the frame ring runs */
    F.log_present = 1;
    o = mon("obs lg when store d020 do log");
    CHECK(has(o, "hw: watch unit 0 (addr+log)") && (F.unit[0].ctrl & U_A_LOG));
    mon("g c000");
    F.pc = 0xC005;
    F.cycles = 100000;
    CHECK(!fake_access(0xD020, 0x0E, 1));
    CHECK(!fake_access(0xD020, 0x0F, 1));
    CHECK(M.running);
    o = mon("obs log");
    CHECK(has(o, "obs lg: store $d020=$0e  pc=$c005 cyc=100000\nobs lg: store $d020=$0f  pc=$c005 cyc=100000"));
    o = mon("obs");
    CHECK(has(o, "do log  hits=2"));
    mon("obs lg del");
    F.ring_n = 0; /* no ring: the same observer logs at the stop, with the registers */
    F.log_n = F.log_rd = 0;
    o = mon("obs sl when store d020 do log a x $0400 $0400:w pc");
    CHECK(has(o, "hw: watch unit 0 (addr), soft: log at the stop") && !(F.unit[0].ctrl & U_A_LOG));
    F.ram[0x0400] = 0x08;
    F.ram[0x0401] = 0x05;
    F.a = 0x44;
    F.x = 0x55;
    F.pc = 0xC005;
    mon("g c000");
    CHECK(fake_access(0xD020, 0x0E, 1));
    rpc_poll();
    CHECK(M.running); /* logged, went on */
    CHECK(olog_has("obs sl: store $d020=$0e  a=44 x=55 $0400=08 $0400=0508 pc=$c000 cyc="));
    mon("obs sl del");

    /* do cmd: monitor commands at the stop, their output in the log, the machine goes on */
    boot();
    F.shut = 1;
    mon("obs cm when store d020 do cmd \\\"m c000 c003; g; r\\\"");
    mon("g c000");
    CHECK(fake_access(0xD020, 1, 1));
    rpc_poll();
    CHECK(M.running);
    CHECK(olog_has("obs cm: .m c000 c003") && olog_has(">C:c000") && olog_has("refused: a do cmd observer does not move") &&
          olog_has("obs cm: .r"));
    mon("obs * del");

    /* an exec observer that logs in the unit (no cond): the bitmap stays off, a record per instruction fetch */
    boot();
    F.shut = 1;
    F.ring_n = 3;
    F.log_present = 1;
    o = mon("obs el when exec c010 do log");
    CHECK(has(o, "hw: watch unit 0 (addr+log)") && !F.bmp_en && (F.unit[0].ctrl & U_T_IB));
    mon("g c000");
    run_polled(10);
    CHECK(M.running);
    o = mon("obs log");
    CHECK(has(o, "obs el: exec $c010  pc=$c010 cyc="));
    mon("obs * del");

    /* rb */
    boot();
    F.shut = 1;
    o = mon("rb f9");
    CHECK(has(o, "rb set:\n  * rb  raster line $0f9 do break  hits=0  hw: watch unit 0 (raster)"));
    CHECK((F.unit[0].ctrl & U_RASTER) && (F.unit[0].ctrl & U_T_CYCLE) && F.unit[0].lo == (0xF9 << 7) && F.unit[0].hi == (0xF9 << 7));
    o = mon("rb f9 18");
    CHECK(F.unit[0].lo == ((0xF9 << 7) | 3)); /* x selects the cycle x/8 */
    o = mon("rb 200");
    CHECK(has(o, "rb: raster line out of range"));
    mon("g c000");
    fake_stop(0xC004, RS_UNIT_RAS, 0);
    rpc_poll();
    CHECK(!M.running && m_streq(M.stop.reason, "observer") && m_streq(M.obs[M.stop.obs].name, "rb"));
    o = mon("rb -");
    CHECK_STR(o, "rb removed");
    CHECK(F.unit[0].ctrl == 0);
    /* obs .. when raster */
    o = mon("obs r1 when raster 50 do break");
    CHECK(has(o, "raster line $050") && F.unit[0].lo == (0x50 << 7));
    mon("obs * del");

    /* every number is hex, with or without `$`; `a` is the register, `$a` the number; the list shows them alike */
    boot();
    F.shut = 1;
    o = mon("obs h1 when store d020 if val == f5 do break");
    CHECK(has(o, "if val==f5 do break") && F.unit[0].data == ((0xFFu << 8) | 0xF5));
    o = mon("obs h2 when store d021 if val == $F5 && a == 5 do break");
    CHECK(F.unit[1].data == ((0xFFu << 8) | 0xF5) && F.unit[1].reg == UNIT_REG(R_SEL_A, R_OP_EQ, 5, 0xFF));
    o = mon("obs h3 when exec c000 if y == $a || x == a do break");
    CHECK(has(o, "if y==0a || x==a "));
    o = mon("obs");
    CHECK(has(o, "if val==f5 do break") && has(o, "if val==f5 && a==05 do break"));
    o = mon("ignore h1 10");
    CHECK(has(o, "ignore h1: skip next 10 (counted by the watch unit)") && F.unit[0].pass == 0x10);
    o = mon("obs");
    CHECK(has(o, "pass=10"));
    CHECK(has(mon("obs h4 when store d020 if val == g5 do break"), "unknown term"));
    CHECK(has(mon("obs h4 when store d020 if val == 12345678f do break"), "unknown term"));
    mon("obs * del");

    /* g <addr> from the middle of an instruction finishes it first (z) */
    boot();
    F.shut = 1;
    F.pc = 0xC000;
    F.mid = 1;
    o = mon("g c010");
    CHECK(has(o, "continuing at .C:c010") && !F.mid && M.running);
    boot();
    F.shut = 1;
    F.mid = 1;
    o = mon("r pc=c010");
    CHECK(has(o, "mid-instruction: step to the boundary first (z)"));
    F.mid = 0;

    /* a flood of hits on a software log: a few lines, "+K skipped", the unit thinned, the machine kept running */
    boot();
    F.shut = 1;
    F.ring_n = 0;
    mon("obs fl when store d020 do log");
    mon("g c000");
    uint32_t seq0 = mach_olog_seq();
    int stops_seen = 0;
    F.cycles = 1000000;
    for (int i = 0; i < 2000; i++) {
        F.pc = 0xC005;
        F.cycles += 100; /* the machine's own clock */
        if (fake_access(0xD020, (uint8_t)(0xF0 + (i & 15)), 1)) {
            rpc_poll();
            stops_seen++;
            CHECK(M.running);
        }
    }
    int lines = 0;
    for (uint32_t c = seq0; mach_olog_next(&c);) {
        lines++;
    }
    CHECK(lines <= 3 * 20 && lines >= 3);       /* the burst and what the rate allows over the fake clock */
    CHECK(stops_seen < 100);                    /* the unit passed hits: far fewer stops than hits */
    CHECK(F.unit[0].pass > 0);
    F.ms += 400;
    rpc_poll(); /* the unit waited for its PASS count in vain: it is armed plain again, its hits counted */
    CHECK(F.unit[0].pass == 0);
    F.ms += 400;
    rpc_poll(); /* the flood is over: the skipped count is told */
    CHECK(olog_has(" skipped"));
    CHECK(M.obs[0].hits > 1500);                /* the passed hits count */
    mon("obs * del");

    /* the RPC client hears the lines: debug/observer_log, the lines of 100 ms in one go */
    boot();
    F.shut = 1;
    F.ring_n = 0;
    mon("obs nl when store d020 do log");
    mon("g c000");
    rpc_poll(); /* the notifier starts at the present */
    nnotes = 0;
    F.pc = 0xC005;
    CHECK(fake_access(0xD020, 0xF3, 1));
    rpc_poll();
    CHECK(nnotes >= 1 && strstr(notes[0], "\"method\":\"debug/observer_log\"") && strstr(notes[0], "obs nl: store $d020=$f3"));
    int n1 = nnotes;
    CHECK(fake_access(0xD020, 0xF4, 1));
    rpc_poll();
    CHECK(nnotes == n1); /* the next line waits for its 100 ms: one packet for a few lines */
    F.ms += 150;
    rpc_poll();
    CHECK(nnotes == n1 + 1 && strstr(notes[nnotes - 1], "obs nl: store $d020=$f4"));
    mon("obs * del");

    /* the hardware log of the same flood: bounded reads, the rest reported as skipped */
    boot();
    F.shut = 1;
    F.ring_n = 3;
    F.log_present = 1;
    mon("obs hl when store d020 do log");
    mon("g c000");
    seq0 = mach_olog_seq();
    for (int i = 0; i < 200; i++) {
        F.pc = 0xC005;
        fake_access(0xD020, (uint8_t)i, 1);
    }
    rpc_poll();
    lines = 0;
    for (uint32_t c = seq0; mach_olog_next(&c);) {
        lines++;
    }
    CHECK(lines >= 1 && lines <= 21);
    F.ms += 400;
    rpc_poll();
    CHECK(olog_has("+") && olog_has(" skipped"));
    CHECK(M.running);
    mon("obs * del");

    /* errors */
    boot();
    F.shut = 1;
    CHECK(has(mon("obs b when exec c000 if a ==  do break"), "obs: condition: unexpected end of condition"));
    CHECK(has(mon("obs b when exec c000 if q == 1 do break"), "unknown term"));
    CHECK(has(mon("obs b when exec c000 do mark"), "obs: action must be break|log|cmd"));
    CHECK(has(mon("obs b when exec c000 do log a z"), "obs: log: bad field 'z'"));
    CHECK(has(mon("obs b when exec c000 do cmd"), "obs: cmd: usage"));
    CHECK(has(mon("ignore nosuch 2"), "no observer 'nosuch'"));
    CHECK(has(mon("obs"), "no observers"));
    CHECK(M.nobs == 0);
}

/* reu and pot (T55) */
static void test_reu_pot(void)
{
    const char *o;
    boot();
    F.shut = 1;
    F.has_lk = 1;
    memset(F.cart_io, 0xFF, sizeof(F.cart_io));
    F.cart_io[0x00] = 0x10 | 0x40;      /* status: 256K chips, end of block */
    F.cart_io[0x01] = 0x91;             /* command: execute, fetch */
    F.cart_io[0x02] = 0x00; F.cart_io[0x03] = 0x04;  /* C64 $0400 */
    F.cart_io[0x04] = 0x34; F.cart_io[0x05] = 0x12;  /* REU $xx1234 */
    F.cart_io[0x06] = 0xF8 | 0x02;      /* bank 2 */
    F.cart_io[0x07] = 0xE8; F.cart_io[0x08] = 0x03;  /* length $03E8 */
    F.cart_io[0x09] = 0x1F;
    F.cart_io[0x0A] = 0x3F;
    o = mon("reu");
    CHECK_STR(o, "reu: 512 KB, 1764/1750-class (256K chips)\n"
                 "  status   $50  verify-error no  end-of-block yes  irq-pending no\n"
                 "  command  $91  fetch (REU -> C64)  autoload no  $FF00-trigger disabled\n"
                 "  C64 $0400  REU $02:1234  length $03E8\n"
                 "  int-mask $1F  addr-control $3F");
    strcpy(host_reu_en, "Disabled");
    CHECK(has(mon("reu"), "reu: nothing on the expansion port"));
    strcpy(host_reu_en, "GeoRAM Mode");
    CHECK(has(mon("reu"), "GeoRAM mode"));
    strcpy(host_reu_en, "Enabled");
    F.has_lk = 0;
    CHECK(has(mon("reu"), "not readable without a bus cycle"));
    /* pot: CIA1 port A out is on the state bus; the SID latch needs sidefx */
    F.cia1[0] = 0x40;
    o = mon("pot");
    CHECK(has(o, "pot: selected "));
    CHECK(has(o, "$d419=--"));
    CHECK(has(mon("pot 1 2 3"), "read-only here"));
}

/* flow, focus, sf, nf (T55) */
static void hook_tick_fake(int n)
{
    (void)n;
    fake_tick(5);
}

static void test_flow(void)
{
    const char *o;
    boot();
    F.shut = 1;
    memset(F.ram + 0xC000, 0xEA, 16); /* NOPs */
    F.ram[0xFF48] = 0x40;             /* the handler: RTI */
    F.ram[0xFF49] = 0xEA;
    F.pc = 0xC000;
    o = mon("flow");
    CHECK_STR(o, "flow: current=main  focus=auto\nframes:\n  (main \xe2\x80\x94 no interrupt/trap frame active)");
    /* an interrupt after the next instruction: z lands in the handler, the flow knows */
    F.irq_after = 1;
    o = mon("z");
    CHECK(has(o, "$ff48") && has(o, " [irq] (step, "));
    o = mon("flow");
    CHECK(has(o, "flow: current=irq  focus=auto\nframes:\n  irq  enter=$ff48 -> ret=$c000  cyc="));
    CHECK(has(mon("r"), "nv-bdIzc  irq\n"));
    o = mon("focus");
    CHECK(has(o, "focus = auto (current flow: irq)\nflow stack:\n  irq  enter=$ff48 ret=$c000"));
    /* RTI pops */
    o = mon("z");
    CHECK(!has(o, "[irq]") && F.pc == 0xC001);
    CHECK(has(mon("flow"), "current=main"));
    /* focus + sf: step until the CPU is in the irq flow */
    CHECK_STR(mon("focus irq"), "focus = irq");
    CHECK(has(mon("focus bogus"), "focus: expected auto|main|irq|nmi|brk|clear"));
    F.irq_after = 4;
    o = mon("sf");
    CHECK(has(o, "$ff48") && has(o, "[irq] (stepf:irq, "));
    CHECK_STR(mon("focus main"), "focus = main");
    o = mon("sf"); /* RTI: back in main */
    CHECK(!has(o, "[irq]") && has(o, "(stepf:main, "));
    CHECK_STR(mon("focus clear"), "focus = none");
    CHECK_STR(mon("focus auto"), "focus = auto");
    /* nf over a JSR: runs the callee, one step in the flow otherwise */
    F.ram[0xC000] = 0x20; /* JSR $C010 */
    F.ram[0xC001] = 0x10;
    F.ram[0xC002] = 0xC0;
    F.ram[0xC010] = 0xE8;
    F.ram[0xC011] = 0x60;
    F.pc = 0xC000;
    F.s = 0xF6;
    sleep_hook = hook_tick_fake;
    o = mon("nf");
    sleep_hook = 0;
    CHECK(has(o, "$c003") && has(o, "(nextf:main, ") && !M.running && F.pc == 0xC003);
}

int main(void)
{
    test_ws();
    test_json();
    test_dis();
    test_asm();
    test_rpc();
    test_monitor();
    test_ownership();
    test_ring();
    test_ring_reu();
    test_lens();
    test_trace();
    test_video();
    test_wait();
    test_ui();
    test_ui_run();
    test_jam();
    test_observers();
    test_reu_pot();
    test_flow();
    test_ui_wide();
    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
