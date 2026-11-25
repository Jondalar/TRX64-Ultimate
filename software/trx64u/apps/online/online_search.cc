// The online search as a sub-app (U21). The form is the F3 tab "Online" (app.toml): Source first, then the fields of that source, Search at its foot. The results are a tab of
// their own, "Results", on the strip only while there are results (app.toml `show`); their list is a node of kind `list` (api.md 6.19), and RETURN on a result opens its file's action
// sheet, or the list of its files (op_open), in the Library, which reads them through the table "online" (online_api.h) as the folder "/Online". Three sources: Assembly64 and CommoServe (one server, ONLINE_HOST; the
// header Client-Id picks the library, "Ultimate" or "Commodore"; the query is built the way the firmware's assembly_search.cc builds it: (field:"value") joined by &, a choice
// unquoted by its aqlKey) and Beyond64 (B64_HOST, its public JSON API, U21 §4). Every request to a server runs on a job (api->job->start): api.md 1.3.9 and 4.8.13.2 forbid
// api->net connect, send and recv on the menu task and the executor task. Search is a node with `job`: the menu calls online.search again while it answers `more`. The choice
// lists, the result rows and the folder "/Online" are read on the menu task: they answer from what is kept and start a job for the rest, which a later read finds ready. One
// job at a time (Work). The state of the search is this file's own; the client is online_http.cc and online_json.cc.
#include "online_impl.h"
#include "online_http.h"
#include "online_json.h"
#include "otext.h"

#define MAX_VALUES 48                      // the values of one choice (Assembly64's Year has 47)
#define MAX_RES 200                        // the results kept: Assembly64 and CommoServe give 20 (U21 D7), Beyond64 B64_LIMIT a page, the pages add up
#define MAX_PAGE 50                        // the most one request brings (Beyond64 takes a limit of 1 to 50)
#define MAX_ENT 64
#define B64_LIMIT 20                       // Beyond64's releases per page: as many as Assembly64 and CommoServe give. With the details of each (D5) a page took 1.9-2.5 s
                                           // in ue2emu, a page of 50 (the API's most) 3.5-3.9 s (U21 D8, measured 2026-11-25)
#define LIST_MAX 3584                      // the text of one choice list (48 values of up to 66 characters)
#define ROW_MAX 320                        // one row of the result list: value, 61, 39, tone, 3 x 63, the TABs
#define ROWS_MAX (MAX_RES * ROW_MAX + 256)
#define TRY_AGAIN_MS 10000u                // a failed load of the choices is tried again after this long at the earliest

#define B64_HOST "beyond64.org"
#define URL_SEARCH   "/leet/search/aql?query="
#define URL_PATTERNS "/leet/search/aql/presets"
#define URL_ENTRIES  "/leet/search/entries"
#define URL_DOWNLOAD "/leet/search/bin"
#define B64_RELEASES "/api/c64/releases"
#define B64_RELEASE  "/api/c64/release/"
#define B64_DOWNLOAD "/api/c64/download/"
#define B64_FILTERS  "/api/c64/filters"
#define WORK_STACK 32768u                  // the stack of the job: the HTTP client, the JSON reader (J_DEPTH 12) and the parsers above

// " <bullet> ": U+2022, which the menu's fonts carry beside ASCII (LVGL's Montserrat, built with the range 0x20-0x7F, 0xB0, 0x2022)
#define DOT " \xE2\x80\xA2 "

namespace {

enum { S_A64 = 0, S_CS = 1, S_B64 = 2, NSRC = 3 };
const char *const k_src[NSRC] = { "Assembly64", "CommoServe", "Beyond64" };

// The choices of Assembly64 and CommoServe: the types of the server's presets, with the words the form shows. The server says which types a library has; until it has
// answered, the types measured on 2026-11-25 stand for it (libs: bit 0 Assembly64, bit 1 CommoServe; U21 §2).
struct AChoice {
    const char *type;
    const char *label;
    const char *any;                       // the value shown while none is chosen
    uint8_t libs;
};
#define NACH 9
const AChoice k_ach[NACH] = {
    { "category", "Category", "Any", 3 },
    { "subcat", "Subcategory", "Any", 1 },
    { "repo", "Archive", "Any", 1 },
    { "date", "Year", "Any", 3 },
    { "latest", "Added within", "Any time", 1 },
    { "rating", "Rating", "Any", 1 },
    { "type", "File type", "Any", 3 },
    { "sort", "Sort by", "Default", 3 },
    { "order", "Order", "Default", 3 },
};
enum { ACH_SORT = 7, ACH_ORDER = 8 };

// The fields of Beyond64 (U21 §2, §4). `filt` names the list of /api/c64/filters the values come from (0: the values are fixed here); `multi`: several values at once (a pick
// adds or takes away one), joined by commas as the API takes them.
struct BDef {
    const char *key;
    const char *label;
    const char *any;
    char filt;
    bool multi;
};
#define NBF 9
const BDef k_bf[NBF] = {
    { "look", "Look in", "Title, creator, credits", 'f', false },
    { "cat", "Category", "Any", 'c', true },
    { "catm", "Category match", "Any of them", 0, false },
    { "plat", "Platform", "Any", 'p', false },
    { "env", "Environment", "Any", 'e', false },
    { "hw", "Hardware", "Any", 'h', true },
    { "hwm", "Hardware match", "All of them", 0, false },
    { "ai", "AI", "Any", 'a', false },
    { "show", "Show", "Loadable only", 0, false },
};
enum { B_LOOK, B_CAT, B_CATM, B_PLAT, B_ENV, B_HW, B_HWM, B_AI, B_SHOW };

struct Preset {
    char type[20];
    int n;
    char key[MAX_VALUES][24];
    char name[MAX_VALUES][28];
    int16_t id[MAX_VALUES];                // subcat: the number a result's `category` gives
};
struct Presets {
    int n;
    Preset p[ONLINE_PRESETS];
};

struct BList {
    int n;
    char v[12][32];
    char label[12][32];
};
struct BFilters {
    BList c, pl, e, h, ai, f;
};

struct Form {
    int src;
    char name[124];                        // Assembly64, CommoServe: Name; Beyond64: the text (q, up to 120 characters)
    char group[48], handle[48], event[48];
    char ach[NACH][24];                    // the chosen aqlKey of each choice, "" none
    char b[NBF][200];                      // Beyond64: the value of each field as the API takes it, "" the default
};

struct Res {
    char title[64];
    char folder[60];                       // the name of its folder in "/Online": unique among the results
    char id[16];
    int cat;                               // Assembly64, CommoServe: the category the entries and the download name
    char right[40];
    char side[3][64];
    bool loadable;                         // Beyond64: a file the download bridge serves (.prg, .d64, .d81); the others always
    int nfiles;                            // Beyond64: how many of its files load here (the details name them); 0: not known (Assembly64, CommoServe name none in a search)
    char file1[64];                        // Beyond64, nfiles 1: that file's name in the folder, and its place in the release's f (the download names it)
    int file1_idx;
};

struct Results {
    int gen;                               // counts the changes of the results: a job started for older results drops its answer
    int src;
    int n;
    Res r[MAX_RES];
    bool more;                             // Beyond64: there is a next page
    int page;
    int looked;                            // Beyond64: the releases looked at
    int hidden;                            // Beyond64: of them not listed (no loadable file, Show: loadable only)
    bool show_all;
    char text[48];                         // what was searched for, short (the title)
    char query[160];                       // all of it (the side panel)
    char path[1024];                       // Beyond64: the request of page 1 (online.more asks for the next page with it)
};

struct Ent {
    char name[64];
    int idx;
    uint64_t size;                         // FS_SIZE_UNKNOWN (fsif.h, base 3.6): Beyond64 names no size, the file list shows "-"
};

const LibApi *g_api;
SvcMutex *g_mx;                            // guards the hand-over of a finished job's result (collect)
Form g_f;
Presets g_pre[2];
bool g_pre_ok[2];
BFilters g_bf;
bool g_bf_ok;
bool g_tried[NSRC];
uint32_t g_try_ms[NSRC];
char g_choice_err[NSRC][96];
Results g_res;
int ent_for = -1, ent_gen = -1;
Ent ent[MAX_ENT];
int nent;
uint8_t *dl_b;
uint32_t dl_len;
int dl_res = -1, dl_idx = -1, dl_gen = -1;
char err[96];                              // what the folder "/Online" says (tab_error)
char g_last[24];                           // the form's side panel: what the last search was for (cut short: a value of the panel has the room of about 16 characters)
char g_outcome[24];                        // and what it came to
char g_more_err[96];                       // why the last page did not come
char g_at[80];                             // "/Online/<result>/": the folder lib.show opens (op_open)

enum { W_PRESETS = 1, W_FILTERS, W_SEARCH, W_MORE, W_ENTRIES, W_DOWNLOAD };

// The one request in flight. The job writes only here, and only while state is 1; the menu and executor tasks never read the result before state is 2, and take it over (collect)
// into the variables above. state: 0 idle, 1 the job runs, 2 the job has ended and its result is not taken yet.
struct Work {
    volatile int state;
    volatile bool abandon;                 // the person cancelled: the result is not wanted
    SvcJobH *jh;                           // the job's own handle, for api->job->cancelled
    int kind;
    int src;
    int gen;                               // W_MORE, W_ENTRIES, W_DOWNLOAD: the results it is for
    int form;                              // the form operation (W_SEARCH) that a stepping menu waits for, 0 none
    char path[1024];
    uint32_t max;
    int ri, ei;                            // W_ENTRIES: the result; W_DOWNLOAD: the result and the server's index of the file
    int page;                              // W_SEARCH, W_MORE of Beyond64: the page asked for
    bool ok;
    char err[96];
    Presets pre;
    BFilters bf;
    int nres;
    Res res[MAX_PAGE];
    bool more;
    int nent;
    Ent ent[MAX_ENT];
    uint8_t *body;
    uint32_t len;
};
Work g_w;

// What the last search came to, for the operation that waits for it.
struct Out {
    bool fresh;
    bool ok;
    bool empty;                            // nothing to list: the old results stay
    int shown;
};
Out g_out;

// What a failed prefetch (W_ENTRIES, W_DOWNLOAD) said: told once to the next call that asks for the same thing, which then asks the server again.
struct PfErr {
    int kind, ri, ei;
    char text[96];
};
PfErr g_pf;

// The search the menu steps: made at its first call, started as soon as no other job runs.
struct Pending {
    bool waiting;                          // not started yet: another job runs
    int src;
    bool show_all;
    char path[1024];
    char text[48];
    char query[160];
};
Pending g_s;

// ---- helpers -------------------------------------------------------------------------------------------------------------------------------------------------
uint32_t now_ms(void)
{
    return g_api && g_api->sys && g_api->sys->ms ? g_api->sys->ms() : 0;
}

void log_line(const char *s)
{
    if (g_api && g_api->log) {
        g_api->log(s);
    }
}

// The job is asked to end: the person cancelled the request, or the base asks the job (api->job->cancelled: the sub-app is being unloaded)
bool work_cancelled(void *)
{
    return g_w.abandon || (g_api && g_api->job && g_api->job->cancelled && g_w.jh && g_api->job->cancelled(g_w.jh));
}

HttpEnv env(void)
{
    HttpEnv e = { g_api ? g_api->net : 0, g_api ? g_api->mem : 0, g_api ? g_api->sys : 0, work_cancelled, 0 };
    return e;
}

void *tree_alloc(uint32_t n, void *)
{
    return g_api && g_api->mem ? g_api->mem->malloc(n) : 0;
}

// The request to the server of a source.
HttpReq req_for(int src, const char *path, const char *addr, uint32_t max)
{
    HttpReq q;
    q.host = src == S_B64 ? B64_HOST : ONLINE_HOST;
    q.addr = addr;
    q.path = path;
    q.client_id = src == S_B64 ? 0 : src == S_CS ? "Commodore" : "Ultimate";
    q.agent = src == S_B64 ? "TRX64-Ultimate online" : "Assembly Query";
    q.max_body = max;
    return q;
}

// a name of the Library: no slash, no control character
void clean(char *dst, int n, const char *src)
{
    ot_fold(dst, n, src);
    for (char *p = dst; *p; p++) {
        if (*p == '/' || *p == '\\') {
            *p = '-';
        }
    }
}

// what the person reads when a request failed
void say_failed(char *dst, int n, int src, int rc, const HttpOut &o)
{
    dst[0] = 0;
    if (rc == HTTP_STATUS) {
        ot_copy(dst, n, k_src[src]);
        ot_cat(dst, n, " answered ");
        ot_cat_int(dst, n, o.status);
    } else if (rc == HTTP_TOO_BIG) {
        ot_copy(dst, n, "The answer is too big for the memory");
    } else if (rc == HTTP_NO_ROOM) {
        ot_copy(dst, n, "No memory for the answer");
    } else {
        ot_copy(dst, n, "No answer from ");
        ot_cat(dst, n, k_src[src]);
        ot_cat(dst, n, " (network?)");
    }
}

// URL encoding as the firmware's url_encode does it: letters, digits and _-.* stay, every other byte is %xx
void url_cat(char *dst, int n, const char *src)
{
    static const char hex[] = "0123456789abcdef";
    for (; *src; src++) {
        unsigned char c = (unsigned char)*src;
        char one[4];
        if (c == '_' || c == '-' || c == '.' || c == '*' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
            one[0] = (char)c;
            one[1] = 0;
        } else {
            one[0] = '%';
            one[1] = hex[c >> 4];
            one[2] = hex[c & 15];
            one[3] = 0;
        }
        ot_cat(dst, n, one);
    }
}

// One request whose answer is JSON: the tree is in `t` (the text is `*text`, the nodes `t->pool`); both are freed with json_free. False: `why` says why, *rc is the client's code
// (HTTP_OK with an answer that is no JSON).
struct JReply {
    uint8_t *text;
    JTree t;
};

// a request that failed, in the console: the source, the path, the client's code and, for a connection not made, SvcNet::connect's
void log_failed(int src, const char *path, int rc, const HttpOut &o)
{
    char m[200] = "online: ";
    ot_cat(m, sizeof(m), k_src[src]);
    ot_cat(m, sizeof(m), " ");
    ot_cat(m, sizeof(m), path);
    ot_cat(m, sizeof(m), ": failed ");
    ot_cat_int(m, sizeof(m), rc);
    if (o.status) {
        ot_cat(m, sizeof(m), ", status ");
        ot_cat_int(m, sizeof(m), o.status);
    }
    if (o.conn) {
        ot_cat(m, sizeof(m), ", connect ");
        ot_cat_int(m, sizeof(m), o.conn);
    }
    log_line(m);
}

bool json_get(int src, const char *path, const char *addr, JReply *r, char *why, int whyn, int *rc_out)
{
    r->text = 0;
    r->t.root = 0;
    r->t.pool = 0;
    HttpOut o;
    int rc = http_get(env(), req_for(src, path, addr, ONLINE_JSON_MAX), &o);
    if (rc_out) {
        *rc_out = rc;
    }
    if (rc != HTTP_OK) {
        say_failed(why, whyn, src, rc, o);
        log_failed(src, path, rc, o);
        return false;
    }
    r->text = o.body;
    r->t = json_parse((char *)o.body, (int)o.len, tree_alloc, 0);
    if (!r->t.root) {
        ot_copy(why, whyn, "The answer of ");
        ot_cat(why, whyn, k_src[src]);
        ot_cat(why, whyn, " is not readable");
        g_api->mem->free(r->text);
        r->text = 0;
        return false;
    }
    return true;
}

void json_free(JReply *r)
{
    if (r->t.pool) {
        g_api->mem->free(r->t.pool);
    }
    if (r->text) {
        g_api->mem->free(r->text);
    }
    r->t.pool = 0;
    r->text = 0;
    r->t.root = 0;
}

// the words of an Assembly64 / CommoServe value that has no name: a rating ">=5" is "5 or more", a file type "d64" is "D64", a year is itself
void value_words(char *dst, int n, const char *type, const char *key, const char *name)
{
    if (name && name[0]) {
        ot_fold(dst, n, name);
        return;
    }
    if (!strcmp(type, "rating") && key[0] == '>' && key[1] == '=') {
        ot_copy(dst, n, key + 2);
        ot_cat(dst, n, " or more");
        return;
    }
    ot_fold(dst, n, key);
    if (!strcmp(type, "type")) {
        for (char *p = dst; *p; p++) {
            if (*p >= 'a' && *p <= 'z') {
                *p = (char)(*p - 32);
            }
        }
    }
}

// a value of /api/c64/filters as the form shows it: "title" is "Title"
void cap_words(char *dst, int n, const char *s)
{
    ot_fold(dst, n, s);
    if (dst[0] >= 'a' && dst[0] <= 'z') {
        dst[0] = (char)(dst[0] - 32);
    }
}

bool loadable_name(const char *n)
{
    return ot_iends(n, ".prg") || ot_iends(n, ".d64") || ot_iends(n, ".d81");
}

// ---- the parsers (on the job's task: they fill the arrays they are given, the job's own) ----------------------------------------------------------------------
bool parse_presets(const JReply &jr, Presets *out)
{
    bool listed = jr.t.root->type == J_LIST;
    out->n = 0;
    for (const JNode *e = listed ? jr.t.root->child : 0; e && out->n < ONLINE_PRESETS; e = e->next) {
        const JNode *v = json_member(e, "values");
        const char *type = json_text(e, "type");
        if (e->type != J_OBJ || !type[0] || !v || v->type != J_LIST) {
            continue;
        }
        Preset &pr = out->p[out->n++];
        memset(&pr, 0, sizeof(pr));
        ot_copy(pr.type, sizeof(pr.type), type);
        for (const JNode *ve = v->child; ve && pr.n < MAX_VALUES; ve = ve->next) {
            const char *k = json_text(ve, "aqlKey");
            if (ve->type != J_OBJ || !k[0]) {
                continue;
            }
            ot_copy(pr.key[pr.n], sizeof(pr.key[0]), k);
            ot_fold(pr.name[pr.n], sizeof(pr.name[0]), json_text(ve, "name"));
            pr.id[pr.n] = (int16_t)json_int(ve, "id", -1);
            pr.n++;
        }
    }
    return out->n > 0;
}

void blist(const JNode *root, const char *key, BList *out)
{
    out->n = 0;
    const JNode *l = json_member(root, key);
    for (const JNode *e = l && l->type == J_LIST ? l->child : 0; e && out->n < 12; e = e->next) {
        const char *v = 0, *lab = 0;
        if (e->type == J_STR) {
            v = lab = e->str;
        } else if (e->type == J_LIST && e->child && e->child->type == J_STR) {               // ai: [value, label]
            v = e->child->str;
            lab = e->child->next && e->child->next->type == J_STR ? e->child->next->str : v;
        }
        if (!v || !v[0] || ot_len(v) >= (int)sizeof(out->v[0])) {
            continue;
        }
        ot_copy(out->v[out->n], sizeof(out->v[0]), v);
        cap_words(out->label[out->n], sizeof(out->label[0]), lab);
        out->n++;
    }
}

bool parse_filters(const JReply &jr, BFilters *out)
{
    const JNode *r = jr.t.root;
    blist(r, "c", &out->c);
    blist(r, "pl", &out->pl);
    blist(r, "e", &out->e);
    blist(r, "h", &out->h);
    blist(r, "ai", &out->ai);
    blist(r, "f", &out->f);
    return out->c.n > 0;
}

void join_words(char *dst, int n, const char *a, const char *b)
{
    if (a[0] && b[0]) {
        ot_copy(dst, n, a);
        ot_cat(dst, n, DOT);
        ot_cat(dst, n, b);
    } else {
        ot_copy(dst, n, a[0] ? a : b);
    }
}

// Assembly64, CommoServe: a list of results. side[1] gets the category's name when the results are taken (collect: the presets are the menu's).
void parse_results(const JReply &jr, Res *out, int *n)
{
    *n = 0;
    for (const JNode *e = jr.t.root->type == J_LIST ? jr.t.root->child : 0; e && *n < MAX_PAGE; e = e->next) {
        if (e->type != J_OBJ) {
            continue;
        }
        Res &x = out[*n];
        memset(&x, 0, sizeof(x));
        ot_fold_cut(x.title, sizeof(x.title), json_text(e, "name"), 61);
        if (!x.title[0]) {
            continue;
        }
        ot_copy(x.id, sizeof(x.id), json_text(e, "id"));
        x.cat = json_int(e, "category", 0);
        int year = json_int(e, "year", 0);
        if (year > 0) {
            ot_cat_int(x.right, sizeof(x.right), year);
        }
        char g[64], h[64];
        ot_fold(g, sizeof(g), json_text(e, "group"));
        ot_fold(h, sizeof(h), json_text(e, "handle"));
        char t[128];
        join_words(t, sizeof(t), g, h);
        ot_fold_cut(x.side[0], sizeof(x.side[0]), t, 63);
        ot_fold_cut(x.side[1], sizeof(x.side[1]), json_text(e, "released"), 40);
        int rating = json_int(e, "rating", 0);
        char rt[24] = "";
        if (rating > 0) {
            ot_copy(rt, sizeof(rt), "Rating ");
            ot_cat_int(rt, sizeof(rt), rating);
        }
        char ev[64] = "";
        const char *evt = json_text(e, "event");
        if (evt[0]) {
            ot_copy(ev, sizeof(ev), "Event ");
            char f[56];
            ot_fold(f, sizeof(f), evt);
            ot_cat(ev, sizeof(ev), f);
        }
        join_words(t, sizeof(t), rt, ev);
        ot_fold_cut(x.side[2], sizeof(x.side[2]), t, 63);
        x.loadable = true;
        (*n)++;
    }
}

void parse_entries(const JReply &jr, Ent *out, int *n)
{
    *n = 0;
    const JNode *ce = json_member(jr.t.root, "contentEntry");
    int i = 0;
    for (const JNode *e = ce && ce->type == J_LIST ? ce->child : 0; e && *n < MAX_ENT; e = e->next, i++) {
        if (e->type != J_OBJ) {
            continue;
        }
        const char *p = json_text(e, "path");
        const char *base = strrchr(p, '/');
        clean(out[*n].name, sizeof(out[*n].name), base ? base + 1 : p);
        out[*n].idx = json_int(e, "id", i);
        out[*n].size = (uint32_t)json_int(e, "size", 0);
        if (out[*n].name[0]) {
            (*n)++;
        }
    }
}

// Beyond64: the files of a release the download bridge serves (.prg, .d64, .d81 by f[].n, U21 §4) into `out` (at most `max`; null: only counted); idx is the file's
// place in f, which the download names
void b64_files(const JNode *rel, Ent *out, int *n, int max)
{
    *n = 0;
    const JNode *f = json_member(rel, "f");
    int i = 0;
    for (const JNode *e = f && f->type == J_LIST ? f->child : 0; e; e = e->next, i++) {
        const char *nm = json_text(e, "n");
        if (e->type != J_OBJ || !loadable_name(nm)) {
            continue;
        }
        if (out) {
            if (*n >= max) {
                break;
            }
            clean(out[*n].name, sizeof(out[*n].name), nm);
            out[*n].idx = i;
            out[*n].size = FS_SIZE_UNKNOWN;
        }
        (*n)++;
    }
}

void words_of_list(char *dst, int n, const JNode *l)
{
    dst[0] = 0;
    for (const JNode *e = l && l->type == J_LIST ? l->child : 0; e; e = e->next) {
        if (e->type != J_STR || !e->str[0]) {
            continue;
        }
        if (dst[0]) {
            ot_cat(dst, n, ", ");
        }
        char w[48];
        ot_fold(w, sizeof(w), e->str);
        ot_cat(dst, n, w);
    }
}

// Beyond64: what /api/c64/release/{id} says of a result: its date, categories, platforms and description, and whether a file of it can be loaded here (D4, D5)
void b64_details(const JNode *rel, Res &x)
{
    char a[64], d[24], t[160];
    ot_copy(a, sizeof(a), x.side[0]);
    ot_fold(d, sizeof(d), json_text(rel, "d"));
    join_words(t, sizeof(t), a, d);
    ot_fold_cut(x.side[0], sizeof(x.side[0]), t, 63);
    char cs[96], pl[48];
    words_of_list(cs, sizeof(cs), json_member(rel, "cs"));
    words_of_list(pl, sizeof(pl), json_member(rel, "pl"));
    join_words(t, sizeof(t), cs, pl);
    ot_fold_cut(x.side[1], sizeof(x.side[1]), t, 63);
    ot_fold_cut(x.side[2], sizeof(x.side[2]), json_text(rel, "x"), 63);
    int nf = 0;
    b64_files(rel, 0, &nf, 0);
    x.loadable = nf > 0;
    x.nfiles = nf;
    if (nf == 1) {
        Ent one;
        int k = 0;
        b64_files(rel, &one, &k, 1);
        ot_copy(x.file1, sizeof(x.file1), one.name);
        x.file1_idx = one.idx;
    }
}

// Beyond64: one page of releases, then the details of each (the list carries no files, D5). Logs the time of both (U21 D8).
void b64_page(Work &w)
{
    char addr[20];
    addr[0] = 0;
    if (SVC_HAS(g_api->net, resolve) && g_api->net->resolve(B64_HOST, addr, sizeof(addr)) < 0) {
        addr[0] = 0;                                         // connect resolves the name itself and says it when it fails
        log_line("online: Beyond64: the name did not resolve, each request resolves it");
    }
    uint32_t t0 = now_ms();
    JReply jr;
    int rc;
    if (!json_get(S_B64, w.path, addr, &jr, w.err, sizeof(w.err), &rc)) {
        return;
    }
    const JNode *list = json_member(jr.t.root, "r");
    w.more = json_bool(jr.t.root, "more", false);
    w.nres = 0;
    for (const JNode *e = list && list->type == J_LIST ? list->child : 0; e && w.nres < MAX_PAGE; e = e->next) {
        int id = json_int(e, "id", 0);
        if (e->type != J_OBJ || id <= 0) {
            continue;
        }
        Res &x = w.res[w.nres];
        memset(&x, 0, sizeof(x));
        ot_cat_int(x.id, sizeof(x.id), id);
        ot_fold_cut(x.title, sizeof(x.title), json_text(e, "t"), 61);
        if (!x.title[0]) {
            ot_copy(x.title, sizeof(x.title), "(no title)");
        }
        ot_fold_cut(x.right, sizeof(x.right), json_text(e, "c"), 39);
        ot_fold_cut(x.side[0], sizeof(x.side[0]), json_text(e, "a"), 40);
        w.nres++;
    }
    json_free(&jr);
    uint32_t t1 = now_ms();
    for (int i = 0; i < w.nres; i++) {
        if (work_cancelled(0)) {
            ot_copy(w.err, sizeof(w.err), "Cancelled");
            return;
        }
        char path[64];
        ot_copy(path, sizeof(path), B64_RELEASE);
        ot_cat(path, sizeof(path), w.res[i].id);
        char why[96];
        if (!json_get(S_B64, path, addr, &jr, why, sizeof(why), &rc)) {
            if (rc == HTTP_STATUS) {
                continue;                                    // a release the server does not show (404): nothing of it can be loaded
            }
            ot_copy(w.err, sizeof(w.err), why);
            return;
        }
        b64_details(jr.t.root, w.res[i]);
        json_free(&jr);
    }
    uint32_t t2 = now_ms();
    char m[160] = "online: Beyond64 page ";
    ot_cat_int(m, sizeof(m), w.page);
    ot_cat(m, sizeof(m), ": ");
    ot_cat_int(m, sizeof(m), w.nres);
    ot_cat(m, sizeof(m), " releases, the list ");
    ot_cat_int(m, sizeof(m), (long)(t1 - t0));
    ot_cat(m, sizeof(m), " ms, their details ");
    ot_cat_int(m, sizeof(m), (long)(t2 - t1));
    ot_cat(m, sizeof(m), " ms, all ");
    ot_cat_int(m, sizeof(m), (long)(t2 - t0));
    ot_cat(m, sizeof(m), " ms");
    log_line(m);
    w.ok = true;
}

// ---- the job ------------------------------------------------------------------------------------------------------------------------------------------------
// The function of the job: the request of g_w, then the answer in g_w. The only task that connects, sends and receives.
int work_job(SvcJobH *j, void *)
{
    Work &w = g_w;
    w.jh = j;
    w.ok = false;
    w.err[0] = 0;
    w.body = 0;
    w.nres = 0;
    w.nent = 0;
    w.more = false;
    if (w.kind == W_DOWNLOAD) {
        HttpOut o;
        int rc = http_get(env(), req_for(w.src, w.path, 0, w.max), &o);
        if (rc == HTTP_OK) {
            w.body = o.body;
            w.len = o.len;
            w.ok = true;
        } else {
            say_failed(w.err, sizeof(w.err), w.src, rc, o);
            log_failed(w.src, w.path, rc, o);
        }
    } else if ((w.kind == W_SEARCH || w.kind == W_MORE) && w.src == S_B64) {
        b64_page(w);
    } else {
        JReply jr;
        if (json_get(w.src, w.path, 0, &jr, w.err, sizeof(w.err), 0)) {
            if (w.kind == W_PRESETS) {
                w.ok = parse_presets(jr, &w.pre);
                if (!w.ok) {
                    ot_copy(w.err, sizeof(w.err), "The server gave no choices");
                }
            } else if (w.kind == W_FILTERS) {
                w.ok = parse_filters(jr, &w.bf);
                if (!w.ok) {
                    ot_copy(w.err, sizeof(w.err), "Beyond64 gave no choices");
                }
            } else if (w.kind == W_SEARCH) {
                parse_results(jr, w.res, &w.nres);
                w.ok = true;
            } else if (w.src == S_B64) {
                b64_files(jr.t.root, w.ent, &w.nent, MAX_ENT);
                w.ok = true;
            } else {
                parse_entries(jr, w.ent, &w.nent);
                w.ok = true;
            }
            json_free(&jr);
        }
    }
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    w.state = 2;
    return 0;
}

void drop_entries(void)
{
    ent_for = -1;
    nent = 0;
}

void drop_download(void)
{
    if (dl_b && g_api) {
        g_api->mem->free(dl_b);
    }
    dl_b = 0;
    dl_len = 0;
    dl_res = dl_idx = -1;
}

int find_folder(const char *folder)
{
    for (int i = 0; i < g_res.n; i++) {
        if (ot_ieq(g_res.r[i].folder, folder)) {
            return i;
        }
    }
    return -1;
}

bool folder_taken(const char *f, int upto)
{
    for (int i = 0; i < upto; i++) {
        if (ot_ieq(g_res.r[i].folder, f)) {
            return true;
        }
    }
    return false;
}

// the folder name of result i: its title, cut, and " (2)", " (3)" ... when an earlier result has it already (finding 12: no "#5")
void name_folder(int i)
{
    Res &x = g_res.r[i];
    char base[52];
    clean(base, sizeof(base), x.title);
    ot_copy(x.folder, sizeof(x.folder), base);
    for (int k = 2; k < 1000 && folder_taken(x.folder, i); k++) {
        ot_copy(x.folder, sizeof(x.folder), base);
        ot_cat(x.folder, sizeof(x.folder), " (");
        ot_cat_int(x.folder, sizeof(x.folder), k);
        ot_cat(x.folder, sizeof(x.folder), ")");
    }
}

// Assembly64: the name of a result's category, from the presets' subcategories (a result's `category` is their id); CommoServe names none
const char *cat_name(int src, int cat)
{
    if (src != S_A64 || !g_pre_ok[S_A64]) {
        return "";
    }
    for (int p = 0; p < g_pre[S_A64].n; p++) {
        const Preset &pr = g_pre[S_A64].p[p];
        if (strcmp(pr.type, "subcat")) {
            continue;
        }
        for (int v = 0; v < pr.n; v++) {
            if (pr.id[v] == cat) {
                return pr.name[v];
            }
        }
    }
    return "";
}

// The results of a page are added to g_res: Beyond64's are filtered by Show (D4, D8); a result of Assembly64 gets its category's name.
void take_page(const Work &w)
{
    for (int i = 0; i < w.nres && g_res.n < MAX_RES; i++) {
        const Res &x = w.res[i];
        g_res.looked++;
        if (w.src == S_B64 && !x.loadable && !g_res.show_all) {
            g_res.hidden++;
            continue;
        }
        Res &d = g_res.r[g_res.n];
        d = x;
        if (w.src == S_B64 && !x.loadable) {
            ot_copy(d.right, sizeof(d.right), "Only on a computer");
        }
        const char *cn = cat_name(w.src, x.cat);
        if (cn[0]) {
            char t[128];
            join_words(t, sizeof(t), x.side[1], cn);
            ot_fold_cut(d.side[1], sizeof(d.side[1]), t, 63);
        }
        name_folder(g_res.n);
        g_res.n++;
    }
    g_res.more = w.more;
    g_res.page = w.page;
    g_res.gen++;
}

void prune_choices(void);

// Takes over the result of a job that has ended. Any task that looks at the state of the search calls it first; the mutex keeps two of them from taking the same result.
void collect(void)
{
    if (g_w.state != 2) {
        return;
    }
    const SvcSync *sy = g_api ? g_api->sync : 0;
    bool locked = g_mx && sy && sy->mutex_lock && sy->mutex_lock(g_mx, SYNC_FOREVER);
    if (g_w.state == 2) {
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        bool want = g_w.ok && !g_w.abandon;
        int k = g_w.kind;
        if (k == W_PRESETS || k == W_FILTERS) {
            if (want && k == W_PRESETS) {
                g_pre[g_w.src] = g_w.pre;
                g_pre_ok[g_w.src] = true;
            } else if (want) {
                g_bf = g_w.bf;
                g_bf_ok = true;
            }
            ot_copy(g_choice_err[g_w.src], sizeof(g_choice_err[0]), want ? "" : g_w.err);
            if (want) {
                prune_choices();
            }
        } else if (k == W_SEARCH) {
            g_out.fresh = true;
            g_out.ok = want;
            g_out.empty = false;
            if (want) {
                int keep = 0, loadable = 0;
                for (int i = 0; i < g_w.nres; i++) {
                    loadable += g_w.res[i].loadable;
                }
                keep = g_s.show_all ? g_w.nres : loadable;
                bool take = g_w.src == S_B64 ? (g_w.nres > 0 && (keep > 0 || g_w.more)) : g_w.nres > 0;
                if (take) {
                    g_res.n = 0;
                    g_res.src = g_w.src;
                    g_res.looked = g_res.hidden = 0;
                    g_res.show_all = g_s.show_all;
                    ot_copy(g_res.text, sizeof(g_res.text), g_s.text);
                    ot_copy(g_res.query, sizeof(g_res.query), g_s.query);
                    ot_copy(g_res.path, sizeof(g_res.path), g_s.path);
                    g_more_err[0] = 0;
                    take_page(g_w);
                    drop_entries();
                    drop_download();
                    g_pf.kind = 0;                          // a failure told for an index of the old results is not the new results'
                    err[0] = 0;
                    g_out.shown = g_res.n;
                } else {
                    g_out.empty = true;
                    g_out.shown = g_w.nres;                 // Beyond64: releases there are, none loadable
                }
            }
        } else if (k == W_MORE) {
            if (want && g_w.gen == g_res.gen) {
                take_page(g_w);
                g_more_err[0] = 0;
            } else if (!want && !g_w.abandon) {
                ot_copy(g_more_err, sizeof(g_more_err), g_w.err);
            }
        } else if (want && k == W_ENTRIES && g_w.gen == g_res.gen) {
            memcpy(ent, g_w.ent, sizeof(ent));
            nent = g_w.nent;
            ent_for = g_w.ri;
            ent_gen = g_w.gen;
        } else if (want && k == W_DOWNLOAD && g_w.gen == g_res.gen) {
            drop_download();
            dl_b = g_w.body;
            dl_len = g_w.len;
            dl_res = g_w.ri;
            dl_idx = g_w.ei;
            dl_gen = g_w.gen;
            g_w.body = 0;
        }
        if (g_w.body && g_api && g_api->mem) {              // a download nobody wants
            g_api->mem->free(g_w.body);
            g_w.body = 0;
        }
        if (!g_w.ok && !g_w.abandon && (k == W_ENTRIES || k == W_DOWNLOAD)) {
            ot_copy(err, sizeof(err), g_w.err);
            g_pf.kind = k;
            g_pf.ri = g_w.ri;
            g_pf.ei = g_w.ei;
            ot_copy(g_pf.text, sizeof(g_pf.text), g_w.err);
        }
        g_w.state = 0;
    }
    if (locked) {
        sy->mutex_unlock(g_mx);
    }
}

// Starts the job for a request. False, with the reason in `why`: another request is in flight (or its result is not taken: call collect first), or the base gave no task.
bool work_begin(int kind, int src, const char *path, uint32_t max, int ri, int ei, int page, char *why, int whyn)
{
    if (!g_api || !g_api->job || !g_api->job->start) {
        ot_copy(why, whyn, "No task for the request");
        return false;
    }
    const SvcSync *sy = g_api->sync;
    bool locked = g_mx && sy && sy->mutex_lock && sy->mutex_lock(g_mx, SYNC_FOREVER);   // the menu task (lists, prefetch) and an op run by REST or a key row both start requests
    if (g_w.state != 0) {
        if (locked) {
            sy->mutex_unlock(g_mx);
        }
        ot_copy(why, whyn, "The server is being asked for something else: try again in a moment");
        return false;
    }
    g_w.kind = kind;
    g_w.src = src;
    g_w.gen = g_res.gen;
    ot_copy(g_w.path, sizeof(g_w.path), path);
    g_w.max = max;
    g_w.ri = ri;
    g_w.ei = ei;
    g_w.page = page;
    g_w.abandon = false;
    g_w.jh = 0;
    g_w.state = 1;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    bool started = g_api->job->start(work_job, 0, WORK_STACK);   // an ended job that nobody joined gives its slot to this one (api.md 4.6.6); the menu task never joins (4.6.11.3)
    if (!started) {
        g_w.state = 0;
    }
    if (locked) {
        sy->mutex_unlock(g_mx);
    }
    if (!started) {
        ot_copy(why, whyn, "No free task for the request");
        return false;
    }
    return true;
}

// Does the menu step this operation (api.md 4.14.9)? A caller that does not (REST, a key row) is not made to wait: the request goes out and the answer lands in the states.
bool stepping(void)
{
    return g_api && g_api->ctx && g_api->ctx->job_step && g_api->ctx->job_step();
}

bool menu_open(void)
{
    return g_api && LIB_API_COVERS(g_api, ctx) && SVC_HAS(g_api->ctx, state) && (g_api->ctx->state() & CTX_MENU_OPEN);
}

// Is there a network to ask? The base's states of the wired and the wireless interface: an address that is not 0.0.0.0.
bool net_up(void)
{
    static const char *const st[] = { "net.eth_ip", "wifi.ip" };
    for (int i = 0; i < 2; i++) {
        const char *v = g_api && g_api->ops && g_api->ops->state ? g_api->ops->state(st[i], 0) : 0;
        if (v && v[0] >= '0' && v[0] <= '9' && strcmp(v, "0.0.0.0")) {
            return true;
        }
    }
    return false;
}

// The choices of a source (Assembly64, CommoServe: the presets; Beyond64: /api/c64/filters): kept, or asked for now. The lists and the form ask; with the menu closed (REST
// reads every state) or no network nothing goes out, and a failed load is tried again TRY_AGAIN_MS later at the earliest.
bool choices_ready(int src)
{
    collect();
    if (src == S_B64 ? g_bf_ok : g_pre_ok[src]) {
        return true;
    }
    if (g_w.state != 0 || !menu_open() || !net_up()) {
        return false;
    }
    uint32_t t = now_ms();
    if (g_tried[src] && t - g_try_ms[src] < TRY_AGAIN_MS) {
        return false;
    }
    g_tried[src] = true;
    g_try_ms[src] = t;
    char why[96];
    if (!work_begin(src == S_B64 ? W_FILTERS : W_PRESETS, src, src == S_B64 ? B64_FILTERS : URL_PATTERNS, ONLINE_JSON_MAX, 0, 0, 0, why, sizeof(why))) {
        ot_copy(g_choice_err[src], sizeof(g_choice_err[0]), why);
    }
    return false;
}

bool choices_loading(int src)
{
    return g_w.state != 0 && (g_w.kind == W_PRESETS || g_w.kind == W_FILTERS) && g_w.src == src;
}

// ---- the form ------------------------------------------------------------------------------------------------------------------------------------------------
const Preset *preset_of(int src, const char *type)
{
    if (src == S_B64 || !g_pre_ok[src]) {
        return 0;
    }
    for (int i = 0; i < g_pre[src].n; i++) {
        if (!strcmp(g_pre[src].p[i].type, type)) {
            return &g_pre[src].p[i];
        }
    }
    return 0;
}

// does the source have choice i? The server's presets say it once they are here; until then the types measured on the servers stand
bool has_choice(int src, int i)
{
    if (src == S_B64) {
        return false;
    }
    if (g_pre_ok[src]) {
        return preset_of(src, k_ach[i].type) != 0;
    }
    return (k_ach[i].libs >> src) & 1;
}

const BList *blist_of(char filt)
{
    switch (filt) {
    case 'c': return &g_bf.c;
    case 'p': return &g_bf.pl;
    case 'e': return &g_bf.e;
    case 'h': return &g_bf.h;
    case 'a': return &g_bf.ai;
    case 'f': return &g_bf.f;
    default: return 0;
    }
}

// does the comma list `set` hold `v`?
bool set_has(const char *set, const char *v)
{
    int l = ot_len(v);
    for (const char *p = set; *p; ) {
        const char *c = strchr(p, ',');
        int n = c ? (int)(c - p) : ot_len(p);
        if (n == l && !strncmp(p, v, (size_t)n)) {
            return true;
        }
        if (!c) {
            break;
        }
        p = c + 1;
    }
    return false;
}

int set_count(const char *set)
{
    if (!set[0]) {
        return 0;
    }
    int n = 1;
    for (const char *p = set; *p; p++) {
        n += *p == ',';
    }
    return n;
}

// `v` into or out of the comma list `set` (n bytes)
void set_toggle(char *set, int n, const char *v)
{
    char out[200];
    out[0] = 0;
    bool had = false;
    for (const char *p = set; *p; ) {
        const char *c = strchr(p, ',');
        int l = c ? (int)(c - p) : ot_len(p);
        char one[64];
        ot_copy(one, l + 1 < (int)sizeof(one) ? l + 1 : (int)sizeof(one), p);
        if (!strcmp(one, v)) {
            had = true;
        } else if (one[0]) {
            if (out[0]) {
                ot_cat(out, sizeof(out), ",");
            }
            ot_cat(out, sizeof(out), one);
        }
        if (!c) {
            break;
        }
        p = c + 1;
    }
    if (!had) {
        if (out[0]) {
            ot_cat(out, sizeof(out), ",");
        }
        ot_cat(out, sizeof(out), v);
    }
    ot_copy(set, n, out);
}

// the fixed values of a Beyond64 field that /api/c64/filters does not list: value, words
struct Fixed {
    const char *v;
    const char *w;
};
const Fixed k_catm[] = { { "", "Any of them" }, { "and", "All of them" } };
const Fixed k_hwm[] = { { "", "All of them" }, { "or", "Any of them" } };
const Fixed k_show[] = { { "", "Loadable only" }, { "all", "All releases" } };

const Fixed *fixed_of(int f, int *n)
{
    *n = 2;
    return f == B_CATM ? k_catm : f == B_HWM ? k_hwm : f == B_SHOW ? k_show : (*n = 0, (const Fixed *)0);
}

// the words of value v of Beyond64 field f
void bvalue_words(char *dst, int n, int f, const char *v)
{
    if (!v[0]) {
        ot_copy(dst, n, k_bf[f].any);
        return;
    }
    int nf;
    const Fixed *fx = fixed_of(f, &nf);
    for (int i = 0; i < nf; i++) {
        if (!strcmp(fx[i].v, v)) {
            ot_copy(dst, n, fx[i].w);
            return;
        }
    }
    if (f == B_LOOK && !strcmp(v, "*")) {
        ot_copy(dst, n, "All five");
        return;
    }
    const BList *l = blist_of(k_bf[f].filt);
    for (int i = 0; l && i < l->n; i++) {
        if (!strcmp(l->v[i], v)) {
            ot_copy(dst, n, l->label[i]);
            return;
        }
    }
    cap_words(dst, n, v);
}

// does value v of Beyond64 field f exist (in the filters the server gave, or the fixed ones)?
bool bvalue_known(int f, const char *v)
{
    int nf;
    const Fixed *fx = fixed_of(f, &nf);
    for (int i = 0; i < nf; i++) {
        if (!strcmp(fx[i].v, v)) {
            return true;
        }
    }
    if (f == B_LOOK && !strcmp(v, "*")) {
        return true;
    }
    const BList *l = blist_of(k_bf[f].filt);
    for (int i = 0; l && i < l->n; i++) {
        if (!strcmp(l->v[i], v)) {
            return true;
        }
    }
    return false;
}

// The form keeps only what the source it shows has (U21 §2 "changing the source drops the choices the new source does not have", finding 4): the fields of another source are
// emptied, and a value the server's own lists do not offer (once they are here) goes.
void prune_choices(void)
{
    int s = g_f.src;
    if (s == S_B64) {
        g_f.group[0] = g_f.handle[0] = g_f.event[0] = 0;
        for (int i = 0; i < NACH; i++) {
            g_f.ach[i][0] = 0;
        }
        if (g_bf_ok) {
            for (int f = 0; f < NBF; f++) {
                if (!k_bf[f].multi) {
                    if (g_f.b[f][0] && !bvalue_known(f, g_f.b[f])) {
                        g_f.b[f][0] = 0;
                    }
                    continue;
                }
                char keep[200];
                keep[0] = 0;
                for (const char *p = g_f.b[f]; *p; ) {
                    const char *c = strchr(p, ',');
                    int l = c ? (int)(c - p) : ot_len(p);
                    char one[64];
                    ot_copy(one, l + 1 < (int)sizeof(one) ? l + 1 : (int)sizeof(one), p);
                    if (one[0] && bvalue_known(f, one)) {
                        if (keep[0]) {
                            ot_cat(keep, sizeof(keep), ",");
                        }
                        ot_cat(keep, sizeof(keep), one);
                    }
                    if (!c) {
                        break;
                    }
                    p = c + 1;
                }
                ot_copy(g_f.b[f], sizeof(g_f.b[f]), keep);
            }
        }
        return;
    }
    for (int f = 0; f < NBF; f++) {
        g_f.b[f][0] = 0;
    }
    for (int i = 0; i < NACH; i++) {
        if (!g_f.ach[i][0]) {
            continue;
        }
        bool keep = has_choice(s, i);
        const Preset *p = preset_of(s, k_ach[i].type);
        if (keep && p) {
            keep = false;
            for (int v = 0; v < p->n && !keep; v++) {
                keep = !strcmp(p->key[v], g_f.ach[i]);
            }
        }
        if (!keep) {
            g_f.ach[i][0] = 0;
        }
    }
    if (!g_f.ach[ACH_SORT][0]) {
        g_f.ach[ACH_ORDER][0] = 0;
    }
}

void set_source(int s)
{
    g_f.src = s;
    prune_choices();
    choices_ready(s);
}

// the words of the chosen value of Assembly64 / CommoServe choice i
void avalue_words(char *dst, int n, int i)
{
    const char *k = g_f.ach[i];
    if (!k[0]) {
        ot_copy(dst, n, k_ach[i].any);
        return;
    }
    const Preset *p = preset_of(g_f.src, k_ach[i].type);
    for (int v = 0; p && v < p->n; v++) {
        if (!strcmp(p->key[v], k)) {
            value_words(dst, n, k_ach[i].type, k, p->name[v]);
            return;
        }
    }
    value_words(dst, n, k_ach[i].type, k, 0);
}

// the words of a multi value of Beyond64 (several, joined; cut short)
void bmulti_words(char *dst, int n, int f)
{
    const char *set = g_f.b[f];
    if (!set[0]) {
        ot_copy(dst, n, k_bf[f].any);
        return;
    }
    char out[200];
    out[0] = 0;
    for (const char *p = set; *p; ) {
        const char *c = strchr(p, ',');
        int l = c ? (int)(c - p) : ot_len(p);
        char one[64], w[40];
        ot_copy(one, l + 1 < (int)sizeof(one) ? l + 1 : (int)sizeof(one), p);
        bvalue_words(w, sizeof(w), f, one);
        if (out[0]) {
            ot_cat(out, sizeof(out), ", ");
        }
        ot_cat(out, sizeof(out), w);
        if (!c) {
            break;
        }
        p = c + 1;
    }
    ot_fold_cut(dst, n, out, 36);
}

void bfield_words(char *dst, int n, int f)
{
    if (k_bf[f].multi) {
        bmulti_words(dst, n, f);
    } else {
        bvalue_words(dst, n, f, g_f.b[f]);
    }
}

// What the form searches for, in words: `text` short (the title of the results: the text, else the first filter), `query` all of it (the side panel).
void describe(char *text, int tn, char *query, int qn)
{
    text[0] = query[0] = 0;
    char part[96];
    auto add = [&](const char *label, const char *value) {
        part[0] = 0;
        if (label) {
            ot_copy(part, sizeof(part), label);
            ot_cat(part, sizeof(part), " ");
        }
        ot_cat(part, sizeof(part), value);
        if (!text[0]) {
            ot_fold_cut(text, tn, part, tn - 1 < 40 ? tn - 1 : 40);
        }
        if (query[0]) {
            ot_cat(query, qn, "; ");
        }
        ot_cat(query, qn, part);
    };
    if (g_f.name[0]) {
        add(0, g_f.name);
    }
    if (g_f.src == S_B64) {
        for (int f = 0; f < NBF; f++) {
            if (!g_f.b[f][0] || f == B_CATM || f == B_HWM) {
                continue;
            }
            char w[64];
            bfield_words(w, sizeof(w), f);
            add(k_bf[f].label, w);
        }
        if (!text[0]) {
            ot_copy(text, tn, "newest");
            ot_copy(query, qn, "The newest releases");
        }
        return;
    }
    if (g_f.group[0]) {
        add("Group", g_f.group);
    }
    if (g_f.handle[0]) {
        add("Handle", g_f.handle);
    }
    if (g_f.event[0]) {
        add("Event", g_f.event);
    }
    for (int i = 0; i < NACH; i++) {
        if (g_f.ach[i][0]) {
            char w[40];
            avalue_words(w, sizeof(w), i);
            add(k_ach[i].label, w);
        }
    }
}

void add_term(char *q, int qn, const char *field, const char *value, bool drop)
{
    if (q[0]) {
        ot_cat(q, qn, " & ");
    }
    ot_cat(q, qn, "(");
    ot_cat(q, qn, field);
    ot_cat(q, qn, ":");
    if (!drop) {
        ot_cat(q, qn, "\"");
    } else if (ot_ieq(field, "rating") && value[0] != '>') {
        ot_cat(q, qn, ">=");
    }
    ot_cat(q, qn, value);
    if (!drop) {
        ot_cat(q, qn, "\"");
    }
    ot_cat(q, qn, ")");
}

void param(char *path, int n, const char *key, const char *value)
{
    ot_cat(path, n, "&");
    ot_cat(path, n, key);
    ot_cat(path, n, "=");
    url_cat(path, n, value);
}

// ot_cat cuts at the end of the buffer: a request that filled it may be cut and is not sent. The texts hold up to 120 characters (ask_max), each byte that is no
// letter or digit takes three in the URL; SEARCH_PATH_MAX leaves http_get's REQ_MAX room for the request line's other words and the headers.
#define SEARCH_PATH_MAX 768
bool fits(const char *t, int n, char *why, int whyn)
{
    const int l = ot_len(t);
    if (l < n - 1 && l <= SEARCH_PATH_MAX) {
        return true;
    }
    ot_copy(why, whyn, "The search is too long: shorten a text");
    return false;
}

// The request of the form's search into `path`. False with the reason in `why`.
bool build_search(char *path, int n, char *why, int whyn)
{
    path[0] = 0;
    if (g_f.src == S_B64) {
        ot_copy(path, n, B64_RELEASES "?limit=");
        ot_cat_int(path, n, B64_LIMIT);
        ot_cat(path, n, "&page=1&s=added");
        if (g_f.name[0]) {
            param(path, n, "q", g_f.name);
            if (g_f.b[B_LOOK][0]) {
                param(path, n, "f", g_f.b[B_LOOK]);
            }
        }
        static const struct { int f; const char *key; } k[] = { { B_CAT, "c" }, { B_PLAT, "pl" }, { B_ENV, "e" }, { B_HW, "h" }, { B_AI, "ai" } };
        for (unsigned i = 0; i < sizeof(k) / sizeof(k[0]); i++) {
            if (g_f.b[k[i].f][0]) {
                param(path, n, k[i].key, g_f.b[k[i].f]);
            }
        }
        if (set_count(g_f.b[B_CAT]) > 1 && g_f.b[B_CATM][0]) {
            param(path, n, "cm", g_f.b[B_CATM]);
        }
        if (set_count(g_f.b[B_HW]) > 1 && g_f.b[B_HWM][0]) {
            param(path, n, "hm", g_f.b[B_HWM]);
        }
        return fits(path, n, why, whyn);
    }
    // the texts are free since base 3.6 (ask_any): a " would end the server's quoted value, (field:"value") has no escape for it
    const char *texts[] = { g_f.name, g_f.group, g_f.handle, g_f.event };
    for (unsigned i = 0; i < sizeof(texts) / sizeof(texts[0]); i++) {
        if (strchr(texts[i], '"')) {
            ot_copy(why, whyn, k_src[g_f.src]);
            ot_cat(why, whyn, " takes no \" in a text");
            return false;
        }
    }
    char q[600];
    q[0] = 0;
    if (g_f.name[0]) add_term(q, sizeof(q), "name", g_f.name, false);
    if (g_f.group[0]) add_term(q, sizeof(q), "group", g_f.group, false);
    if (g_f.handle[0]) add_term(q, sizeof(q), "handle", g_f.handle, false);
    if (g_f.event[0]) add_term(q, sizeof(q), "event", g_f.event, false);
    bool filter = q[0] != 0;
    for (int i = 0; i < NACH; i++) {
        if (g_f.ach[i][0] && has_choice(g_f.src, i)) {
            add_term(q, sizeof(q), k_ach[i].type, g_f.ach[i], true);
            filter = filter || (i != ACH_SORT && i != ACH_ORDER);
        }
    }
    if (!filter) {
        ot_copy(why, whyn, "Type a name or choose a filter first");
        return false;
    }
    if (!fits(q, (int)sizeof(q), why, whyn)) {
        return false;
    }
    ot_copy(path, n, URL_SEARCH);
    url_cat(path, n, q);
    return fits(path, n, why, whyn);
}

// ---- operations ---------------------------------------------------------------------------------------------------------------------------------------------
int src_of(const char *t)
{
    for (int s = 0; s < NSRC; s++) {
        if (ot_ieq(t, k_src[s])) {
            return s;
        }
    }
    return t[0] >= '0' && t[0] < '0' + NSRC && !t[1] ? t[0] - '0' : -1;
}

void say_value(OpOut &o, const char *label, const char *words)
{
    ot_copy(o.msg, o.msglen, label);
    ot_cat(o.msg, o.msglen, ": ");
    ot_cat(o.msg, o.msglen, words);
}

int op_set(CoreCtx *, const OpArgs &a, OpOut &o)
{
    const char *f = a.s(0);
    const char *t = a.s(1);
    while (*t == ' ') {
        t++;
    }
    char v[124];
    ot_copy(v, sizeof(v), t);
    for (char *p = v + strlen(v); p > v && p[-1] == ' '; ) {
        *--p = 0;
    }
    if (!strcmp(f, "source")) {
        int s = src_of(v);
        if (s < 0) {
            return OPE_ARG;
        }
        set_source(s);
        say_value(o, "Source", k_src[s]);
        return OPE_OK;
    }
    struct { const char *f; char *dst; int n; const char *label; } texts[] = {
        { "name", g_f.name, (int)sizeof(g_f.name), "Name" }, { "group", g_f.group, (int)sizeof(g_f.group), "Group" },
        { "handle", g_f.handle, (int)sizeof(g_f.handle), "Handle" }, { "event", g_f.event, (int)sizeof(g_f.event), "Event" },
    };
    for (unsigned i = 0; i < sizeof(texts) / sizeof(texts[0]); i++) {
        if (!strcmp(f, texts[i].f)) {
            if (i > 0 && g_f.src == S_B64) {
                ot_copy(o.msg, o.msglen, "Beyond64 has no such field");
                return OPE_FAILED;
            }
            ot_copy(texts[i].dst, texts[i].n, v);
            say_value(o, i == 0 && g_f.src == S_B64 ? "Text" : texts[i].label, v[0] ? v : "Any");
            return OPE_OK;
        }
    }
    if (f[0] == 'a' && f[1] == '.') {
        for (int i = 0; i < NACH; i++) {
            if (strcmp(f + 2, k_ach[i].type)) {
                continue;
            }
            if (!has_choice(g_f.src, i)) {
                ot_copy(o.msg, o.msglen, k_src[g_f.src]);
                ot_cat(o.msg, o.msglen, " has no such choice");
                return OPE_FAILED;
            }
            ot_copy(g_f.ach[i], sizeof(g_f.ach[i]), v);
            if (i == ACH_SORT && !v[0]) {
                g_f.ach[ACH_ORDER][0] = 0;
            }
            char w[40];
            avalue_words(w, sizeof(w), i);
            say_value(o, k_ach[i].label, w);
            return OPE_OK;
        }
        return OPE_ARG;
    }
    if (f[0] == 'b' && f[1] == '.') {
        for (int i = 0; i < NBF; i++) {
            if (strcmp(f + 2, k_bf[i].key)) {
                continue;
            }
            if (g_f.src != S_B64) {
                ot_copy(o.msg, o.msglen, k_src[g_f.src]);
                ot_cat(o.msg, o.msglen, " has no such choice");
                return OPE_FAILED;
            }
            if (k_bf[i].multi && v[0]) {
                set_toggle(g_f.b[i], sizeof(g_f.b[i]), v);
            } else {
                ot_copy(g_f.b[i], sizeof(g_f.b[i]), v);
            }
            char w[48];
            bfield_words(w, sizeof(w), i);
            say_value(o, k_bf[i].label, w);
            return OPE_OK;
        }
    }
    return OPE_ARG;
}

int op_next_source(CoreCtx *, const OpArgs &, OpOut &o)
{
    set_source((g_f.src + 1) % NSRC);
    say_value(o, "Source", k_src[g_f.src]);
    return OPE_OK;
}

int op_clear(CoreCtx *, const OpArgs &, OpOut &o)
{
    int s = g_f.src;
    memset(&g_f, 0, sizeof(g_f));
    g_f.src = s;
    ot_copy(o.msg, o.msglen, "The form is empty");
    return OPE_OK;
}

void goto_place(const char *where)
{
    if (g_api && LIB_API_COVERS(g_api, overlay) && SVC_HAS(g_api->overlay, goto_screen)) {
        g_api->overlay->goto_screen(where);
    }
}

// The search (the node Search, `job`). First call: the request is made and its job started (or, while another request of the sub-app runs, started as soon as that has
// ended); a stepping menu gets `more` until the answer is taken. Then: nothing found -> the form stays and says so (no error, U21 §2); results -> the results tab.
int op_search(CoreCtx *, const OpArgs &, OpOut &o)
{
    o.more = false;
    bool step = stepping();
    if (g_w.form == W_SEARCH && step) {
        if (g_api->ctx->job_cancelled && g_api->ctx->job_cancelled()) {
            if (!g_s.waiting && g_w.state != 0 && g_w.kind == W_SEARCH) {
                g_w.abandon = true;                         // before collect: a result that came in the same tick is dropped
            }
            g_w.form = 0;
            g_s.waiting = false;
            collect();
            ot_copy(o.msg, o.msglen, "Cancelled");
            return OPE_OK;
        }
        collect();
        if (g_s.waiting) {
            char why[96];
            if (g_w.state == 0 && work_begin(W_SEARCH, g_s.src, g_s.path, ONLINE_JSON_MAX, 0, 0, 1, why, sizeof(why))) {
                g_s.waiting = false;
            }
            o.more = true;
            return OPE_OK;
        }
        if (!g_out.fresh) {
            if (g_w.state != 0) {
                o.more = true;
                return OPE_OK;
            }
            g_w.form = 0;                                   // the job is gone without an answer
            ot_copy(o.msg, o.msglen, "The request was lost");
            return OPE_FAILED;
        }
        g_out.fresh = false;
        g_w.form = 0;
        const char *src = k_src[g_s.src];
        ot_fold_cut(g_last, sizeof(g_last), g_s.text, 16);
        if (!g_out.ok) {
            ot_copy(o.msg, o.msglen, g_w.err[0] ? g_w.err : "The search failed");
            ot_copy(g_outcome, sizeof(g_outcome), "Failed");
            return OPE_FAILED;
        }
        if (g_out.empty) {
            if (g_out.shown > 0) {
                ot_copy(o.msg, o.msglen, "Nothing loadable in ");
            } else {
                ot_copy(o.msg, o.msglen, "Nothing found in ");
            }
            ot_cat(o.msg, o.msglen, src);
            ot_cat(o.msg, o.msglen, " for '");
            ot_cat(o.msg, o.msglen, g_s.text);
            ot_cat(o.msg, o.msglen, "'");
            if (g_out.shown > 0) {
                ot_cat(o.msg, o.msglen, ": Show All lists ");
                ot_cat_int(o.msg, o.msglen, g_out.shown);
            }
            ot_copy(g_outcome, sizeof(g_outcome), g_out.shown > 0 ? "Nothing loadable" : "Nothing found");
            return OPE_OK;
        }
        goto_place("F3/results");
        o.msg[0] = 0;
        ot_cat_int(o.msg, o.msglen, g_out.shown);
        ot_cat(o.msg, o.msglen, " found in ");
        ot_cat(o.msg, o.msglen, src);
        g_outcome[0] = 0;
        ot_cat_int(g_outcome, sizeof(g_outcome), g_out.shown);
        ot_cat(g_outcome, sizeof(g_outcome), " found");
        return OPE_OK;
    }
    if (g_w.form != 0 && !step) {                           // the menu is stepping a search: a call from REST or a key neither takes its answer nor starts another
        ot_copy(o.msg, o.msglen, "A search is running: try again in a moment");
        return OPE_FAILED;
    }
    if (!net_up()) {
        ot_copy(o.msg, o.msglen, "No network: set it up in F8 System > Network");
        return OPE_FAILED;
    }
    char why[96];
    if (!build_search(g_s.path, sizeof(g_s.path), why, sizeof(why))) {
        ot_copy(o.msg, o.msglen, why);
        return OPE_FAILED;
    }
    g_s.src = g_f.src;
    g_s.show_all = g_f.src == S_B64 && g_f.b[B_SHOW][0];
    describe(g_s.text, sizeof(g_s.text), g_s.query, sizeof(g_s.query));
    g_out.fresh = false;
    collect();
    g_s.waiting = false;
    if (!work_begin(W_SEARCH, g_s.src, g_s.path, ONLINE_JSON_MAX, 0, 0, 1, why, sizeof(why))) {
        if (!step || g_w.state == 0) {
            ot_copy(o.msg, o.msglen, why);
            return OPE_FAILED;
        }
        g_s.waiting = true;                                 // a list's or the Library's request runs: the search starts when it has ended
    }
    if (step) {
        g_w.form = W_SEARCH;
        o.more = true;
    } else {
        ot_copy(o.msg, o.msglen, "Asking ");
        ot_cat(o.msg, o.msglen, k_src[g_s.src]);
        ot_cat(o.msg, o.msglen, ": the results come to online.rows");
    }
    return OPE_OK;
}

bool have_entries(int ri);

int g_pick = -1, g_pick_gen = -1;          // the result whose files the list online.files shows (online.open asked for it)
char g_pick_err[96];                       // why they did not come: the list says it and does not ask again (each RETURN on a result asks anew)

// The file of a result in Browse with its action sheet open (browse_sheet, base 3.6), as RETURN on it there does. The folder must list at once: the result's entries are kept
// (have_entries), else Browse finds the folder still loading and the cursor stays at the top.
bool open_file(int i, const char *name, OpOut &o)
{
    if (!g_api || !SVC_HAS(g_api->ctx, browse_sheet)) {
        return false;
    }
    char path[FS_PATH_MAX];
    ot_copy(path, sizeof(path), "/Online/");
    ot_cat(path, sizeof(path), g_res.r[i].folder);
    ot_cat(path, sizeof(path), "/");
    ot_cat(path, sizeof(path), name);
    if (!g_api->ctx->browse_sheet(path)) {
        return false;
    }
    ot_copy(o.msg, o.msglen, name);
    return true;
}

// RETURN on a result (the list's `op`, api.md 6.19.3). A result whose one file is known (Beyond64 names it with the details; Assembly64 and CommoServe once its entries
// are kept) opens that file's action sheet in Browse. Any other: the list of its files (ctx.ask_list, the state online.files), which fills while they load; RETURN on a file
// (online.file) opens its action sheet. A result of Beyond64 with no file this machine can load says so instead (D4). Without browse_sheet or ask_list: its folder in the Library
// as before (lib.show "/Online/<result>", then F3).
int op_open(CoreCtx *, const OpArgs &a, OpOut &o)
{
    collect();
    int i = a.i(0);
    if (i < 0 || i >= g_res.n) {
        ot_copy(o.msg, o.msglen, "No such result");
        return OPE_FAILED;
    }
    const Res &x = g_res.r[i];
    if (!x.loadable) {
        ot_copy(o.msg, o.msglen, "Only on a computer: no .prg, .d64 or .d81 in ");
        ot_cat(o.msg, o.msglen, x.title);
        return OPE_OK;
    }
    const bool kept = (ent_for == i && ent_gen == g_res.gen) || (g_res.src == S_B64 && x.nfiles == 1 && have_entries(i));
    if (kept && nent == 1 && open_file(i, ent[0].name, o)) {
        return OPE_OK;
    }
    if (g_api && SVC_HAS(g_api->ctx, ask_list) && SVC_HAS(g_api->ctx, browse_sheet)) {
        g_pick = i;
        g_pick_gen = g_res.gen;
        g_pick_err[0] = 0;
        if (!kept) {
            have_entries(i);                                     // the request starts now; the list shows "Loading" until it has come
        }
        g_api->ctx->ask_list("online.file", i, x.title, "online.files");
        ot_copy(o.msg, o.msglen, x.title);
        ot_cat(o.msg, o.msglen, ": its files");
        return OPE_OK;
    }
    ot_copy(g_at, sizeof(g_at), "/Online/");
    ot_cat(g_at, sizeof(g_at), x.folder);
    ot_cat(g_at, sizeof(g_at), "/");
    if (g_api && g_api->ops && g_api->ops->call) {
        OpIn in = in_str("state", "online.at");
        OpResult rr;
        g_api->ops->call("lib.show", &in, 1, &rr);
    }
    goto_place("F3");
    ot_copy(o.msg, o.msglen, x.title);
    ot_cat(o.msg, o.msglen, ": its files");
    return OPE_OK;
}

// RETURN on a file of the list online.open asked for: phase is the result, answer the file's name.
int op_file(CoreCtx *, const OpArgs &a, OpOut &o)
{
    collect();
    int i = a.i(0);
    if (i < 0 || i >= g_res.n || i != g_pick || g_pick_gen != g_res.gen) {
        ot_copy(o.msg, o.msglen, "The results have changed: search again");
        return OPE_FAILED;
    }
    if (!have_entries(i)) {
        ot_copy(o.msg, o.msglen, err);
        return OPE_FAILED;
    }
    for (int k = 0; k < nent; k++) {
        if (ot_ieq(ent[k].name, a.s(1))) {
            if (open_file(i, ent[k].name, o)) {
                return OPE_OK;
            }
            ot_copy(o.msg, o.msglen, "Browse cannot open ");
            ot_cat(o.msg, o.msglen, ent[k].name);
            return OPE_FAILED;
        }
    }
    ot_copy(o.msg, o.msglen, "No such file");
    return OPE_FAILED;
}

// RETURN on the list's last row (`more`): Beyond64's next page, asked for in the background; the rows grow when it has come (the list reads its state again)
int op_more(CoreCtx *, const OpArgs &, OpOut &o)
{
    collect();
    if (g_res.src != S_B64 || !g_res.more) {
        ot_copy(o.msg, o.msglen, "There are no more results");
        return OPE_OK;
    }
    if (g_w.state != 0 && g_w.kind == W_MORE) {
        ot_copy(o.msg, o.msglen, "The next page is on its way");
        return OPE_OK;
    }
    char path[1024];
    ot_copy(path, sizeof(path), g_res.path);
    char *pg = (char *)ot_ifind(path, ot_len(path), "&page=1&");
    if (!pg) {
        ot_copy(o.msg, o.msglen, "The search is gone: search again");
        return OPE_FAILED;
    }
    char tail[1024];
    ot_copy(tail, sizeof(tail), pg + 7);
    pg[0] = 0;
    ot_cat(path, sizeof(path), "&page=");
    ot_cat_int(path, sizeof(path), g_res.page + 1);
    ot_cat(path, sizeof(path), tail);
    char why[96];
    if (!work_begin(W_MORE, S_B64, path, ONLINE_JSON_MAX, 0, 0, g_res.page + 1, why, sizeof(why))) {
        ot_copy(o.msg, o.msglen, why);
        return OPE_FAILED;
    }
    g_more_err[0] = 0;
    ot_copy(o.msg, o.msglen, "Asking Beyond64 for page ");
    ot_cat_int(o.msg, o.msglen, g_res.page + 1);
    return OPE_OK;
}

int op_new(CoreCtx *, const OpArgs &, OpOut &o)
{
    goto_place("F3/online");
    o.msg[0] = 0;
    return OPE_OK;
}

// The results go: the tab leaves the strip (show), the menu goes to the form
int op_forget(CoreCtx *, const OpArgs &, OpOut &o)
{
    collect();
    g_res.n = 0;
    g_res.more = false;
    g_res.looked = g_res.hidden = 0;
    g_res.gen++;
    drop_entries();
    drop_download();
    g_more_err[0] = 0;
    goto_place("F3/online");
    ot_copy(o.msg, o.msglen, "The results are cleared");
    return OPE_OK;
}

// ---- states -----------------------------------------------------------------------------------------------------------------------------------------------------
// A list of choices (api.md 6.19.2): one row a line, value TAB words TAB right; "chosen" at the right of what is chosen
struct ListBuf {
    char *b;
    int n;
};

void list_row(ListBuf &l, const char *value, const char *words, bool chosen)
{
    ot_cat(l.b, l.n, value);
    ot_cat(l.b, l.n, "\t");
    ot_cat(l.b, l.n, words);
    if (chosen) {
        ot_cat(l.b, l.n, "\tchosen\tcyan");
    }
    ot_cat(l.b, l.n, "\n");
}

// "!..." while a source's choices are not here (6.19.2: a note, shown and never taken)
bool list_wait(ListBuf &l, int src)
{
    if (choices_ready(src)) {
        return false;
    }
    if (choices_loading(src)) {
        ot_copy(l.b, l.n, "!Loading the choices from ");
        ot_cat(l.b, l.n, k_src[src]);
        ot_cat(l.b, l.n, " ...\n");
    } else if (g_choice_err[src][0]) {
        ot_copy(l.b, l.n, "!");
        ot_cat(l.b, l.n, g_choice_err[src]);
        ot_cat(l.b, l.n, "\n!Tried again in a moment\n");
    } else if (!net_up()) {
        ot_copy(l.b, l.n, "!No network: F8 System > Network\n");
    } else {
        ot_copy(l.b, l.n, "!Waiting for the server ...\n");
    }
    return true;
}

const char *st_src(CoreCtx *)
{
    choices_ready(g_f.src);                                 // the form is up: its source's choices are asked for now, so they are here when a list opens
    return k_src[g_f.src];
}
const char *st_ftitle(CoreCtx *)
{
    static char t[40];
    ot_copy(t, sizeof(t), "Search ");
    ot_cat(t, sizeof(t), k_src[g_f.src]);
    return t;
}
const char *st_lsource(CoreCtx *)
{
    static char b[256];
    ListBuf l = { b, (int)sizeof(b) };
    b[0] = 0;
    for (int s = 0; s < NSRC; s++) {
        list_row(l, k_src[s], k_src[s], s == g_f.src);
    }
    return b;
}
const char *st_lblname(CoreCtx *) { return g_f.src == S_B64 ? "Text" : "Name"; }
const char *any_of(const char *s) { return s[0] ? s : "Any"; }
const char *st_vname(CoreCtx *) { return any_of(g_f.name); }
const char *st_vgroup(CoreCtx *) { return any_of(g_f.group); }
const char *st_vhandle(CoreCtx *) { return any_of(g_f.handle); }
const char *st_vevent(CoreCtx *) { return any_of(g_f.event); }
const char *st_pname(CoreCtx *) { return g_f.name[0] ? g_f.name : 0; }
const char *st_pgroup(CoreCtx *) { return g_f.group[0] ? g_f.group : 0; }
const char *st_phandle(CoreCtx *) { return g_f.handle[0] ? g_f.handle : 0; }
const char *st_pevent(CoreCtx *) { return g_f.event[0] ? g_f.event : 0; }
const char *st_hac(CoreCtx *) { return g_f.src == S_B64 ? "yes" : 0; }
const char *st_hb(CoreCtx *) { return g_f.src != S_B64 ? "yes" : 0; }
const char *st_hbcatm(CoreCtx *) { return g_f.src != S_B64 || set_count(g_f.b[B_CAT]) < 2 ? "yes" : 0; }
const char *st_hbhwm(CoreCtx *) { return g_f.src != S_B64 || set_count(g_f.b[B_HW]) < 2 ? "yes" : 0; }

template <int I> const char *st_av(CoreCtx *)
{
    static char w[40];
    collect();
    avalue_words(w, sizeof(w), I);
    return w;
}
template <int I> const char *st_ah(CoreCtx *)
{
    collect();
    return !has_choice(g_f.src, I) || (I == ACH_ORDER && !g_f.ach[ACH_SORT][0]) ? "yes" : 0;
}
template <int I> const char *st_al(CoreCtx *)
{
    static char b[LIST_MAX];
    ListBuf l = { b, (int)sizeof(b) };
    b[0] = 0;
    int src = g_f.src;
    if (src == S_B64 || list_wait(l, src)) {
        return b;
    }
    const Preset *p = preset_of(src, k_ach[I].type);
    if (!p) {
        ot_copy(b, sizeof(b), "!");
        ot_cat(b, sizeof(b), k_src[src]);
        ot_cat(b, sizeof(b), " has no such choice\n");
        return b;
    }
    list_row(l, "", k_ach[I].any, !g_f.ach[I][0]);
    for (int v = 0; v < p->n; v++) {
        char w[40];
        value_words(w, sizeof(w), k_ach[I].type, p->key[v], p->name[v]);
        list_row(l, p->key[v], w, !strcmp(p->key[v], g_f.ach[I]));
    }
    return b;
}

template <int F> const char *st_bv(CoreCtx *)
{
    static char w[48];
    collect();
    bfield_words(w, sizeof(w), F);
    return w;
}
template <int F> const char *st_bl(CoreCtx *)
{
    static char b[2048];
    ListBuf l = { b, (int)sizeof(b) };
    b[0] = 0;
    const BDef &d = k_bf[F];
    if (d.filt && list_wait(l, S_B64)) {
        return b;
    }
    int nf;
    const Fixed *fx = fixed_of(F, &nf);
    if (fx) {
        for (int i = 0; i < nf; i++) {
            list_row(l, fx[i].v, fx[i].w, !strcmp(g_f.b[F], fx[i].v));
        }
        return b;
    }
    list_row(l, "", d.multi ? "Any (none chosen)" : d.any, !g_f.b[F][0]);
    if (F == B_LOOK) {
        list_row(l, "*", "All five", !strcmp(g_f.b[F], "*"));
    }
    const BList *bl = blist_of(d.filt);
    for (int i = 0; bl && i < bl->n; i++) {
        bool on = d.multi ? set_has(g_f.b[F], bl->v[i]) : !strcmp(g_f.b[F], bl->v[i]);
        list_row(l, bl->v[i], bl->label[i], on);
    }
    return b;
}

const char *st_busy(CoreCtx *)
{
    static char b[48];
    ot_copy(b, sizeof(b), "Searching ");
    ot_cat(b, sizeof(b), k_src[g_w.form == W_SEARCH ? g_s.src : g_f.src]);
    return b;
}
const char *st_net(CoreCtx *) { return net_up() ? "yes" : 0; }
const char *st_last(CoreCtx *) { return g_last[0] ? g_last : "None yet"; }
const char *st_outcome(CoreCtx *) { return g_outcome[0] ? g_outcome : 0; }
const char *st_never(CoreCtx *) { return g_outcome[0] ? 0 : "yes"; }

// ---- the results ----------------------------------------------------------------------------------------------------------------------------------------------------
bool more_running(void)
{
    return g_w.state != 0 && g_w.kind == W_MORE;
}

const char *st_shown(CoreCtx *)
{
    collect();
    return g_res.n > 0 || (g_res.src == S_B64 && g_res.looked > 0 && g_res.more) ? "yes" : 0;
}

void count_words(char *dst, int n)
{
    dst[0] = 0;
    ot_cat_int(dst, n, g_res.n);
    if (g_res.src == S_B64 && g_res.more) {
        ot_cat(dst, n, "+");
    }
}

const char *st_title(CoreCtx *)
{
    static char t[96];
    collect();
    char c[16];
    count_words(c, sizeof(c));
    ot_copy(t, sizeof(t), k_src[g_res.src]);
    ot_cat(t, sizeof(t), DOT);
    ot_cat(t, sizeof(t), g_res.text);
    ot_cat(t, sizeof(t), DOT);
    ot_cat(t, sizeof(t), c);
    return t;
}

const char *st_rsource(CoreCtx *) { return k_src[g_res.src]; }
// The side panel of the results. A value of the panel has the room of about 30 characters less its label (U21, ue2emu 2026-11-25: a longer one is drawn over the label),
// so the values are short: the query is cut, and what Beyond64 has only for a computer is a row of its own.
const char *st_rquery(CoreCtx *)
{
    static char t[24];
    ot_fold_cut(t, sizeof(t), g_res.query, 18);
    return t;
}
const char *st_rfound(CoreCtx *)
{
    static char t[32];
    collect();
    t[0] = 0;
    ot_cat_int(t, sizeof(t), g_res.n);
    if (g_res.src != S_B64) {
        ot_cat(t, sizeof(t), g_res.n >= 20 ? " (the most)" : "");     // the servers answer 20 at most and say nothing of more (§3 finding 11, D7)
        return t;
    }
    ot_cat(t, sizeof(t), " of ");
    ot_cat_int(t, sizeof(t), g_res.looked);
    ot_cat(t, sizeof(t), g_res.more ? " (more)" : "");
    return t;
}
const char *st_rhidden(CoreCtx *)
{
    static char t[12];
    t[0] = 0;
    ot_cat_int(t, sizeof(t), g_res.hidden);
    return t;
}
const char *st_rnohidden(CoreCtx *) { return g_res.src == S_B64 && g_res.hidden > 0 ? 0 : "yes"; }

// The rows of the results (api.md 6.19.2): value (the index), title, right column, tone, three side lines; Beyond64's last row asks for the next page.
const char *st_rows(CoreCtx *)
{
    static char b[ROWS_MAX];
    static int built = -1, built_more = -1;
    static char built_err[96];
    collect();
    int m = more_running() ? 1 : 0;
    if (built == g_res.gen && built_more == m && !strcmp(built_err, g_more_err)) {
        return b[0] ? b : 0;
    }
    built = g_res.gen;
    built_more = m;
    ot_copy(built_err, sizeof(built_err), g_more_err);
    b[0] = 0;
    for (int i = 0; i < g_res.n; i++) {
        const Res &x = g_res.r[i];
        char line[ROW_MAX];
        line[0] = 0;
        ot_cat_int(line, sizeof(line), i);
        ot_cat(line, sizeof(line), "\t");
        ot_cat(line, sizeof(line), x.title);
        ot_cat(line, sizeof(line), "\t");
        ot_cat(line, sizeof(line), x.right);
        ot_cat(line, sizeof(line), "\t");
        ot_cat(line, sizeof(line), x.loadable ? "" : "disabled");   // base 3.6: the whole row grey; RETURN on it still says why (op_open)
        for (int k = 0; k < 3; k++) {
            ot_cat(line, sizeof(line), "\t");
            ot_cat(line, sizeof(line), x.side[k]);
        }
        ot_cat(line, sizeof(line), "\n");
        ot_cat(b, sizeof(b), line);
    }
    if (g_res.src == S_B64) {
        if (!g_res.n && g_res.looked) {
            ot_cat(b, sizeof(b), "!None of the ");
            ot_cat_int(b, sizeof(b), g_res.looked);
            ot_cat(b, sizeof(b), " releases so far loads here\n");
        }
        if (m) {
            ot_cat(b, sizeof(b), "!Asking Beyond64 for page ");
            ot_cat_int(b, sizeof(b), g_res.page + 1);
            ot_cat(b, sizeof(b), " ...\n");
        } else if (g_more_err[0]) {
            ot_cat(b, sizeof(b), "!");
            ot_cat(b, sizeof(b), g_more_err);
            ot_cat(b, sizeof(b), "\n");
            if (g_res.more) {
                ot_cat(b, sizeof(b), "+Try again\n");
            }
        } else if (g_res.more) {
            ot_cat(b, sizeof(b), "+More from Beyond64\n");
        }
    }
    return b[0] ? b : 0;
}

// lib.show opens the folder of the file this state names: "/Online/<result>" (op_open sets it)
const char *st_at(CoreCtx *) { return g_at[0] ? g_at : 0; }

// ---- the folder /Online -----------------------------------------------------------------------------------------------------------------------------------
// Called on the menu task (netfs.cc): none of these connects, sends or receives. What is kept is answered at once; what is not kept is asked for by a prefetch (one job, Work) and the call says
// so (tab_error) and fails; a later call finds it ready, or the reason it failed (told once: the call after that asks again).
bool tab_has_results(void)
{
    collect();
    for (int i = 0; i < g_res.n; i++) {
        if (g_res.r[i].loadable) {
            return true;
        }
    }
    return false;
}
const char *tab_error(void) { return err; }
// D41 (L-3c): the prefetch of a result's files or of a file's bytes is still running (or has ended and is not taken over yet): the file manager waits and lists the folder again instead of failing once.
bool tab_loading(void)
{
    return g_w.state != 0 && (g_w.kind == W_ENTRIES || g_w.kind == W_DOWNLOAD);
}

// "<result>/<file>" into the result's index and the file's name (null: none)
int split_rel(const char *rel, const char **file)
{
    while (*rel == '/') {
        rel++;
    }
    const char *sl = strchr(rel, '/');
    char lab[64];
    size_t ll = sl ? (size_t)(sl - rel) : strlen(rel);
    ll = ll < sizeof(lab) - 1 ? ll : sizeof(lab) - 1;
    memcpy(lab, rel, ll);
    lab[ll] = 0;
    *file = sl && sl[1] ? sl + 1 : 0;
    int ri = find_folder(lab);
    return ri >= 0 && g_res.r[ri].loadable ? ri : -1;
}

// The job that runs is for this file of a result (the result's entries, or the file's download).
bool tab_loading_for(const char *rel)
{
    const char *file;
    if (g_w.state == 0 || (g_w.kind != W_ENTRIES && g_w.kind != W_DOWNLOAD)) {
        return false;
    }
    const int ri = split_rel(rel, &file);
    if (!file || ri < 0 || ri != g_w.ri || g_w.gen != g_res.gen) {
        return false;
    }
    if (g_w.kind == W_ENTRIES) {
        return true;
    }
    for (int i = 0; i < nent; i++) {
        if (ot_ieq(ent[i].name, file)) {
            return ent[i].idx == g_w.ei;
        }
    }
    return false;
}

// Asks for the request in the background, unless it is asked for already; the words say what the caller gets to read in tab_error. Always false: the caller does not have it yet.
bool prefetch(int kind, int ri, int ei, const char *path, uint32_t max, const char *loading)
{
    if (g_pf.kind == kind && g_pf.ri == ri && g_pf.ei == ei) {
        ot_copy(err, sizeof(err), g_pf.text);
        g_pf.kind = 0;
        return false;
    }
    if (g_w.state != 0 && g_w.kind == kind && g_w.ri == ri && g_w.ei == ei && g_w.gen == g_res.gen) {
        ot_copy(err, sizeof(err), loading);
        return false;
    }
    char why[96];
    ot_copy(err, sizeof(err), work_begin(kind, g_res.src, path, max, ri, ei, 0, why, sizeof(why)) ? loading : why);
    return false;
}

bool have_entries(int ri)
{
    if (ent_for == ri && ent_gen == g_res.gen) {
        return true;
    }
    const Res &x = g_res.r[ri];
    if (g_res.src == S_B64 && x.nfiles == 1 && x.file1[0]) {        // the details of the search named its one file: no request (online.open puts the cursor on it at once)
        memset(&ent[0], 0, sizeof(ent[0]));
        ot_copy(ent[0].name, sizeof(ent[0].name), x.file1);
        ent[0].idx = x.file1_idx;
        ent[0].size = FS_SIZE_UNKNOWN;
        nent = 1;
        ent_for = ri;
        ent_gen = g_res.gen;
        return true;
    }
    char path[160];
    if (g_res.src == S_B64) {
        ot_copy(path, sizeof(path), B64_RELEASE);
        ot_cat(path, sizeof(path), g_res.r[ri].id);
    } else {
        ot_copy(path, sizeof(path), URL_ENTRIES "/");
        url_cat(path, sizeof(path), g_res.r[ri].id);
        ot_cat(path, sizeof(path), "/");
        ot_cat_int(path, sizeof(path), g_res.r[ri].cat);
    }
    char say[96];
    ot_copy(say, sizeof(say), "Loading the files from ");
    ot_cat(say, sizeof(say), k_src[g_res.src]);
    ot_cat(say, sizeof(say), " ...");
    return prefetch(W_ENTRIES, ri, -1, path, ONLINE_JSON_MAX, say);
}

int tab_list(const char *rel, FsEntry *out, int max)
{
    collect();
    while (*rel == '/') {
        rel++;
    }
    int n = 0;
    if (!*rel) {
        for (int i = 0; i < g_res.n && n < max; i++) {
            if (!g_res.r[i].loadable) {
                continue;
            }
            memset(&out[n], 0, sizeof(out[n]));
            ot_copy(out[n].name, sizeof(out[n].name), g_res.r[i].folder);
            out[n].dir = true;
            n++;
        }
        return n;
    }
    const char *file;
    int ri = split_rel(rel, &file);
    if (ri < 0 || !have_entries(ri)) {
        return -1;
    }
    for (int i = 0; i < nent && n < max; i++) {
        memset(&out[n], 0, sizeof(out[n]));
        ot_copy(out[n].name, sizeof(out[n].name), ent[i].name);
        out[n].size = ent[i].size;
        n++;
    }
    return n;
}

bool tab_stat(const char *rel, uint64_t *size, bool *dir)
{
    collect();
    while (*rel == '/') {
        rel++;
    }
    *size = 0;
    *dir = true;
    if (!*rel) {
        return true;
    }
    const char *file;
    int ri = split_rel(rel, &file);
    if (ri < 0) {
        return false;
    }
    if (!file) {
        return true;
    }
    if (!have_entries(ri)) {
        return false;
    }
    for (int i = 0; i < nent; i++) {
        if (ot_ieq(ent[i].name, file)) {
            *dir = false;
            *size = fs_size_known(ent[i].size) ? ent[i].size : 0;   // the "-" is the listing's (tab_list); the base's callers of stat read a length and test for no FS_SIZE_UNKNOWN
            return true;
        }
    }
    return false;
}

// The rows of the list online.open asks for (ctx.ask_list, api.md 4.14.14; read again about every half second while it is up): the files of the result g_pick, value and
// title its name, the right column its size ("-" unknown); while they load, or when they failed, one note row that says so.
const char *st_files(CoreCtx *)
{
    static char b[MAX_ENT * 96 + 128];
    collect();
    b[0] = 0;
    if (g_pick < 0 || g_pick >= g_res.n || g_pick_gen != g_res.gen) {
        return "!The results have changed: search again\n";
    }
    if (g_pick_err[0] || !have_entries(g_pick)) {
        const bool running = g_w.state != 0 && g_w.kind == W_ENTRIES && g_w.ri == g_pick && g_w.gen == g_res.gen;
        if (!g_pick_err[0] && !running) {
            ot_copy(g_pick_err, sizeof(g_pick_err), err);    // failed (prefetch put the reason in err): said, not asked for again every half second
        }
        ot_copy(b, sizeof(b), "!");
        ot_cat(b, sizeof(b), g_pick_err[0] ? g_pick_err : err);
        ot_cat(b, sizeof(b), "\n");
        return b;
    }
    for (int i = 0; i < nent; i++) {
        char kb[24] = "-";                                     // as fsif.h fs_size_kb writes it (its snprintf is not in this sub-app's libc)
        if (fs_size_known(ent[i].size)) {
            kb[0] = 0;
            ot_cat_int(kb, sizeof(kb), (long)((ent[i].size + 1023) >> 10));
            ot_cat(kb, sizeof(kb), " KB");
        }
        ot_cat(b, sizeof(b), ent[i].name);
        ot_cat(b, sizeof(b), "\t");
        ot_cat(b, sizeof(b), ent[i].name);
        ot_cat(b, sizeof(b), "\t");
        ot_cat(b, sizeof(b), kb);
        ot_cat(b, sizeof(b), "\n");
    }
    return b[0] ? b : "!No file of it loads here\n";
}

// The bytes of a file of a result in a block of the caller's allocator (the base frees it with its own free: a block of this sub-app's account is no block of the base's). A file that is not
// kept yet is downloaded in the background: null, and tab_error says so; the call after the download has ended gets the bytes.
uint8_t *tab_fetch(const char *rel, uint8_t *(*alloc)(uint32_t n), uint32_t *len)
{
    collect();
    const char *file;
    int ri = split_rel(rel, &file);
    if (!file || !alloc || ri < 0 || !have_entries(ri)) {
        return 0;
    }
    int ei = -1;
    for (int i = 0; i < nent; i++) {
        if (ot_ieq(ent[i].name, file)) {
            ei = i;
        }
    }
    if (ei < 0) {
        ot_copy(err, sizeof(err), "No such file in this result");
        return 0;
    }
    if (!(dl_b && dl_res == ri && dl_idx == ent[ei].idx && dl_gen == g_res.gen)) {
        drop_download();                                    // the job never touches dl_b: the old file goes before the new one grows, so one download is the most at once
        char path[160];
        if (g_res.src == S_B64) {
            ot_copy(path, sizeof(path), B64_DOWNLOAD);
            ot_cat(path, sizeof(path), g_res.r[ri].id);
        } else {
            ot_copy(path, sizeof(path), URL_DOWNLOAD "/");
            url_cat(path, sizeof(path), g_res.r[ri].id);
            ot_cat(path, sizeof(path), "/");
            ot_cat_int(path, sizeof(path), g_res.r[ri].cat);
        }
        ot_cat(path, sizeof(path), "/");
        ot_cat_int(path, sizeof(path), ent[ei].idx);
        char say[96];
        ot_copy(say, sizeof(say), "Downloading ");
        ot_cat(say, sizeof(say), ent[ei].name);
        ot_cat(say, sizeof(say), " ...");
        prefetch(W_DOWNLOAD, ri, ent[ei].idx, path, ONLINE_DOWNLOAD_MAX, say);
        return 0;
    }
    uint8_t *c = alloc(dl_len ? dl_len : 1);
    if (c) {
        memcpy(c, dl_b, dl_len);
        *len = dl_len;
    }
    return c;
}

}   // namespace

#define ACH_STATES(I, T) \
    { "online.v.a." T, st_av<I> }, \
    { "online.l.a." T, st_al<I> }, \
    { "online.h.a." T, st_ah<I> },
#define BF_STATES(F, K) \
    { "online.v.b." K, st_bv<F> }, \
    { "online.l.b." K, st_bl<F> },

static const ArgSpec set_args[] = { ARG_STRING("field", 16), ARG_STRING("text", 120) };
static const ArgSpec row_args[] = { ARG_INT("index", 0, 9999), ARG_STRING("answer", 65) };
static const ArgSpec file_args[] = { ARG_INT("phase", 0, 9999), ARG_STRING("answer", 64) };

const LibOpDecl online_ops[] = {
    { "online.set", set_args, 2, op_set },
    { "online.next_source", 0, 0, op_next_source },
    { "online.clear", 0, 0, op_clear },
    { "online.search", 0, 0, op_search },
    { "online.open", row_args, 2, op_open },
    { "online.file", file_args, 2, op_file },
    { "online.more", row_args, 2, op_more },
    { "online.new", 0, 0, op_new },
    { "online.forget", 0, 0, op_forget },
};
const int online_nops = (int)(sizeof(online_ops) / sizeof(online_ops[0]));

const LibStateDecl online_states[] = {
    { "online.src", st_src },
    { "online.ftitle", st_ftitle },
    { "online.l.source", st_lsource },
    { "online.lbl.name", st_lblname },
    { "online.v.name", st_vname },
    { "online.v.group", st_vgroup },
    { "online.v.handle", st_vhandle },
    { "online.v.event", st_vevent },
    { "online.p.name", st_pname },
    { "online.p.group", st_pgroup },
    { "online.p.handle", st_phandle },
    { "online.p.event", st_pevent },
    { "online.h.ac", st_hac },
    { "online.h.b", st_hb },
    { "online.h.b.catm", st_hbcatm },
    { "online.h.b.hwm", st_hbhwm },
    ACH_STATES(0, "category") ACH_STATES(1, "subcat") ACH_STATES(2, "repo") ACH_STATES(3, "date") ACH_STATES(4, "latest")
    ACH_STATES(5, "rating") ACH_STATES(6, "type") ACH_STATES(7, "sort") ACH_STATES(8, "order")
    BF_STATES(0, "look") BF_STATES(1, "cat") BF_STATES(2, "catm") BF_STATES(3, "plat") BF_STATES(4, "env")
    BF_STATES(5, "hw") BF_STATES(6, "hwm") BF_STATES(7, "ai") BF_STATES(8, "show")
    { "online.busy", st_busy },
    { "online.net", st_net },
    { "online.s.last", st_last },
    { "online.s.result", st_outcome },
    { "online.s.never", st_never },
    { "online.shown", st_shown },
    { "online.title", st_title },
    { "online.rows", st_rows },
    { "online.r.source", st_rsource },
    { "online.r.query", st_rquery },
    { "online.r.found", st_rfound },
    { "online.r.hidden", st_rhidden },
    { "online.r.nohidden", st_rnohidden },
    { "online.at", st_at },
    { "online.files", st_files },
};
const int online_nstates = (int)(sizeof(online_states) / sizeof(online_states[0]));

const OnlineApi online_table = { sizeof(OnlineApi), tab_has_results, tab_list, tab_stat, tab_fetch, tab_error, tab_loading, tab_loading_for };

void online_attach(const LibApi *api)
{
    g_api = api;
    if (!g_mx && api->sync && api->sync->mutex_new) {
        g_mx = api->sync->mutex_new();
    }
}
