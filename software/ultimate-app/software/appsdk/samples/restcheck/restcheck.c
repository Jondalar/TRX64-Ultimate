/*
 * restcheck.c — answers PUT /v1/apps/restcheck:run, for tests.
 *
 *   echo    replies "abi=<major>.<minor> ui=<has_ui> source=<launch_source>
 *           action=<action> file=<selected_file> args=<args>"
 *   config  args is "Category/Item"; replies the item's current value
 *   fail    returns exit code 7 and replies "failed on purpose"
 */

#include "u2p_app.h"

static char out[256];

/* Apps are freestanding: no C library. */
static int eq(const char *a, const char *b)
{
    while (*a && (*a == *b)) {
        a++, b++;
    }
    return *a == *b;
}

static void put(char **p, const char *s)
{
    while (*s && (*p < out + sizeof(out) - 1)) {
        *(*p)++ = *s++;
    }
    **p = 0;
}

static void put_int(char **p, int v)
{
    char buf[12];
    int n = 0;
    if (v < 0) {
        put(p, "-");
        v = -v;
    }
    do {
        buf[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v && (n < 11));
    while (n) {
        char c[2] = { buf[--n], 0 };
        put(p, c);
    }
}

int app_main(const u2p_api_t *api, const u2p_app_context_t *ctx)
{
    const char *action = ctx->action ? ctx->action : "";
    char *p = out;

    if (eq(action, "config")) {
        char category[64];
        const char *args = ctx->args ? ctx->args : "";
        int n = 0;
        while (args[n] && (args[n] != '/') && (n < (int)sizeof(category) - 1)) {
            category[n] = args[n];
            n++;
        }
        category[n] = 0;
        if (args[n] != '/') {
            ctx->reply("args must be Category/Item");
            return 2;
        }
        const char *slash = args + n;
        char value[64];
        int r = api->config->get(category, slash + 1, value, sizeof(value));
        if (r < 0) {
            put(&p, "error ");
            put_int(&p, r);
            ctx->reply(out);
            return 3;
        }
        ctx->reply(value);
        return 0;
    }
    if (eq(action, "fail")) {
        ctx->reply("failed on purpose");
        return 7;
    }

    put(&p, "abi=");
    put_int(&p, api->abi_major);
    put(&p, ".");
    put_int(&p, api->abi_minor);
    put(&p, " ui=");
    put_int(&p, ctx->has_ui);
    put(&p, " source=");
    put_int(&p, (int)ctx->launch_source);
    put(&p, " action=");
    put(&p, action);
    put(&p, " file=");
    put(&p, ctx->selected_file ? ctx->selected_file : "-");
    put(&p, " args=");
    put(&p, ctx->args ? ctx->args : "-");
    api->sys->log("[restcheck] %s\n", out);
    ctx->reply(out);
    return 0;
}
