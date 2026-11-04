// SPDX-License-Identifier: GPL-3.0-or-later
// T25.1 SID engine: combined-waveform table generator.
//
// Provenance: the table contents are computed by libresidfp's
// WaveformCalculator (Leandro Nini, Antti Lankila, Dag Lem; GPL-2.0-or-later),
// linked from a pinned checkout (tools/fetch_libresidfp.sh), with its AVERAGE
// parameter sets for the 6581 and the 8580. This file only re-indexes and
// compresses them; it copies no reSIDfp code.
//
// reSIDfp computes a combined waveform in two steps: the "input" value
//   wv = base(acc) & pulse_mask & noise_mask     (base: tri, saw, saw&saw<<1)
// and then pulldown[t][wv] for t = ST, PT, PS, PST, NP. We store pulldown by
// that input value, so noise combinations are covered too, and keep the top 8
// bits (OSC3, the noise write-back and the MSB pulldown use only those; the
// low nibble is 0, as in reSID 0.16's sampled tables).
//
// Index space per model (16384 entries, padded):
//   ST  0x0000 + wv        (4096)
//   PT  0x1000 + wv>>1     (2048, the triangle's LSB is always 0)
//   PS  0x1800 + wv        (4096)
//   PST 0x2800 + wv        (4096)
//   NP  0x3800 + wv>>4     (256, noise and pulse only: wv low nibble is 0)
// e = {model, index14}. Blocks of 8 entries are deduplicated:
//   dir[e >> 3]            -> 9-bit block number   (4096 x 9, one RAMB36)
//   data[{blk, e[2:0]}]    -> 8-bit output >> 4    (4096 x 8, one RAMB36)
// Block 0 is all zero.
//
//   gen_tables <outdir>   writes sid_wf_dir.mem, sid_wf_data.mem
#include "WaveformCalculator.h"
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

using namespace reSIDfp;

static const int BASE[5] = {0x0000, 0x1000, 0x1800, 0x2800, 0x3800};
static const int SIZE[5] = {4096, 2048, 4096, 4096, 256};

static unsigned wv_of(int t, int idx)
{
    return t == 1 ? unsigned(idx) << 1 : t == 4 ? unsigned(idx) << 4 : unsigned(idx);
}

int main(int argc, char** argv)
{
    std::string out = argc > 1 ? argv[1] : ".";
    std::vector<int> flat(32768, 0);
    rc_matrix_t pd[2];
    for (int m = 0; m < 2; m++) {
        pd[m] = WaveformCalculator::getInstance()->buildPulldownTable(m ? CSG8580 : MOS6581, AVERAGE);
        for (int t = 0; t < 5; t++)
            for (int i = 0; i < SIZE[t]; i++)
                flat[m * 16384 + BASE[t] + i] = ((*pd[m])[t][wv_of(t, i)] >> 4) & 0xff;
    }
    std::map<std::vector<int>, int> blocks;
    std::vector<std::vector<int>> data;
    data.push_back(std::vector<int>(8, 0));
    blocks[data[0]] = 0;
    std::vector<int> dir(4096, 0);
    for (int b = 0; b < 4096; b++) {
        std::vector<int> blk(flat.begin() + b * 8, flat.begin() + b * 8 + 8);
        auto it = blocks.find(blk);
        if (it == blocks.end()) {
            int n = int(data.size());
            blocks[blk] = n;
            data.push_back(blk);
            dir[b] = n;
        } else {
            dir[b] = it->second;
        }
    }
    if (data.size() > 512) {
        fprintf(stderr, "too many blocks: %zu\n", data.size());
        return 1;
    }
    // round trip
    for (int e = 0; e < 32768; e++)
        if (data[dir[e >> 3]][e & 7] != flat[e]) {
            fprintf(stderr, "round trip mismatch at %x\n", e);
            return 1;
        }
    // NP: how often would a nonzero low nibble of wv change the top 8 bits?
    for (int m = 0; m < 2; m++) {
        int diff = 0;
        for (unsigned wv = 0; wv < 4096; wv++)
            if (((*pd[m])[4][wv] >> 4) != ((*pd[m])[4][wv & 0xff0] >> 4)) diff++;
        fprintf(stderr, "model %d: NP with a nonzero low nibble differs in %d of 4096 inputs\n", m, diff);
    }
    FILE* f = fopen((out + "/sid_wf_dir.mem").c_str(), "w");
    if (!f) return 1;
    fprintf(f, "// T25.1 combined waveforms, block directory (gen_tables.cpp, data from libresidfp GPL-2+)\n");
    for (int b = 0; b < 4096; b++) fprintf(f, "%03x\n", dir[b]);
    fclose(f);
    f = fopen((out + "/sid_wf_data.mem").c_str(), "w");
    if (!f) return 1;
    fprintf(f, "// T25.1 combined waveforms, 8-entry blocks, output bits 11..4 (gen_tables.cpp)\n");
    for (int a = 0; a < 4096; a++) {
        int b = a >> 3;
        fprintf(f, "%02x\n", b < int(data.size()) ? data[b][a & 7] : 0);
    }
    fclose(f);
    fprintf(stderr, "%zu blocks, %zu data entries\n", data.size(), data.size() * 8);
    return 0;
}
