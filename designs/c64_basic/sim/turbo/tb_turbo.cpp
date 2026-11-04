// SPDX-License-Identifier: GPL-3.0-or-later
// c64_core under Verilator for the T29 turbo tests (docs/specs/T29-turbo.md
// §8.1). A command interpreter on stdin, driven by run.py:
//
//   boot <state>            cold start to READY, save the model
//   restore <state>         load a saved model
//   save <state>
//   load <prg>              the PRG's bytes into the RAM through the second port
//   sys <addr>              type SYS <addr> into the keyboard buffer
//   run <cycles>            run C64 (PHI2) cycles
//   peek <addr> [n]         n bytes (default 1), printed as hex
//   poke <addr> <val>
//   cfg <en> <prefer>       the app's setting + strobe (page "011" 0x02, 0x2D, 0x2E)
//   halt | go               halt_req (pauses the machine through the gate), release
//   stat                    turbo_stat, turbo_on, cycles
//   cycles                  C64 cycles since the start of the run
//   until <addr> <val> <max>   run until the byte equals val (prints the cycles used)
//   grab <file> [lines]     the VIC pixel stream of the next frame: x y colour per line
//   vmode <n>               System Mode request (vmode input; the mode_ctl.v handshake is not modelled)
//   irqcnt                  CIA/VIC independent; print the core's raw time base values
//   dbgreg <addr>           trx_dbg register read (page 3 words), e.g. 0x402
//   dbgwr <addr> <val>
//   reu <0|1>               the REU on or off ($DF00-$DFFF is the REU's with it on)
//   trace on|off            (trace build) VCD
// Everything printed starts with "> " so run.py can find it.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <sstream>
#include <iostream>
#include "Vc64_core.h"
#include "verilated.h"
#include "verilated_save.h"
#ifdef TRACE
#include "verilated_vcd_c.h"
#include "Vc64_core___024root.h"
#endif

static Vc64_core *top;
static uint64_t clocks;
static uint64_t halted_clocks;     // clocks the gate was shut
#ifdef TRACE
static VerilatedVcdC *tfp;
static bool tracing;
#endif

static void tick()
{
    // the BUFGCE: gclk's edge passes if the enable was high before it
    bool pass = top->gate_ce;
    top->clk = 1; top->gclk = pass; top->snd_clk = 1; top->cart_wclk = 1; top->clk2x = 1;
    top->eval();
    top->clk2x = 0;
    top->eval();
#ifdef TRACE
    if (tracing) tfp->dump(clocks * 2);
#endif
    top->clk = 0; top->gclk = 0; top->snd_clk = 0; top->cart_wclk = 0; top->clk2x = 1;
    top->eval();
    top->clk2x = 0;
    top->eval();
#ifdef TRACE
    if (tracing) tfp->dump(clocks * 2 + 1);
#endif
    if (!pass) halted_clocks++;
    clocks++;
}

static void run_cycles(uint64_t n) { for (uint64_t i = 0; i < n * 32; i++) tick(); }

static uint8_t peek(uint16_t a)
{
    top->dbg_addr = a; top->dbg_req = 1;
    tick(); tick();
    top->dbg_req = 0;
    uint8_t v = top->dbg_rdata;
    tick();
    return v;
}

static void poke(uint16_t a, uint8_t v)
{
    top->dbg_addr = a; top->dbg_wdata = v; top->dbg_we = 1; top->dbg_req = 1;
    tick(); tick();
    top->dbg_we = 0; top->dbg_req = 0;
    tick();
}

static void init()
{
    { const char *v = getenv("VMODE"); top->vmode = v ? atoi(v) & 3 : 0; }   // T40 System Mode for the boot
    top->rst = 1; top->snd_rst = 1; top->audio_sample_en = 0;
    top->keys = 0; top->joy1 = 0; top->joy2 = 0; top->restore = 0;
    top->dbg_addr = 0; top->dbg_we = 0; top->dbg_wdata = 0; top->dbg_req = 0;
    top->reu_en = 1; top->reu_size = 5; top->reu_cmd_ready = 1; top->reu_rdata = 0; top->reu_rdata_valid = 0;
    top->cpu_reset_req = 0; top->cpu_pause_req = 0;
    top->cart_type = 0; top->cart_kill_tgl = 0; top->cart_force_tgl = 0; top->cart_ack_tgl = 0;
    top->cart_fill_be = 0; top->cart_fill_addr = 0; top->cart_fill_data = 0;
    top->dma_req = 0; top->dma_addr = 0; top->dma_we = 0; top->dma_wdata = 0; top->ultimax = 0;
    top->iec_atn_i = 1; top->iec_clk_i = 1; top->iec_data_i = 1;
    top->halt_req = 0; top->sb_we = 0; top->sb_ldreq = 0; top->sb_addr = 0; top->sb_waddr = 0; top->sb_wdata = 0; top->sb_ldmask = 0;
    top->turbo_en = 0; top->turbo_prefer = 0x80; top->turbo_upd_tgl = 0; top->turbo_ran_clr = 0;
    for (int i = 0; i < 64; i++) tick();
    top->rst = 0; top->snd_rst = 0;
}

static std::string row(int r)
{
    std::string s;
    for (int i = 0; i < 40; i++) {
        uint8_t c = peek(0x0400 + r * 40 + i) & 0x7F;
        if (c == 0) s += '@';
        else if (c < 27) s += char('A' + c - 1);
        else if (c < 64) s += char(c);
        else s += '.';
    }
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

static int boot(const char *state)
{
    init();
    for (int n = 0; n < 400; n++) {
        run_cycles(10000);
        for (int r = 0; r < 25; r++)
            if (row(r).rfind("READY.", 0) == 0) {
                run_cycles(20000);
                VerilatedSave os;
                os.open(state);
                os << clocks << *top;
                printf("> READY after %llu cycles\n", (unsigned long long)(clocks / 32));
                return 0;
            }
    }
    printf("> no READY\n");
    return 3;
}

static void cfg(int en, int prefer)
{
    top->turbo_en = en; top->turbo_prefer = prefer;
    for (int i = 0; i < 4; i++) tick();
    top->turbo_upd_tgl = !top->turbo_upd_tgl;
    for (int i = 0; i < 8; i++) tick();
}

static void load_prg(const char *f)
{
    FILE *fp = fopen(f, "rb");
    if (!fp) { printf("> ERR cannot open %s\n", f); return; }
    std::vector<uint8_t> prg;
    int c;
    while ((c = fgetc(fp)) != EOF) prg.push_back(uint8_t(c));
    fclose(fp);
    uint16_t load = prg[0] | prg[1] << 8;
    for (size_t i = 2; i < prg.size(); i++) poke(uint16_t(load + i - 2), prg[i]);
    printf("> loaded %zu bytes at %04X\n", prg.size() - 2, load);
}

static void sys(unsigned a)
{
    char cmd[32];
    snprintf(cmd, sizeof cmd, "SYS%u\r", a);
    int n = strlen(cmd);
    for (int i = 0; i < n; i++) poke(0x0277 + i, uint8_t(cmd[i]));
    poke(0x00C6, n);
}

int main(int argc, char **argv)
{
    Verilated::commandArgs(argc, argv);
    top = new Vc64_core;
#ifdef TRACE
    Verilated::traceEverOn(true);
    tfp = new VerilatedVcdC;
    top->trace(tfp, 99);
    tfp->open(getenv("TRACE_FILE") ? getenv("TRACE_FILE") : "trace.vcd");
#endif
    setvbuf(stdout, nullptr, _IOLBF, 0);
    std::string line;
    int rc = 0;
    while (std::getline(std::cin, line)) {
        std::istringstream is(line);
        std::string cmd;
        is >> cmd;
        if (cmd.empty() || cmd[0] == '#') continue;
        if (cmd == "init") init();
        else if (cmd == "boot") { std::string s; is >> s; rc = boot(s.c_str()); }
        else if (cmd == "restore") {
            std::string s; is >> s;
            VerilatedRestore r; r.open(s.c_str()); r >> clocks >> *top;
            halted_clocks = 0;
            printf("> restored at cycle %llu\n", (unsigned long long)(clocks / 32));
        }
        else if (cmd == "save") {
            std::string s; is >> s;
            VerilatedSave os; os.open(s.c_str()); os << clocks << *top;
        }
        else if (cmd == "load") { std::string s; is >> s; load_prg(s.c_str()); }
        else if (cmd == "sys") { unsigned a; is >> std::hex >> a; sys(a); }
        else if (cmd == "run") { uint64_t n; is >> n; run_cycles(n); }
        else if (cmd == "peek") {
            unsigned a, n = 1; is >> std::hex >> a >> std::dec;
            if (is >> n) {}
            printf("> %04X:", a);
            for (unsigned i = 0; i < n; i++) printf(" %02X", peek(uint16_t(a + i)));
            printf("\n");
        }
        else if (cmd == "poke") { unsigned a, v; is >> std::hex >> a >> v; poke(uint16_t(a), uint8_t(v)); }
        else if (cmd == "cfg") { unsigned en, pr; is >> std::hex >> en >> pr; cfg(en, pr); }
        else if (cmd == "halt") { top->halt_req = 1; }
        else if (cmd == "go") { top->halt_req = 0; }
        else if (cmd == "stat") {
            printf("> stat %08X on=%d cycles=%llu halted_clk=%llu\n", top->turbo_stat, top->turbo_on,
                   (unsigned long long)(clocks / 32), (unsigned long long)halted_clocks);
        }
        else if (cmd == "cycles") { printf("> cycles %llu\n", (unsigned long long)(clocks / 32)); }
        else if (cmd == "until") {
            unsigned a, v; uint64_t mx; is >> std::hex >> a >> v >> std::dec >> mx;
            uint64_t c0 = clocks / 32; bool ok = false;
            while (clocks / 32 - c0 < mx) {
                run_cycles(500);
                if (peek(uint16_t(a)) == v) { ok = true; break; }
            }
            printf("> until %s %llu\n", ok ? "ok" : "timeout", (unsigned long long)(clocks / 32 - c0));
        }
        else if (cmd == "grab") {
            std::string f; is >> f;
            FILE *fp = fopen(f.c_str(), "w");
            // from the start of a frame to the next
            while (!(top->pix_y == 0 && top->pix_x == 0)) tick();
            while (top->pix_y == 0) tick();
            int px_x = -1, px_y = -1;
            while (top->pix_y != 0) {
                tick();
                if (int(top->pix_x) != px_x || int(top->pix_y) != px_y) {
                    px_x = top->pix_x; px_y = top->pix_y;
                    fprintf(fp, "%d %d %d\n", px_x, px_y, top->pix_color);
                }
            }
            fclose(fp);
            printf("> grab done\n");
        }
        else if (cmd == "vmode") { int m; is >> m; top->vmode = m; }
        else if (cmd == "dbgreg") {
            unsigned a; is >> std::hex >> a;
            top->dr_addr = 0x400 | (a & 0x3FF); top->dr_re = 1; tick(); tick(); top->dr_re = 0;
            printf("> dbgreg %03X = %08X\n", a, top->dr_rdata);
        }
        else if (cmd == "trace") {
#ifdef TRACE
            std::string m; is >> m; tracing = m == "on";
#endif
        }
        else if (cmd == "watch") {
#ifdef TRACE
            // watch <n>: one line per C64 clock for n clocks (turbo engine internals)
            int n; is >> n;
            auto *r = top->rootp;
            for (int i = 0; i < n; i++) {
                tick();
                printf("> w gph=%2d sl=%d iss=%d tm=%d addr=%04X we=%d tce=%d slotd=%d rdy=%d pn=%d dn=%d\n",
                       r->c64_core__DOT__c64__DOT__turbo__DOT__gph, r->c64_core__DOT__c64__DOT__turbo__DOT__sl_st,
                       r->c64_core__DOT__c64__DOT__turbo__DOT__issue, r->c64_core__DOT__c64__DOT__t_mode,
                       r->c64_core__DOT__c64__DOT__cpu_addr, r->c64_core__DOT__c64__DOT__cpu_we,
                       r->c64_core__DOT__c64__DOT__t_ce, r->c64_core__DOT__c64__DOT__turbo__DOT__slow_done,
                       r->c64_core__DOT__c64__DOT__cpu_rdy, r->c64_core__DOT__c64__DOT__turbo__DOT__pn,
                       r->c64_core__DOT__c64__DOT__turbo__DOT__cap_pend);
            }
#endif
        }
        else if (cmd == "cpu") {
#ifdef TRACE
            auto *r = top->rootp;
            printf("> cpu addr=%04X we=%d tmode=%d g_edge=%d pc=%04X run2=%d\n",
                   r->c64_core__DOT__c64__DOT__cpu_addr, r->c64_core__DOT__c64__DOT__cpu_we,
                   r->c64_core__DOT__c64__DOT__t_mode, r->c64_core__DOT__c64__DOT__g_edge,
                   r->c64_core__DOT__c64__DOT__cpu_pc, r->c64_core__DOT__c64__DOT__run2);
#endif
        }
        else if (cmd == "sbr") {
            unsigned a; is >> std::hex >> a;
            top->sb_addr = a; for (int i = 0; i < 4; i++) tick();
            printf("> sbr %03X = %08X\n", a, top->sb_rdata);
        }
        else if (cmd == "sbw") {
            unsigned a, v; is >> std::hex >> a >> v;
            top->sb_waddr = a; top->sb_wdata = v; top->sb_we = 1; tick(); tick(); top->sb_we = 0; tick();
        }
        else if (cmd == "sbld") { unsigned m; is >> std::hex >> m; top->sb_ldmask = m; top->sb_ldreq = 1; tick(); tick(); top->sb_ldreq = 0; }
        else if (cmd == "reset") { top->cpu_reset_req = 1; for (int i = 0; i < 200; i++) tick(); top->cpu_reset_req = 0; }
        else if (cmd == "pause") { int v; is >> v; top->cpu_pause_req = v; }
        else if (cmd == "reu") { int v; is >> v; top->reu_en = v; }
        else if (cmd == "echo") { std::string r; std::getline(is, r); printf(">%s\n", r.c_str()); }
        else if (cmd == "quit") break;
        else printf("> ERR unknown command %s\n", cmd.c_str());
    }
#ifdef TRACE
    tfp->close();
#endif
    top->final();
    delete top;
    return rc;
}
