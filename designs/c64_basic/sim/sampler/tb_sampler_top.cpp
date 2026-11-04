// SPDX-License-Identifier: GPL-3.0-or-later
// T25.6: the whole c64_core with the sampler: a 6502 program (top_prg.asm) does the app's detection over the real
// bus, starts two voices (left 390 Hz, right 781 Hz from the REU area of a DDR2 model behind the sampler's read
// handshake) and one with IRQ; checks the detection result, the IRQ count, the tones in the audio after trx_audio.
//   tb_sampler_top [smp_en 0|1] [cycles after the program starts, default 150000]
//
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
static uint8_t *ddr;
static int req_last = 0, resp_cnt = -1;
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
    // DDR2 behind the sampler's toggle handshake: 14 clocks per 32 bit read
    if (resp_cnt < 0 && top->smp_f_req_tgl != req_last) {
        req_last = top->smp_f_req_tgl; resp_cnt = 14;
        uint32_t a = top->smp_f_addr & ~3u;
        top->smp_f_data = ddr[a] | ddr[a + 1] << 8 | ddr[a + 2] << 16 | (uint32_t)ddr[a + 3] << 24;
    } else if (resp_cnt > 0) resp_cnt--;
    if (resp_cnt == 0) { top->smp_f_done_tgl = !top->smp_f_done_tgl; resp_cnt = -1; }
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


static double goertzel(const std::vector<double> &x, double f, double fs)
{
    double w = 2 * M_PI * f / fs, c = 2 * cos(w), s1 = 0, s2 = 0;
    for (double v : x) { double s = v + c * s1 - s2; s2 = s1; s1 = s; }
    return sqrt(s1 * s1 + s2 * s2 - c * s1 * s2) / (x.size() / 2.0);
}

int main(int argc, char** argv)
{
    Verilated::commandArgs(argc, argv);
    const int en = argc > 1 ? atoi(argv[1]) : 1;
    const uint64_t cycles = argc > 2 ? atoll(argv[2]) : 150000;
    ddr = (uint8_t *)calloc(1, 128u << 20);
    for (int i = 0; i < 2048; i++) ddr[0x1008000 + i] = (int8_t)lrint(100 * sin(2 * M_PI * i / 64));
    for (int i = 0; i < 2048; i++) ddr[0x1009000 + i] = (int8_t)lrint(100 * sin(2 * M_PI * i / 32));
    for (int i = 0; i < 16; i++) ddr[0x100A000 + i] = 40;
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
    top->sid_cfg_base = 0x0140; top->sid_cfg_mask = 0xFEFE; top->sid_cfg_split = 0;
    top->sid_cfg_8580 = 0; top->sid_cfg_res = 0; top->sid_cfg_digi = 0xA;
    top->sid_cv_clk = 0; top->sid_cv_we = 0; top->sid_cv_addr = 0; top->sid_cv_wdata = 0; top->sid_lv_addr = 0;
    top->sid_mix_gains = 0x80808080u;
    top->sid_mix_gains2 = 0x00808000u;                  // sampler L -> left, sampler R -> right, 0 dB (what the app's pans 0 / 10 give)
    top->smp_en = en;
    top->smp_a_tgl = 0; top->smp_a_we = 0; top->smp_a_addr = 0; top->smp_a_wdata = 0;
    top->smp_f_done_tgl = 0; top->smp_f_data = 0;
    // no BASIC boot (12 minutes of simulation): the program is entered through the reset vector and the IRQ vector
    // points at its handler; both are poked into the KERNAL ROM block while the machine is in reset
    FILE *f = fopen("top_prg.bin", "rb");
    if (!f) { printf("no top_prg.bin\n"); return 4; }
    static uint8_t prg[0x300]; memset(prg, 0, sizeof prg);
    size_t n = fread(prg, 1, sizeof prg, f); fclose(f);
    for (int i = 0; i < 64; i++) tick();
    for (size_t i = 0; i < 0x300; i++) poke(0xC000 + i, prg[i]);
    poke(0xC804, 0); poke(0xC800, 0xEE); poke(0xC801, 0); poke(0xC802, 0xEE);
    top->dbg_rom = 1;
    poke(0x3FFC, 0x00); poke(0x3FFD, 0xC0);             // reset  -> $C000
    poke(0x3FFE, 0x00); poke(0x3FFF, 0xC2);             // IRQ    -> $C200
    top->dbg_rom = 0;
    top->rst = 0; top->snd_rst = 0;
    std::vector<double> l, r;
    const uint64_t t0 = clocks;
    int irq_mid = -1;
    while (clocks - t0 < cycles * 32) {
        top->audio_sample_en = (++snd_div % 657) == 0;
        tick();
        if (top->audio_sample_en && clocks - t0 > cycles * 16ull) {
            l.push_back((int32_t)(top->audio_l << 8) >> 8);
            r.push_back((int32_t)(top->audio_r << 8) >> 8);
        }
        if (getenv("WTRACE") && top->rootp->c64_core__DOT__io_ev && top->rootp->c64_core__DOT__io_ev_io && top->rootp->c64_core__DOT__io_ev_we) printf("W cyc %llu %04x <- %02x\n", (unsigned long long)((clocks - t0) / 32), top->rootp->c64_core__DOT__io_ev_addr, top->rootp->c64_core__DOT__io_ev_data);
        if (getenv("TRACE") && ((clocks - t0) % 320000) == 0) printf("cyc %llu pc %04x irq %d nmi %d rdy %d paused %d ba %d\n", (unsigned long long)((clocks - t0) / 32), top->rootp->c64_core__DOT__c64__DOT__cpu_pc, top->rootp->c64_core__DOT__c64__DOT__cpu_irq, top->rootp->c64_core__DOT__c64__DOT__cpu_nmi, top->rootp->c64_core__DOT__c64__DOT__cpu_rdy, top->rootp->c64_core__DOT__c64__DOT__cpu_paused, top->rootp->c64_core__DOT__c64__DOT__vic_ba);
        if (irq_mid < 0 && clocks - t0 > cycles * 16ull) irq_mid = peek(0xC801);
    }
    { auto *rp = top->rootp;
      printf("sampler state: run %02x chg %02x irq_pend %02x sv_valid %d fbusy %d\n", rp->c64_core__DOT__smp__DOT__run, rp->c64_core__DOT__smp__DOT__chg,
             rp->c64_core__DOT__smp__DOT__irq_pend, rp->c64_core__DOT__smp__DOT__sv_valid, rp->c64_core__DOT__smp__DOT__fbusy);
#define W(v, w) ((uint32_t)rp->c64_core__DOT__smp__DOT__m0[(v) * 8 + (w)] | (uint32_t)rp->c64_core__DOT__smp__DOT__m1[(v) * 8 + (w)] << 8 | (uint32_t)rp->c64_core__DOT__smp__DOT__m2[(v) * 8 + (w)] << 16 | (uint32_t)rp->c64_core__DOT__smp__DOT__m3[(v) * 8 + (w)] << 24)
      for (int v = 0; v < 3; v++) printf("  voice %d: w0 %08x w1 %08x w2 %08x w3 %08x w6 %08x w7 %08x\n", v,
             W(v,0), W(v,1), W(v,2), W(v,3), W(v,6), W(v,7)); }
    int found = peek(0xC800), irqs = peek(0xC801), ver = peek(0xC802);
    printf("progress marker %d\n", peek(0xC804));
    printf("smp_en %d: detection %d (1 = found), version %02x, IRQs counted %d (at half time %d)\n", en, found, ver, irqs, irq_mid);
    int rc = 0;
    if (!en) {
        if (found != 0) { printf("FAIL window off but detected\n"); rc = 1; }
        printf(rc ? "tb_sampler_top FAIL\n" : "tb_sampler_top PASS\n");
        return rc;
    }
    double l390 = goertzel(l, 390.625, 47990), l781 = goertzel(l, 781.25, 47990);
    double r390 = goertzel(r, 390.625, 47990), r781 = goertzel(r, 781.25, 47990);
    printf("left: 390 Hz %.0f 781 Hz %.0f; right: 390 Hz %.0f 781 Hz %.0f (24 bit amplitude)\n", l390, l781, r390, r781);
    double s423 = std::max(goertzel(l, 423.0, 47990), goertzel(r, 423.0, 47990));
    printf("SID 1 triangle (423 Hz, written by the same program): %.0f\n", s423);
    if (!(s423 > 10000)) { printf("FAIL the SID path is silent\n"); rc = 1; }
    if (found != 1) { printf("FAIL not detected\n"); rc = 1; }
    if (ver != 1) { printf("FAIL version\n"); rc = 1; }
    if (irqs != 1 || irq_mid != 1) { printf("FAIL IRQ count (the sample ends once: 1)\n"); rc = 1; }
    if (!(l390 > 100000 && l781 < l390 / 20)) { printf("FAIL left channel not the 390 Hz voice\n"); rc = 1; }
    if (!(r781 > 100000 && r390 < r781 / 20)) { printf("FAIL right channel not the 781 Hz voice\n"); rc = 1; }
    printf(rc ? "tb_sampler_top FAIL\n" : "tb_sampler_top PASS\n");
    return rc;
}
