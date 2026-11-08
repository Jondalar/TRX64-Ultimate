/*
 * cart_fmt.c
 *
 *  The snprintf the tools format their lines with (T95 D5). Freestanding: the app has no libc, and the
 *  firmware's printf is not in the ABI. Supports %d %u %x %X %s %c %%, a '0' flag, a width and a precision
 *  for %s - what the ported tools use, nothing more.
 */

#include "cart_fmt.h"

typedef struct
{
    char *buf;
    int size;
    int pos;
} fmt_out_t;

static void fmt_char(fmt_out_t *o, char c)
{
    if (o->pos < o->size - 1) {
        o->buf[o->pos] = c;
    }
    o->pos++;
}

int cart_vsnprintf(char *buf, int size, const char *fmt, va_list ap)
{
    fmt_out_t o = { buf, size, 0 };
    char digits[12];

    while (*fmt) {
        if (*fmt != '%') {
            fmt_char(&o, *fmt++);
            continue;
        }
        fmt++;

        char pad = ' ';
        int width = 0;
        int precision = -1;
        if (*fmt == '0') {
            pad = '0';
            fmt++;
        }
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt++ - '0');
        }
        if (*fmt == '.') {
            fmt++;
            precision = 0;
            while (*fmt >= '0' && *fmt <= '9') {
                precision = precision * 10 + (*fmt++ - '0');
            }
        }

        const char *str = digits;
        int len = 0;
        bool negative = false;

        switch (*fmt) {
        case 'd':
        case 'u':
        case 'x':
        case 'X': {
            unsigned int base = (*fmt == 'x' || *fmt == 'X') ? 16 : 10;
            const char *hex = (*fmt == 'x') ? "0123456789abcdef" : "0123456789ABCDEF";
            unsigned int value;
            if (*fmt == 'd') {
                int v = va_arg(ap, int);
                negative = (v < 0);
                value = negative ? (unsigned int)(-(v + 1)) + 1 : (unsigned int)v;
            } else {
                value = va_arg(ap, unsigned int);
            }
            char rev[12];
            int n = 0;
            do {
                rev[n++] = hex[value % base];
                value /= base;
            } while (value);
            if (negative) {
                digits[len++] = '-';
            }
            while (n) {
                digits[len++] = rev[--n];
            }
            break;
        }
        case 's':
            str = va_arg(ap, const char *);
            if (!str) {
                str = "(null)";
            }
            while (str[len] && (precision < 0 || len < precision)) {
                len++;
            }
            break;
        case 'c':
            digits[0] = (char)va_arg(ap, int);
            len = 1;
            break;
        case 0:
            continue; // format ends after '%'
        default:
            fmt_char(&o, '%');
            digits[0] = *fmt;
            len = 1;
            break;
        }
        fmt++;

        // A zero-padded negative number keeps its sign in front of the zeros
        if (negative && pad == '0') {
            fmt_char(&o, '-');
            str++;
            len--;
            width--;
        }
        for (int i = len; i < width; i++) {
            fmt_char(&o, pad);
        }
        for (int i = 0; i < len; i++) {
            fmt_char(&o, str[i]);
        }
    }

    if (size > 0) {
        o.buf[(o.pos < size) ? o.pos : size - 1] = 0;
    }
    return o.pos;
}

int cart_snprintf(char *buf, int size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int ret = cart_vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return ret;
}

