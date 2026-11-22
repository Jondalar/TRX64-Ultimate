// The settings as the base shows them (trx64u_lib.h): a read is api->ops->state("set.<key>"), which answers the label of the value in force (a text
// setting: its text) and null when the base has no such setting or no settings yet (the core application's ops_state, coreapp/src/ops_glue.cc
// setting_state). The code works with the index of the value, as it did with Settings::value(): the label is looked up in the setting's values, which
// setlists.h has from the same menu.toml (a range: the number in the label minus its low end).
#include "trx64u_lib.h"
#include <string.h>

struct SetDef {
    const char *key;
    const char *labels;                         // each label ends in NUL; null: a range
    int16_t n;
    int32_t lo, base;
};
#include "setlists.h"

static const SetDef *find_def(const char *key)
{
    int a = 0, b = (int)(sizeof(set_defs) / sizeof(set_defs[0])) - 1;
    while (a <= b) {
        int m = (a + b) / 2;
        int c = strcmp(key, set_defs[m].key);
        if (!c) {
            return &set_defs[m];
        }
        if (c < 0) {
            b = m - 1;
        } else {
            a = m + 1;
        }
    }
    return 0;
}

static const char *state_of(const char *key)
{
    char name[72];
    if (!g_api->ops || strlen(key) + 5 > sizeof(name)) {
        return 0;
    }
    strcpy(name, "set.");
    strcpy(name + 4, key);
    return g_api->ops->state(name, 0);
}

// a range's label: the number, then the unit
static bool label_number(const char *s, int32_t *v)
{
    while (*s == ' ') {
        s++;
    }
    bool neg = *s == '-';
    if (neg) {
        s++;
    }
    if (!lc_isdigit((unsigned char)*s)) {
        return false;
    }
    int32_t x = 0;
    while (lc_isdigit((unsigned char)*s)) {
        x = x * 10 + (*s++ - '0');
    }
    *v = neg ? -x : x;
    return true;
}

int setting_ix(const char *key, int def)
{
    const SetDef *d = find_def(key);
    const char *v = d ? state_of(key) : 0;
    if (!v) {
        return def;
    }
    if (!d->labels) {
        int32_t x;
        if (!label_number(v, &x) || x < d->lo || x >= d->lo + d->n) {
            return def;
        }
        return (int)(x - d->lo);
    }
    const char *l = d->labels;
    for (int i = 0; i < d->n; i++) {
        if (!strcmp(l, v)) {
            return i;
        }
        l += strlen(l) + 1;
    }
    return def;
}

int setting_num(const char *key, int def)
{
    const SetDef *d = find_def(key);
    if (!d) {
        return def;
    }
    int ix = setting_ix(key, -1);
    return ix < 0 ? def : ix + (int)d->base;
}

int setting_count(const char *key)
{
    const SetDef *d = find_def(key);
    return d ? d->n : 0;
}

bool setting_known(const char *key)
{
    return find_def(key) != 0;
}

const char *setting_text(const char *key)
{
    const char *t = state_of(key);
    if (!t) {
        return "";
    }
    while (*t == ' ') {
        t++;
    }
    return t;
}

bool settings_ready(void)
{
    return state_of(set_defs[0].key) != 0;
}
