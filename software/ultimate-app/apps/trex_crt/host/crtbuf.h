/*
 * crtbuf.h - building the CRT files of the host tests. Test code, no part of the app.
 */
#ifndef CRTBUF_H
#define CRTBUF_H

#include <stddef.h>
#include <stdint.h>

typedef struct { uint8_t *d; size_t n; } crtbuf_t;

void put16(uint8_t *p, unsigned v);
void put32(uint8_t *p, uint32_t v);
void crt_head(crtbuf_t *b, int type, int exrom, int game, const char *name);
void crt_chip(crtbuf_t *b, int bank, unsigned load, const uint8_t *data, unsigned size);
// The directory the files are written under, set once before the first crt_save()
void crt_root(const char *root);
void crt_save(crtbuf_t *b, const char *rel);
// A mostly erased 8K chunk with a little data
void sparse(uint8_t *d, uint32_t seed, int first_ff);
// The CRT the write tests of this cartridge use
void make_write_crt(const char *cart, const char *rel);

#endif
