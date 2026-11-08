/*
 * crtbuf.c - the CRT files the host tests write (T57 spec 8.1), shared by the harness and the core-only link.
 */

#include "crtbuf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void put16(uint8_t *p, unsigned v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
void put32(uint8_t *p, uint32_t v) { put16(p, v >> 16); put16(p + 2, v & 0xFFFF); }

void crt_head(crtbuf_t *b, int type, int exrom, int game, const char *name)
{
    b->d = calloc(1, 9 * 1024 * 1024);
    b->n = 0x40;
    memcpy(b->d, "C64 CARTRIDGE   ", 16);
    put32(b->d + 0x10, 0x40);
    put16(b->d + 0x14, 0x0100);
    put16(b->d + 0x16, (unsigned)type);
    b->d[0x18] = (uint8_t)exrom;
    b->d[0x19] = (uint8_t)game;
    strncpy((char *)b->d + 0x20, name, 32);
}
void crt_chip(crtbuf_t *b, int bank, unsigned load, const uint8_t *data, unsigned size)
{
    uint8_t *h = b->d + b->n;
    memcpy(h, "CHIP", 4);
    put32(h + 4, 0x10 + size);
    put16(h + 8, 0);
    put16(h + 10, (unsigned)bank);
    put16(h + 12, load);
    put16(h + 14, size);
    memcpy(h + 16, data, size);
    b->n += 16 + size;
}
// Where the test files live; both drivers set it once
static char s_root[256];

void crt_root(const char *root) { snprintf(s_root, sizeof(s_root), "%s", root); }

void crt_save(crtbuf_t *b, const char *rel)
{
    char p[512];
    snprintf(p, sizeof(p), "%s%s", s_root, rel);
    FILE *f = fopen(p, "wb");
    fwrite(b->d, 1, b->n, f);
    fclose(f);
    free(b->d);
}

// A mostly erased 8K chunk with a little data (a write then programs few bytes, a verify still compares 8K)
void sparse(uint8_t *d, uint32_t seed, int first_ff)
{
    memset(d, 0xFF, 0x2000);
    uint32_t x = seed * 2654435761u + 1;
    for (int i = 0; i < 48; i++) {
        x = x * 1664525u + 1013904223u;
        d[(x >> 8) % 0x2000] = (uint8_t)(x >> 24);
    }
    if (first_ff)
        d[0] = (uint8_t)(0x40 + (seed & 0x3F));
}

void make_write_crt(const char *cart, const char *rel)
{
    crtbuf_t b;
    uint8_t d[0x2000];
    if (!strcmp(cart, "ef1") || !strcmp(cart, "ef3")) {
        static const int banks[] = { 0, 5, 63 };
        crt_head(&b, 32, 1, 0, "TEST EF");
        for (int i = 0; i < 3; i++) {
            sparse(d, 100u + (unsigned)banks[i], 1);
            if (banks[i] == 0) {
                memcpy(d + 0x1800, "eapi", 4); // the EAPI of the CRT is replaced (replace_eapi)
            }
            crt_chip(&b, banks[i], 0x8000, d, 0x2000);
            sparse(d, 200u + (unsigned)banks[i], 1);
            if (banks[i] == 0)
                memcpy(d + 0x1800, "eapi", 4);
            crt_chip(&b, banks[i], 0xE000, d, 0x2000);
        }
    } else if (!strcmp(cart, "megabyter") || !strcmp(cart, "megacart") || !strcmp(cart, "gmod2")) {
        int type = !strcmp(cart, "megabyter") ? 86 : !strcmp(cart, "megacart") ? 88 : 60;
        static const int mb[] = { 0, 1, 127 }, mc[] = { 0, 200, 255 }, gm[] = { 0, 3, 63 };
        const int *banks = type == 86 ? mb : type == 88 ? mc : gm;
        crt_head(&b, type, 0, 1, "TEST");
        for (int i = 0; i < 3; i++) {
            sparse(d, 300u + (unsigned)banks[i], 1);
            crt_chip(&b, banks[i], 0x8000, d, 0x2000);
        }
        if (type == 60) {
            uint8_t ee[2048];
            for (int i = 0; i < 2048; i++)
                ee[i] = (uint8_t)(i * 3 + 7);
            crt_chip(&b, 0, 0xDE00, ee, 2048);
        }
    } else {
        crt_head(&b, 0, 0, 0, "ROM");
        sparse(d, 1, 1);
        crt_chip(&b, 0, 0x8000, d, 0x2000);
        crt_chip(&b, 0, 0xA000, d, 0x2000);
    }
    crt_save(&b, rel);
}
