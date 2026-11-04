// SPDX-License-Identifier: GPL-3.0-or-later
// T25.1 M1: run the reference model over a register-op log and print the
// golden output: a CRC-32 over every cycle's 20-bit sample, the per-voice
// DCA outputs and OSC3/ENV3, plus the sample range. Optional 16-bit mono WAV
// (box average of 20 cycles, ~49 kHz) for listening.
//
//   sidref_run <memdir> <log> <6581|8580> <res 0|1> <digi 0..3> <curve.mem> [out.wav]
#include "sidref.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

static uint32_t crc_tab[256];
static void crc_init()
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
        crc_tab[i] = c;
    }
}
static uint32_t crc_add(uint32_t c, uint32_t v, int bytes)
{
    for (int i = 0; i < bytes; i++) c = crc_tab[(c ^ (v >> (8 * i))) & 0xff] ^ (c >> 8);
    return c;
}

static bool load_curve(const char* p, uint16_t* c)
{
    FILE* f = fopen(p, "r");
    if (!f) return false;
    char line[512];
    int i = 0;
    while (i < 1024 && fgets(line, sizeof line, f))
        if (line[0] != '/' && line[0] != '\n') c[i++] = uint16_t(strtoul(line, nullptr, 16));
    fclose(f);
    return i == 1024;
}

int main(int argc, char** argv)
{
    if (argc < 7) {
        fprintf(stderr, "usage: sidref_run memdir log 6581|8580 res digi curve.mem [out.wav]\n");
        return 2;
    }
    static trxsid::Tables tab;
    static uint16_t curve[1024];
    if (!tab.load(argv[1]) || !load_curve(argv[6], curve)) return 2;
    FILE* f = fopen(argv[2], "r");
    if (!f) return 2;
    crc_init();
    trxsid::Sid m(&tab);
    m.cfg.is8580 = strcmp(argv[3], "8580") == 0;
    m.cfg.res_high = atoi(argv[4]) != 0;
    m.cfg.digi = unsigned(atoi(argv[5]));
    m.cfg.curve = curve;
    m.reset();
    uint32_t crc_out = 0xffffffff, crc_v = 0xffffffff, crc_rd = 0xffffffff;
    int32_t lo = 0, hi = 0;
    long cyc = 0;
    std::vector<int16_t> wav;
    int64_t acc = 0;
    int nacc = 0;
    auto clock1 = [&]() {
        m.clock();
        cyc++;
        crc_out = crc_add(crc_out, uint32_t(m.out) & 0xfffff, 3);
        for (int v = 0; v < 3; v++) crc_v = crc_add(crc_v, uint32_t(m.vo[v]), 4);
        crc_v = crc_add(crc_v, (m.w[2].osc3 >> 4) | (m.e[2].env3 << 8), 2);
        if (m.out < lo) lo = m.out;
        if (m.out > hi) hi = m.out;
        acc += m.out;
        if (++nacc == 20) {
            wav.push_back(int16_t(acc / 20 >> 4));
            acc = 0;
            nacc = 0;
        }
    };
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char* h = strchr(line, '#');
        if (h) *h = 0;
        long idle;
        char op;
        unsigned r, val = 0;
        if (sscanf(line, "%ld %c %x %x", &idle, &op, &r, &val) < 2) continue;
        for (long i = 0; i < idle; i++) clock1();
        if (op == 'W') m.write(r, uint8_t(val));
        else crc_rd = crc_add(crc_rd, m.read(r), 1);
        clock1();
    }
    fclose(f);
    printf("%s %s res%s digi%s %s: cycles %ld out %08x voices %08x reads %08x range %d..%d\n", argv[2], argv[3],
           argv[4], argv[5], argv[6], cyc, ~crc_out, ~crc_v, ~crc_rd, lo, hi);
    if (argc > 7) {
        FILE* o = fopen(argv[7], "wb");
        if (!o) return 2;
        uint32_t n = uint32_t(wav.size() * 2), rate = 985248 / 20;
        auto u32 = [&](uint32_t x) { fwrite(&x, 4, 1, o); };
        auto u16 = [&](uint16_t x) { fwrite(&x, 2, 1, o); };
        fwrite("RIFF", 4, 1, o); u32(36 + n); fwrite("WAVEfmt ", 8, 1, o); u32(16); u16(1); u16(1);
        u32(rate); u32(rate * 2); u16(2); u16(16); fwrite("data", 4, 1, o); u32(n);
        fwrite(wav.data(), 2, wav.size(), o);
        fclose(o);
    }
    return 0;
}
