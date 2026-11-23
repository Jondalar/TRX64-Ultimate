// The settings as the base holds them (trx64u_lib.h), through its settings group (svc_settings.h). A read is settings.value of the setting's index: the
// position of the value in force, an override included. The index of each key of this library's menu is looked up once with settings.find and kept while
// settings.generation is unchanged (a new Settings object or a rebind to another config counts it up, and every kept index is looked up again). setlists.h
// has the choice settings of the same menu.toml: which keys the menu has, their number of values, a range's low end and base, and the labels.
// A key the menu does not have (a setting of the generic application, "Joystick Input") is read with settings.cfg_value.
#include "trx64u_lib.h"
#include <string.h>

struct SetDef {
    const char *key;
    const char *labels;                         // each label ends in NUL; null: a range
    int16_t n;
    int32_t lo, base;
};
#include "setlists.h"

#define N_DEFS ((int)(sizeof(set_defs) / sizeof(set_defs[0])))

static const SetDef *find_def(const char *key)
{
    int a = 0, b = N_DEFS - 1;
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

// The base's index of a key of the menu: 0 not looked up yet, -1 the base has none, else index + 1; all of them belong to generation g_gen.
static int16_t g_ix[N_DEFS];
static uint32_t g_gen;

static int base_ix(const SvcSettings *s, const SetDef *d)
{
    uint32_t gen = s->generation();
    if (gen != g_gen) {
        memset(g_ix, 0, sizeof(g_ix));
        g_gen = gen;
    }
    int16_t &e = g_ix[d - set_defs];
    if (!e) {
        int ix = s->find(d->key);
        e = (int16_t)(ix < 0 ? -1 : ix + 1);
    }
    return e < 0 ? -1 : e - 1;
}

// the base's index of any key: a key of the menu from the kept ones, another one looked up
static int key_ix(const SvcSettings *s, const char *key)
{
    const SetDef *d = find_def(key);
    return d ? base_ix(s, d) : s->find(key);
}

int setting_ix(const char *key, int def)
{
    const SvcSettings *s = g_api->settings;
    if (!s) {
        return def;
    }
    const SetDef *d = find_def(key);
    if (!d) {
        return s->cfg_value(key, def);
    }
    int ix = base_ix(s, d);
    int v = ix < 0 ? -1 : s->value(ix);
    return v < 0 || v >= d->n ? def : v;
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

const char *setting_label(const char *key, int ix)
{
    const SetDef *d = find_def(key);
    if (!d || !d->labels || ix < 0 || ix >= d->n) {
        return 0;
    }
    const char *l = d->labels;
    for (int i = 0; i < ix; i++) {
        l += strlen(l) + 1;
    }
    return l;
}

const char *setting_text(const char *key)
{
    const SvcSettings *s = g_api->settings;
    const char *t = s ? s->cfg_text(key) : 0;
    return t ? t : "";
}

bool settings_ready(void)
{
    const SvcSettings *s = g_api->settings;
    return s && base_ix(s, &set_defs[0]) >= 0;
}

bool setting_set(const char *key, int ix)
{
    const SvcSettings *s = g_api->settings;
    int k = s ? key_ix(s, key) : -1;
    return k >= 0 && s->set(k, ix);
}

void setting_override(const char *key, int ix)
{
    const SvcSettings *s = g_api->settings;
    int k = s ? key_ix(s, key) : -1;
    if (k >= 0) {
        s->override_value(k, ix);
    }
}
