// The C library calls this library needs beyond the SDK's sdk/libc.c (libc_extra.c), and the character classes it uses (inline here: newlib's ctype.h
// reads a locale table the image does not have). The declarations are the C library's, so the code calls them by their usual names.
#ifndef TRX64U_LIBC_EXTRA_H
#define TRX64U_LIBC_EXTRA_H

#include <stdarg.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// %s %c %d %i %u %x %X with the flags '-' and '0', a width and (for %s) a precision; an 'l' is read over (int and long are one size here). The result
// is cut at n - 1 bytes and always terminated (n > 0); the return value is the length the whole text would have.
int vsnprintf(char *out, size_t n, const char *fmt, va_list ap);
int snprintf(char *out, size_t n, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
int strcasecmp(const char *a, const char *b);
int strncasecmp(const char *a, const char *b, size_t n);
size_t strspn(const char *s, const char *accept);
int atoi(const char *s);

#ifdef __cplusplus
}
#endif

static inline int lc_isdigit(int c) { return c >= '0' && c <= '9'; }
static inline int lc_isalpha(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
static inline int lc_isxdigit(int c) { return lc_isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
static inline int lc_toupper(int c) { return c >= 'a' && c <= 'z' ? c - 32 : c; }
static inline int lc_tolower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

#endif
