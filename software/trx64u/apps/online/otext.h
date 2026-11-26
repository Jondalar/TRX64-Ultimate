// The few text helpers the online sub-app uses. An application image has no C library: sdk/libc.c gives memcpy, memmove, memset, memcmp, strlen, strcmp, strncmp, strcpy, strncpy, strcat, strchr, strrchr
// and the compiler's helpers, nothing else: no snprintf, no strcasecmp, no strstr. These are the same calls over plain loops.
#ifndef APPS_ONLINE_OTEXT_H
#define APPS_ONLINE_OTEXT_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

static inline int ot_len(const char *s)
{
    int n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

// dst = src, cut to n - 1 characters
static inline void ot_copy(char *dst, int n, const char *src)
{
    int i = 0;
    for (; i < n - 1 && src[i]; i++) {
        dst[i] = src[i];
    }
    if (n > 0) {
        dst[i] = 0;
    }
}

// dst += src, cut so that dst keeps n - 1 characters
static inline void ot_cat(char *dst, int n, const char *src)
{
    int l = ot_len(dst);
    if (l < n) {
        ot_copy(dst + l, n - l, src);
    }
}

static inline void ot_cat_int(char *dst, int n, long v)
{
    char t[16];
    int k = 0;
    bool neg = v < 0;
    unsigned long u = neg ? (unsigned long)(-v) : (unsigned long)v;
    do {
        t[k++] = (char)('0' + (int)(u % 10));
        u /= 10;
    } while (u && k < 14);
    char o[18];
    int j = 0;
    if (neg) {
        o[j++] = '-';
    }
    while (k) {
        o[j++] = t[--k];
    }
    o[j] = 0;
    ot_cat(dst, n, o);
}

static inline char ot_lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }

// equal, ignoring the case of ASCII letters
static inline bool ot_ieq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        if (ot_lower(*a) != ot_lower(*b)) {
            return false;
        }
    }
    return *a == *b;
}

// the first place `what` stands in the n bytes at `s`, ignoring case; null when it does not
static inline const char *ot_ifind(const char *s, int n, const char *what)
{
    int w = ot_len(what);
    for (int i = 0; i + w <= n; i++) {
        int k = 0;
        while (k < w && ot_lower(s[i + k]) == ot_lower(what[k])) {
            k++;
        }
        if (k == w) {
            return s + i;
        }
    }
    return 0;
}

// does `s` end with `tail`, ignoring case?
static inline bool ot_iends(const char *s, const char *tail)
{
    int a = ot_len(s), b = ot_len(tail);
    return a >= b && ot_ieq(s + a - b, tail);
}

// The ASCII letter a Latin-1 letter (U+00C0 .. U+00FF) is read as; 0 for a sign that has none. The menu's fonts (LVGL Montserrat, built for U+0020..U+007F, U+00B0 and
// U+2022) have no other letter.
static inline char ot_latin_base(unsigned cp)
{
    static const char t[] = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYPsaaaaaaaceeeeiiiidnooooo/ouuuuypy";
    return cp >= 0xC0 && cp <= 0xFF ? t[cp - 0xC0] : 0;
}

// dst = src as one line the menu's fonts can draw, cut so that dst keeps n - 1 bytes. The servers answer UTF-8 (online_json.cc decodes a \u escape to UTF-8 as
// well): ASCII stays, the degree sign and the bullet (the separator of the sub-app's own lines, DOT in online_search.cc) stay UTF-8, a Latin-1 letter becomes its base
// letter, "ss" stands for the sharp s, any other sign is a '?', and a control character (a line end; a TAB, which separates the fields of a list row) is a space. A byte
// that does not start valid UTF-8 is read as Latin-1. A sign is written whole or not at all.
static inline void ot_fold(char *dst, int n, const char *src)
{
    int j = 0;
    const unsigned char *s = (const unsigned char *)src;
    while (*s && j < n - 1) {
        unsigned cp = *s++;
        if (cp >= 0xC0 && cp < 0xF8 && (s[0] & 0xC0) == 0x80) {
            int more = cp >= 0xF0 ? 3 : cp >= 0xE0 ? 2 : 1;
            unsigned v = cp & (0x3Fu >> more);
            int k = 0;
            while (k < more && (s[k] & 0xC0) == 0x80) {
                v = (v << 6) | (s[k] & 0x3Fu);
                k++;
            }
            if (k == more) {
                s += more;
                cp = v;
            }
        }
        if (cp == 0xDF) {
            dst[j++] = 's';
            if (j < n - 1) {
                dst[j++] = 's';
            }
            continue;
        }
        if (cp == 0xB0 || cp == 0x2022) {
            const char *u = cp == 0xB0 ? "\xC2\xB0" : "\xE2\x80\xA2";
            int ul = ot_len(u);
            if (j + ul > n - 1) {
                break;
            }
            for (int k = 0; k < ul; k++) {
                dst[j++] = u[k];
            }
            continue;
        }
        char c = cp < 0x20 || cp == 0x7F ? ' ' : cp < 0x80 ? (char)cp : ot_latin_base(cp);
        dst[j++] = c ? c : '?';
    }
    if (n > 0) {
        dst[j] = 0;
    }
}

// dst = src folded (ot_fold) to at most `most` bytes (and n - 1): a longer text is cut before a whole sign and ends in "..."
static inline void ot_fold_cut(char *dst, int n, const char *src, int most)
{
    char t[256];
    ot_fold(t, sizeof(t), src);
    if (most > n - 1) {
        most = n - 1;
    }
    int len = ot_len(t);
    if (len > most) {
        int cut = most > 3 ? most - 3 : 0;
        while (cut > 0 && ((unsigned char)t[cut] & 0xC0) == 0x80) {
            cut--;                                            // not inside a sign of more than one byte
        }
        t[cut] = 0;
        ot_copy(dst, n, t);
        if (most > 3) {
            ot_cat(dst, n, "...");
        }
        return;
    }
    ot_copy(dst, n, t);
}

#endif
