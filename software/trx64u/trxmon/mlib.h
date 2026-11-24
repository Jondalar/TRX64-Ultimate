/* Moved from TREX/1541ultimate 0103c585, apps/trxmon/src/mlib.h (TRX64-Ultimate U20 A2). */
/*
 * mlib.h — the few string and number helpers trxmon needs. It is built
 * freestanding (no libc); the fork builds the same code on the host for its
 * unit tests, so nothing here is named like a libc function.
 */
#ifndef TRXMON_MLIB_H
#define TRXMON_MLIB_H

#include <stddef.h>
#include <stdint.h>
#include <string.h> /* memcpy, memset, memmove, memcmp: in trx64u the SDK's sdk/libc.c (../monitor/Makefile LIB_LIBC) */

int m_strlen(const char *s);
int m_streq(const char *a, const char *b);
int m_strieq(const char *a, const char *b);
int m_strnieq(const char *a, const char *b, int n); /* first n chars, ignoring case */
char m_lower(char c);

/* TRX64's parse_hex: an optional '$', then hex digits only. n < 0: up to the NUL. */
int m_parse_hex(const char *t, int n, uint32_t *out);
int m_parse_dec(const char *t, int n, uint32_t *out);

/* A bounded text buffer. Writes past cap are dropped and set `full`. */
typedef struct
{
    char *p;
    int len, cap;
    int full;
} sbuf_t;

void sb_init(sbuf_t *b, char *mem, int cap);
void sb_reset(sbuf_t *b);
void sb_putc(sbuf_t *b, char c);
void sb_put(sbuf_t *b, const char *s);
void sb_putn(sbuf_t *b, const char *s, int n);
void sb_hex(sbuf_t *b, uint32_t v, int digits); /* lower case */
void sb_HEX(sbuf_t *b, uint32_t v, int digits); /* upper case */
void sb_hexmin(sbuf_t *b, uint32_t v); /* hex, no leading zeros (at least one digit) */
void sb_dec(sbuf_t *b, uint64_t v);
void sb_sdec(sbuf_t *b, int64_t v);
void sb_spaces(sbuf_t *b, int n);
const char *sb_str(sbuf_t *b); /* NUL-terminated; the NUL is not counted */

#endif
