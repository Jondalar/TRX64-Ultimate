// SPDX-License-Identifier: GPL-3.0-or-later
// T25.4: the whole c64_core with the 8-SID reDIP (-DSID_MULTI) driven like the board: the app's register
// values for the UltiSID (ultimate_sys.vhd defaults or what the app writes), a 6502 program at $C000 that
// writes SID 1 over the real bus ($D418=$0F, $D405=$09, $D406=$F0, $D400=$25, $D401=$1C, $D404=$11), and
// the audio output of the core (after trx_audio) sampled once per I2S frame.
//   tb_audio_top [split 0..7] [cycles per window] [gain bytes hex ...]
// Prints peak and rms of the output per gain byte; exit 0 when every gain >= 0x10 is not silent.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include "Vc64_core.h"
#include "Vc64_core___024root.h"
#include "verilated.h"

static Vc64_core *top;
static uint64_t clocks;
static int snd_div;

static void tick()
{
    top->clk = 1; top->gclk = 1; top->snd_clk = 1; top->cart_wclk = 1; top->clk2x = 1;
    top->eval();
    top->clk2x = 0;
    top->eval();
    top->clk = 0; top->gclk = 0; top->snd_clk = 0; top->cart_wclk = 0; top->clk2x = 1;
    top->eval();
    top->clk2x = 0;
    top->eval();
    clocks++;
}
static void run_cycles(uint64_t n) { for (uint64_t i = 0; i < n * 32; i++) tick(); }
static uint8_t peek(uint16_t a)
{
    top->dbg_addr = a; top->dbg_req = 1;
    tick(); tick();
    top->dbg_req = 0;
    return top->dbg_rdata;
}
static void poke(uint16_t a, uint8_t v)
{
    top->dbg_addr = a; top->dbg_wdata = v; top->dbg_we = 1; top->dbg_req = 1;
    tick();
    top->dbg_we = 0; top->dbg_req = 0;
}
static std::string row(int r)
{
    std::string s;
    for (int i = 0; i < 40; i++) {
        uint8_t c = peek(0x0400 + r * 40 + i) & 0x7F;
        s += c == 0 ? '@' : c < 27 ? char('A' + c - 1) : c < 64 ? char(c) : '.';
    }
    return s;
}

int main(int argc, char** argv)
{
    Verilated::commandArgs(argc, argv);
    const int split = argc > 1 ? atoi(argv[1]) : 0;
    const uint64_t cycles = argc > 2 ? atoll(argv[2]) : 100000;
    unsigned gains[16]; int ngain = 0;
    for (int i = 3; i < argc && ngain < 16; i++) gains[ngain++] = strtoul(argv[i], 0, 16);
    if (!ngain) { gains[0] = 0x80; ngain = 1; }
    const unsigned gain = gains[0];
    top = new Vc64_core;
    top->vmode = 0;
    top->rst = 1; top->snd_rst = 1; top->audio_sample_en = 0;
    top->keys = 0; top->joy1 = 0; top->joy2 = 0; top->restore = 0;
    top->dbg_addr = 0; top->dbg_we = 0; top->dbg_wdata = 0;
    top->reu_en = 1; top->reu_size = 5; top->reu_cmd_ready = 1; top->reu_rdata = 0; top->reu_rdata_valid = 0;
    top->cpu_reset_req = 0; top->cpu_pause_req = 0;
    top->cart_type = 0; top->cart_kill_tgl = 0; top->cart_force_tgl = 0; top->cart_ack_tgl = 0;
    top->cart_fill_be = 0; top->cart_fill_addr = 0; top->cart_fill_data = 0;
    top->dma_req = 0; top->dma_addr = 0; top->dma_we = 0; top->dma_wdata = 0; top->ultimax = 0;
    top->iec_atn_i = 1; top->iec_clk_i = 1; top->iec_data_i = 1;
    // the app's UltiSID registers as ultimate_sys.vhd resets them (and the gains the menu sets)
    top->sid_cfg_base = 0x0140; top->sid_cfg_mask = 0xFEFE; top->sid_cfg_split = split;
    top->sid_cfg_8580 = 0; top->sid_cfg_res = 0; top->sid_cfg_digi = 0xA;
    top->sid_cv_clk = 0; top->sid_cv_we = 0; top->sid_cv_addr = 0; top->sid_cv_wdata = 0; top->sid_lv_addr = 0;
    top->sid_mix_gains = gain * 0x01010101u;
    for (int i = 0; i < 64; i++) tick();
    top->rst = 0; top->snd_rst = 0;
    bool ready = false;
    for (int n = 0; n < 400 && !ready; n++) {
        run_cycles(10000);
        for (int r = 0; r < 25; r++) if (row(r).rfind("READY.", 0) == 0) ready = true;
    }
    printf("%s after %llu cycles\n", ready ? "READY" : "no READY", (unsigned long long)(clocks / 32));
    if (!ready) return 3;
    static const uint8_t prg[] = {0xA9,0x0F,0x8D,0x18,0xD4, 0xA9,0x09,0x8D,0x05,0xD4, 0xA9,0xF0,0x8D,0x06,0xD4,
                                  0xA9,0x25,0x8D,0x00,0xD4, 0xA9,0x1C,0x8D,0x01,0xD4, 0xA9,0x11,0x8D,0x04,0xD4,
                                  0x4C,0x1E,0xC0};
    for (unsigned i = 0; i < sizeof prg; i++) poke(0xC000 + i, prg[i]);
    const char *cmd = "SYS49152\r";
    for (int i = 0; i < 9; i++) poke(0x0277 + i, uint8_t(cmd[i]));
    poke(0x00C6, 9);
    // the I2S frame: one sample_en per 31.5278 MHz / 48 kHz clocks; a window per gain byte
    // (the app's mixer bytes: 0x00 = muted, 0x80 = 0 dB; -12 dB source and -30 dB master give 1, and
    // pan x 1 / 256 = 0 in the app's own byte, see the spec)
    int rc = 0;
    for (int gi = 0; gi < ngain; gi++) {
        top->sid_mix_gains = gains[gi] * 0x01010101u;
        double sum2 = 0; long n = 0; int peak = 0; long nen = 0;
        const uint64_t t0 = clocks;
        while (clocks - t0 < cycles * 32) {
            top->audio_sample_en = (++snd_div % 657) == 0;
            tick();
            if (top->audio_sample_en) {
                nen++;
                int l = (int32_t)(top->audio_l << 8) >> 8, r = (int32_t)(top->audio_r << 8) >> 8;
                if (clocks - t0 > cycles * 16ull) {
                    peak = std::max(peak, std::max(abs(l), abs(r)));
                    sum2 += double(l) * l + double(r) * r; n += 2;
                }
            }
        }
        printf("gain byte %02x split %d: frames %ld peak %d (24 bit) rms %.1f\n", gains[gi], split, nen, peak, n ? sqrt(sum2 / n) : 0.0);
        if (gains[gi] >= 0x10 && !peak) rc = 1;
    }
    return rc;

}
