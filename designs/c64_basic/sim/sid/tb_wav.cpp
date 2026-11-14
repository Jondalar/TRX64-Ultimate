// SPDX-License-Identifier: GPL-3.0-or-later
// U12 D5: our own SID through its own output chain, as a .wav, on a host.
//
// c64_trx_sid is what c64_top instantiates: trx_sid (decode + the T25.1 engine)
// and T25.3's trx_audio (mixer gains, CIC, halfbands, fractional FIR, DC
// blocker). So what comes out here is what the board sends to I2S, and a
// judgement about how something sounds no longer costs a board session.
//
//   tb_wav <memdir> <curve.mem|-> <trace|-> <out.wav> [max C64 cycles]
//
// <curve.mem>  1024 x 16 bit hex (cores/trx_sid/model/gen_curves.py writes
//              curve0..6.mem), loaded into both UltiSIDs; "-" keeps the RAM's
//              power-up content from sid_curve_init.mem.
// <trace>      register ops, the format gen_logs.py writes:
//                "<idle> W <reg> <val>"   wait <idle> C64 cycles, then write
//                "<idle> R <reg>"         the same for a read
//              <idle> decimal, <reg> and <val> hex, <reg> is 00..1F of UltiSID 1 ($D400 + reg). "-" plays
//              the built-in demo instead, so the tool runs with no input file.
//
// env: MODEL8580=0|1 (the chip model of UltiSID 1, waveform tables),
//      DIGI=0..3 (the DC offset level, as the menu's Digis Level),
//      RES=0|1 (Filter Resonance Low/High), VMODE=0..3 (System Mode, T40).
#include "Vc64_trx_sid.h"
#include "verilated.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <vector>
#include <string>

struct Op { long idle; bool write; int reg; int val; };

// A tune would come from a register trace of the real thing; this is enough to
// hear that the chain is alive and to compare two builds: a triangle note, a
// sawtooth fifth above it, the filter swept over both, then a release.
static std::vector<Op> demo_trace()
{
    std::vector<Op> t;
    auto w = [&](long idle, int r, int v) { t.push_back({idle, true, r, v}); };
    const int C64_CYC_PER_MS = 985;
    w(0, 0x18, 0x1f);                       // volume 15, filter off
    w(0, 0x05, 0x08); w(0, 0x06, 0xa0);     // voice 1 attack/decay, sustain
    w(0, 0x0c, 0x08); w(0, 0x0d, 0xa0);     // voice 2
    // note A-3 on voice 1 (triangle), E-4 on voice 2 (sawtooth)
    w(0, 0x00, 0x2d); w(0, 0x01, 0x1c);
    w(0, 0x07, 0x61); w(0, 0x08, 0x2a);
    w(0, 0x15, 0x00); w(0, 0x16, 0x20);     // cutoff low
    w(0, 0x17, 0x13);                       // resonance 1, voices 1+2 filtered
    w(0, 0x18, 0x1f);
    w(0, 0x04, 0x11);                       // gate voice 1, triangle
    w(0, 0x0b, 0x21);                       // gate voice 2, sawtooth
    for (int i = 0; i < 48; i++)            // sweep the cutoff up over ~480 ms
        w(10 * C64_CYC_PER_MS, 0x16, 0x20 + i * 4);
    w(0, 0x18, 0x1f | 0x10);                // low-pass only
    for (int i = 0; i < 24; i++)
        w(10 * C64_CYC_PER_MS, 0x16, 0xd0 - i * 8);
    w(0, 0x04, 0x10);                       // release
    w(0, 0x0b, 0x20);
    w(400 * C64_CYC_PER_MS, 0x18, 0x00);
    return t;
}

static std::vector<Op> read_trace(const char* path)
{
    std::vector<Op> t;
    FILE* f = fopen(path, "r");
    if (!f) { fprintf(stderr, "tb_wav: cannot open %s\n", path); exit(2); }
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        long idle; char rw; unsigned reg, val;
        int n = sscanf(line, "%ld %c %x %x", &idle, &rw, &reg, &val);
        if (n < 3) continue;
        t.push_back({idle, rw == 'W' || rw == 'w', (int)(reg & 0x1f), n >= 4 ? (int)(val & 0xff) : 0});
    }
    fclose(f);
    return t;
}

static void put32(FILE* f, uint32_t v) { fwrite(&v, 4, 1, f); }
static void put16(FILE* f, uint16_t v) { fwrite(&v, 2, 1, f); }

int main(int argc, char** argv)
{
    Verilated::commandArgs(argc, argv);
    if (argc < 5) {
        fprintf(stderr, "tb_wav <memdir> <curve.mem|-> <trace|-> <out.wav> [cycles]\n");
        return 2;
    }
    const char* curve_path = argv[2];
    const char* trace_path = argv[3];
    const char* wav_path = argv[4];
    const long max_cyc = argc > 5 ? atol(argv[5]) : 4000000;   // ~4 s of C64 time

    const int vm = getenv("VMODE") ? atoi(getenv("VMODE")) & 3 : 0;
    const int model8580 = getenv("MODEL8580") ? atoi(getenv("MODEL8580")) & 1 : 0;
    const int digi = getenv("DIGI") ? atoi(getenv("DIGI")) & 3 : 2;      // the menu's default
    const int res_high = getenv("RES") ? atoi(getenv("RES")) & 1 : 0;

    std::vector<Op> ops = strcmp(trace_path, "-") ? read_trace(trace_path) : demo_trace();

    // T40 §9.2: phi2 per System Mode, and mclk is 12.288 MHz (81.375 ns)
    const double PHI2[4] = {50e6 * 56.75 / 3 / 30 / 32, 50e6 * 47.125 / 3 / 24 / 32,
                            50e6 * 43.25 / 3 / 22 / 32, 50e6 * 35.375 / 2 / 28 / 32};
    const double T2X = 1.0 / (PHI2[vm] * 64), TM = 81.375 / 1e9;

    Vc64_trx_sid* d = new Vc64_trx_sid;
    d->vmode = vm;
    d->cfg_base = 0x0140;                   // UltiSID 1 at $D400, UltiSID 2 off
    d->cfg_mask = 0xfefe;
    d->cfg_split = 0;
    d->cfg_8580 = model8580 ? 0x3 : 0x0;    // both chips, so a split cannot surprise
    d->cfg_res_high = res_high ? 0x3 : 0x0;
    d->cfg_digi = (digi << 2) | digi;
    d->cfg_rs8 = 0;
    d->mix_gains = 0x80808080;              // 0 dB
    d->mix_gains2 = 0;
    d->smp_l = 0;
    d->smp_r = 0;
    d->pot_x = 0;
    d->pot_y = 0;
    d->ps_v = 0;
    d->ra = 0;
    d->lv_addr = 0;
    d->cen = 1;                             // no access
    d->we = 0;
    d->phi2_n = 0;
    d->audio_sample_en = 0;
    d->reset = 1;
    d->reset_sample = 1;

    long t2count = 0;
    auto tick2 = [&]() {
        bool ce = (t2count & 1) == 0;       // clk rises on every other clk2x edge
        d->clk2x = 0; d->cv_clk = 0; if (ce) d->clk = 0;
        d->eval();
        d->clk2x = 1; d->cv_clk = 1; if (ce) d->clk = 1;
        d->eval();
        t2count++;
    };
    for (int i = 0; i < 8; i++) tick2();
    d->reset = 0;
    for (int i = 0; i < 100; i++) tick2();

    if (strcmp(curve_path, "-")) {          // the curve the test wants, into both chips
        FILE* cf = fopen(curve_path, "r");
        if (!cf) { fprintf(stderr, "tb_wav: cannot open %s\n", curve_path); return 2; }
        std::vector<unsigned> cv;
        char line[64];
        while (fgets(line, sizeof(line), cf)) {
            unsigned v;
            if (sscanf(line, "%x", &v) == 1) cv.push_back(v);
        }
        fclose(cf);
        if (cv.size() < 1024) { fprintf(stderr, "tb_wav: %s has %zu entries, 1024 wanted\n", curve_path, cv.size()); return 2; }
        for (int u = 0; u < 2; u++)
            for (int i = 0; i < 2048; i++) {
                d->cv_we = 1;
                d->cv_addr = (u << 11) | i;
                d->cv_wdata = (cv[i >> 1] >> (8 * (i & 1))) & 0xff;
                tick2();
            }
        d->cv_we = 0;
        tick2();
    }
    d->reset_sample = 0;

    // ---- the two clock domains, advanced by time like tb_audio does
    std::vector<int16_t> pcm;               // interleaved L R
    double t2 = 0, tm = TM / 3;
    long mcount = 0, sub = 0, cyc = 0;
    size_t op_i = 0;
    long idle_left = ops.empty() ? -1 : ops[0].idle;
    long peak = 0;
    double sq = 0;
    long nout = 0;

    while (cyc < max_cyc && (op_i < ops.size() || cyc < max_cyc)) {
        if (t2 <= tm) {
            // one clk2x edge; a C64 cycle is 64 of them, the bus op sits in it
            if (sub == 0) {
                bool acc = false;
                if (idle_left == 0 && op_i < ops.size()) {
                    const Op& o = ops[op_i];
                    acc = true;
                    d->we = o.write ? 1 : 0;
                    d->addr = 0x400 | o.reg;
                    d->din = o.val;
                    op_i++;
                    idle_left = op_i < ops.size() ? ops[op_i].idle : -1;
                } else if (idle_left > 0) {
                    idle_left--;
                }
                d->cen = acc ? 0 : 1;
                d->phi2_n = 0;
            }
            if (sub == 62) d->phi2_n = 1;
            bool ce = (t2count & 1) == 0;
            d->clk2x = 0; if (ce) d->clk = 0;
            d->eval();
            d->clk2x = 1; if (ce) d->clk = 1;
            d->eval();
            t2count++;
            t2 += T2X;
            if (++sub == 64) {
                sub = 0;
                cyc++;
                if (idle_left < 0 && op_i >= ops.size() && cyc > 1000) {
                    // the trace is done; let the release ring out for 200 ms
                    static long tail = 0;
                    if (++tail > 200 * 985) break;
                }
            }
        } else {
            d->audio_sample_en = ((mcount % 256) == 255) ? 1 : 0;
            d->clk_sample = 1; d->eval(); d->clk_sample = 0; d->eval();
            if (d->audio_sample_en) {
                int32_t l = (int32_t)(d->audio_sample_word_0 << 8) >> 8;
                int32_t r = (int32_t)(d->audio_sample_word_1 << 8) >> 8;
                pcm.push_back((int16_t)(l >> 8));
                pcm.push_back((int16_t)(r >> 8));
                if (labs(l) > peak) peak = labs(l);
                sq += (double)l * l;
                nout++;
            }
            mcount++;
            tm += TM;
        }
    }

    FILE* w = fopen(wav_path, "wb");
    if (!w) { fprintf(stderr, "tb_wav: cannot write %s\n", wav_path); return 2; }
    const uint32_t rate = 48000, bytes = (uint32_t)(pcm.size() * 2);
    fwrite("RIFF", 1, 4, w); put32(w, 36 + bytes); fwrite("WAVE", 1, 4, w);
    fwrite("fmt ", 1, 4, w); put32(w, 16); put16(w, 1); put16(w, 2);
    put32(w, rate); put32(w, rate * 4); put16(w, 4); put16(w, 16);
    fwrite("data", 1, 4, w); put32(w, bytes);
    fwrite(pcm.data(), 1, bytes, w);
    fclose(w);

    double rms = nout ? sqrt(sq / nout) : 0;
    printf("tb_wav: %ld C64 cycles, %ld frames (%.2f s at 48 kHz), peak %ld, rms %.0f, "
           "model %s, digi %d, res %s, curve %s\n",
           cyc, nout, nout / 48000.0, peak, rms, model8580 ? "8580" : "6581",
           digi, res_high ? "high" : "low", strcmp(curve_path, "-") ? curve_path : "power-up");
    delete d;
    return nout > 0 ? 0 : 1;
}
