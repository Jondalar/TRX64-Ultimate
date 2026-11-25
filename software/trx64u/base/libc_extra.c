/* The C library calls of libc_extra.h. No floating point, no 64-bit numbers: the library prints neither. */
#include "libc_extra.h"
#include <stdint.h>

struct Out {
    char *p;
    size_t n, at;
};

static void put(struct Out *o, char c)
{
    if (o->n && o->at < o->n - 1) {
        o->p[o->at] = c;
    }
    o->at++;
}

static void pad(struct Out *o, int count, char c)
{
    while (count-- > 0) {
        put(o, c);
    }
}

int vsnprintf(char *out, size_t n, const char *fmt, va_list ap)
{
    struct Out o = { out, n, 0 };
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            put(&o, *fmt);
            continue;
        }
        fmt++;
        int left = 0, zero = 0, width = 0, prec = -1;
        for (;; fmt++) {
            if (*fmt == '-') {
                left = 1;
            } else if (*fmt == '0') {
                zero = 1;
            } else {
                break;
            }
        }
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt++ - '0');
        }
        if (*fmt == '.') {
            prec = 0;
            fmt++;
            while (*fmt >= '0' && *fmt <= '9') {
                prec = prec * 10 + (*fmt++ - '0');
            }
        }
        while (*fmt == 'l') {
            fmt++;
        }
        char num[12];
        const char *s = num;
        int len = 0;
        switch (*fmt) {
        case 's':
            s = va_arg(ap, const char *);
            if (!s) {
                s = "(null)";
            }
            while (s[len] && (prec < 0 || len < prec)) {
                len++;
            }
            break;
        case 'c':
            num[0] = (char)va_arg(ap, int);
            len = 1;
            break;
        case 'd':
        case 'i':
        case 'u':
        case 'x':
        case 'X': {
            unsigned int v;
            int neg = 0;
            if (*fmt == 'd' || *fmt == 'i') {
                int d = va_arg(ap, int);
                neg = d < 0;
                v = neg ? 0u - (unsigned int)d : (unsigned int)d;
            } else {
                v = va_arg(ap, unsigned int);
            }
            unsigned int base = (*fmt == 'x' || *fmt == 'X') ? 16u : 10u;
            const char *digits = *fmt == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
            char tmp[12];
            int k = 0;
            do {
                tmp[k++] = digits[v % base];
                v /= base;
            } while (v);
            if (neg) {
                if (zero && !left) {                    /* the sign before the zeros */
                    put(&o, '-');
                    width--;
                } else {
                    tmp[k++] = '-';
                }
            }
            while (k) {
                num[len++] = tmp[--k];
            }
            break;
        }
        case '%':
            num[0] = '%';
            len = 1;
            break;
        case 0:
            fmt--;                                      /* a lone '%' at the end: the loop ends on the terminator */
            continue;
        default:
            num[0] = '%';
            num[1] = *fmt;
            len = 2;
            break;
        }
        if (!left) {
            pad(&o, width - len, (zero && *fmt != 's' && *fmt != 'c') ? '0' : ' ');
        }
        for (int i = 0; i < len; i++) {
            put(&o, s[i]);
        }
        if (left) {
            pad(&o, width - len, ' ');
        }
    }
    if (n) {
        out[o.at < n ? o.at : n - 1] = 0;
    }
    return (int)o.at;
}

int snprintf(char *out, size_t n, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(out, n, fmt, ap);
    va_end(ap);
    return r;
}

int strcasecmp(const char *a, const char *b)
{
    for (;; a++, b++) {
        int x = lc_tolower((unsigned char)*a), y = lc_tolower((unsigned char)*b);
        if (x != y || !x) {
            return x - y;
        }
    }
}

int strncasecmp(const char *a, const char *b, size_t n)
{
    for (; n; n--, a++, b++) {
        int x = lc_tolower((unsigned char)*a), y = lc_tolower((unsigned char)*b);
        if (x != y || !x) {
            return x - y;
        }
    }
    return 0;
}

size_t strspn(const char *s, const char *accept)
{
    size_t k = 0;
    for (; s[k]; k++) {
        const char *a = accept;
        while (*a && *a != s[k]) {
            a++;
        }
        if (!*a) {
            break;
        }
    }
    return k;
}

int atoi(const char *s)
{
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    int neg = *s == '-';
    if (*s == '-' || *s == '+') {
        s++;
    }
    int v = 0;
    while (lc_isdigit((unsigned char)*s)) {
        v = v * 10 + (*s++ - '0');
    }
    return neg ? -v : v;
}
