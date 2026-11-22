// PROVENANCE: TRX64-Ultimate software/ultimate-app/software/io/c64/trx_json.h, copied for the ring library (U18 D6: this copy is the master).
// The jsmn walk the .c64re reader and the node builder share (T66 part 4):
// token compare, a key's value token, an integer, and a TRX64 typed-array node
// {"$ta":"Uint8Array","b64":"..."}. Header-only so the host test bank
// (software/io/c64/host) compiles it without the firmware around it.
#ifndef TRX_JSON_H
#define TRX_JSON_H

#include <stdint.h>
#include <string.h>
#include "ringsys.h"         // r_strtoll: the SDK has no strtoll

#define JSMN_STATIC
#include "jsmn.h"

static inline bool tok_eq(const char *js, const jsmntok_t &t, const char *s)
{
    int n = t.end - t.start;
    return t.type == JSMN_STRING && int(strlen(s)) == n && !strncmp(js + t.start, s, n);
}

// the value token of key k in object o, or -1
static inline int json_key(const char *js, const jsmntok_t *t, int n, int o, const char *k)
{
    if (o < 0 || t[o].type != JSMN_OBJECT) {
        return -1;
    }
    for (int i = o + 1; i < n && t[i].start < t[o].end; i++) {
        if (t[i].parent == o && tok_eq(js, t[i], k)) {
            return i + 1;
        }
    }
    return -1;
}

static inline int64_t tok_int(const char *js, const jsmntok_t *t, int i)
{
    if (i < 0) {
        return -1;
    }
    return r_strtoll(js + t[i].start);
}

// The same with a value for "the key is not there" (nodes whose 0 means something).
static inline int64_t tok_int_or(const char *js, const jsmntok_t *t, int i, int64_t dflt)
{
    return i < 0 ? dflt : r_strtoll(js + t[i].start);
}

// true when token i is the literal true
static inline bool tok_true(const char *js, const jsmntok_t *t, int i)
{
    return i >= 0 && t[i].type == JSMN_PRIMITIVE && js[t[i].start] == 't';
}

static inline int b64val(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

// a {"$ta":..,"b64":..} node into out (exactly len bytes), false otherwise
static inline bool tok_ta(const char *js, const jsmntok_t *t, int n, int i, uint8_t *out, int len)
{
    int b = json_key(js, t, n, i, "b64");
    if (b < 0) {
        return false;
    }
    const char *s = js + t[b].start;
    int sl = t[b].end - t[b].start;
    int o = 0;
    uint32_t acc = 0;
    int nb = 0;
    for (int k = 0; k < sl; k++) {
        int v = b64val(s[k]);
        if (v < 0) {
            continue;
        }
        acc = (acc << 6) | v;
        nb += 6;
        if (nb >= 8) {
            nb -= 8;
            if (o < len) {
                out[o] = uint8_t(acc >> nb);
            }
            o++;
        }
    }
    return o == len;
}

// The same, but a shorter node is taken as far as it goes (TRX64 writes plain
// JSON arrays for some nodes and a typed array for others): how many bytes came
// out, 0 when the node is not a typed array at all.
static inline int tok_ta_upto(const char *js, const jsmntok_t *t, int n, int i, uint8_t *out, int len)
{
    int b = json_key(js, t, n, i, "b64");
    if (b < 0) {
        return 0;
    }
    const char *s = js + t[b].start;
    int sl = t[b].end - t[b].start;
    int o = 0;
    uint32_t acc = 0;
    int nb = 0;
    for (int k = 0; k < sl && o < len; k++) {
        int v = b64val(s[k]);
        if (v < 0) {
            continue;
        }
        acc = (acc << 6) | v;
        nb += 6;
        if (nb >= 8) {
            nb -= 8;
            out[o++] = uint8_t(acc >> nb);
        }
    }
    return o;
}

// The byte length of a {"$ta":..,"b64":..} node, without decoding it: 0 when
// the node is not one.
static inline uint32_t tok_ta_len(const char *js, const jsmntok_t *t, int n, int i)
{
    int b = json_key(js, t, n, i, "b64");
    if (b < 0) {
        return 0;
    }
    const char *s = js + t[b].start;
    int sl = t[b].end - t[b].start;
    uint32_t chars = 0, pad = 0;
    for (int k = 0; k < sl; k++) {
        if (b64val(s[k]) >= 0) {
            chars++;
        } else if (s[k] == '=') {
            pad++;
        }
    }
    uint32_t whole = (chars / 4) * 3;
    switch (chars % 4) {            // a tail without padding characters
    case 2: return whole + 1;
    case 3: return whole + 2;
    default: break;
    }
    return whole - (pad > 2 ? 2 : pad);
}

// A plain JSON array of numbers into out (up to len bytes): how many it had.
static inline int tok_bytes(const char *js, const jsmntok_t *t, int n, int i, uint8_t *out, int len)
{
    if (i < 0 || t[i].type != JSMN_ARRAY) {
        return 0;
    }
    int o = 0;
    for (int k = i + 1; k < n && t[k].start < t[i].end && o < len; k++) {
        if (t[k].parent == i) {
            out[o++] = uint8_t(r_strtoll(js + t[k].start));
        }
    }
    return o;
}

// Either shape: a typed array or a plain array of numbers.
static inline int tok_any_bytes(const char *js, const jsmntok_t *t, int n, int i, uint8_t *out, int len)
{
    int got = tok_ta_upto(js, t, n, i, out, len);
    return got ? got : tok_bytes(js, t, n, i, out, len);
}

#endif
