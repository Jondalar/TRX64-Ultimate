#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Makes tb_ring_top.v and build.sh from sim/statebus (tb_sb_top.v, build.sh) and
# tb_ring_eng.vh, so the ring bench follows the statebus bench. Run from sim/:
#   python3 ring/gen_top.py
s=open('statebus/tb_sb_top.v').read()
hdr_end=s.index('module tb_sb_top (')
s="""// Top of the frame ring bench (tb_ring.cpp, T20 §13.1). Made by gen_top.py
// from ../statebus/tb_sb_top.v and tb_ring_eng.vh: change those, not this.
// tb_sb_top's machine (c64_core with its gate, dma_win, trx_sys, the text
// layer) plus the engine (trx_engine.v), cart_fill, mig_arb and the MIG UI
// model, as top.v wires them. ui_clk is the MIG's 75 MHz; the model's memory
// is 64 MB (addresses wrap: 0x5C00000 lands at 0x1C00000).
"""+s[hdr_end:]
s=s.replace("module tb_sb_top (\n    input  wire        clk,","module tb_ring_top (\n    input  wire        clk,\n    input  wire        ui_clk,")
def rep(a,b):
    global s
    assert a in s, a
    s=s.replace(a,b,1)
rep("""    wire  [7:0] cap_every;
    wire [15:0] cap_gen, cap_nosnap;
""","""    wire  [7:0] cap_every;
    wire [15:0] cap_gen, cap_nosnap;
    wire        log_on, seq_busy, mk, mk_snap, mk_cap, mk_skip, mk_intg, reu_busy, phi2_tick;
    wire  [9:0] mk_x;
    wire  [8:0] mk_line;
    wire        p2_we, p2_re, seq_sb_req, seq_sb_gnt;
    wire  [9:0] p2_addr;
    wire [31:0] p2_wdata, p2_rdata;
    wire [11:0] seq_sb_addr;
""")
rep("""        .rsp_sb_req(rsp_sb_req), .rsp_sb_addr(rsp_sb_addr), .sb_addr(sb_addr), .sb_rdata(sb_rdata),""","""        .p2_we(p2_we), .p2_re(p2_re), .p2_addr(p2_addr), .p2_wdata(p2_wdata), .p2_rdata(p2_rdata),
        .sb_we(sb_we), .sb_waddr(sb_waddr), .sb_wdata(sb_wdata), .sb_ldreq(sb_ldreq),
        .sb_ldmask(sb_ldmask), .sb_ldarm(sb_ldarm), .pos_set(pos_set), .pos_arm_in(pos_arm_in),
        .pos_cyc(pos_cyc), .pos_release(pos_release), .pos_arm(pos_arm), .pos_hold(pos_hold),
        .resume_phase(resume_phase), .rp_set(rp_set), .rp_pend(rp_pend),
        .rsp_sb_req(rsp_sb_req), .rsp_sb_addr(rsp_sb_addr), .seq_sb_req(seq_sb_req),
        .seq_sb_addr(seq_sb_addr), .seq_sb_gnt(seq_sb_gnt), .sb_addr(sb_addr), .sb_rdata(sb_rdata),""")
rep("""        .cap_every(cap_every), .cap_gen(cap_gen), .cap_tgl(cap_tgl), .cap_nosnap(cap_nosnap),
""","""        .cap_every(cap_every), .cap_gen(cap_gen), .cap_tgl(cap_tgl), .cap_nosnap(cap_nosnap),
        .log_on(log_on), .cap_block(seq_busy), .mk(mk), .mk_snap(mk_snap), .mk_cap(mk_cap),
        .mk_skip(mk_skip), .mk_intg(mk_intg), .reu_busy(reu_busy), .mk_x(mk_x), .mk_line(mk_line),
        .phi2_tick(phi2_tick),
""")
# engine block from the old generated top
eb=open('ring/tb_ring_eng.vh').read()
i=s.rindex("endmodule")
s=s[:i]+"\n"+eb+s[i:]
rep("""    output wire        sess,""","""    output wire        sess,
    output wire        eng_tgl,
    output wire        gap_tgl,
    // the cartridge (cart_fill on mig_arb's cart client) and the write taps
    input  wire  [7:0] cart_type,
    output reg  [31:0] n_s0 = 0,            // stream 0 / 1 writes made so far (log on)
    output reg  [31:0] n_s1 = 0,
    output reg  [31:0] n_s0_mk = 0,         // ... at the last marker's boundary
    output reg  [31:0] n_s1_mk = 0,
    output reg  [31:0] n_mk = 0,""")
rep("""        .cart_type(8'd0), .cart_kill_tgl(1'b0), .cart_force_tgl(1'b0), .cart_active(),
        .cart_wclk(clk), .cart_req_tgl(), .cart_req_we(), .cart_req_addr(), .cart_req_data(),
        .cart_req_slot(), .cart_req_cached(), .cart_ack_tgl(1'b0),""","""        .cart_type(cart_type), .cart_kill_tgl(1'b0), .cart_force_tgl(1'b0), .cart_active(),
        .cart_wclk(ui_clk), .cart_req_tgl(cart_req_tgl), .cart_req_we(cart_req_we),
        .cart_req_addr(cart_req_addr), .cart_req_data(cart_req_data),
        .cart_req_slot(cart_req_slot), .cart_req_cached(cart_req_cached), .cart_ack_tgl(cart_ack_tgl),""")
rep("""        .cart_fill_be(8'd0), .cart_fill_addr(9'd0), .cart_fill_data(64'd0),""","""        .cart_fill_be(cart_fill_be), .cart_fill_addr(cart_fill_addr), .cart_fill_data(cart_fill_data),
        .wa_we(wa_we), .wa_addr(wa_addr), .wa_new(wa_new), .wa_old(wa_old),
        .wc_we(wc_we), .wc_addr(wc_addr), .wc_new(wc_new), .wc_old(wc_old),
        .wk_we(wk_we), .wk_addr(wk_addr), .wk_new(wk_new),
        .sb_we(sb_we), .sb_waddr(sb_waddr), .sb_wdata(sb_wdata), .sb_ldreq(sb_ldreq),
        .sb_ldmask(sb_ldmask), .sb_ldarm(sb_ldarm), .pos_set(pos_set), .pos_arm_in(pos_arm_in),
        .pos_cyc(pos_cyc), .pos_release(pos_release), .pos_arm(pos_arm), .pos_hold(pos_hold),
        .resume_phase(resume_phase), .rp_set(rp_set), .rp_pend(rp_pend),""")
rep("""    wire [11:0] seq_sb_addr;
""","""    wire [11:0] seq_sb_addr;
    wire        wa_we, wc_we, wk_we;
    wire [15:0] wa_addr;
    wire  [7:0] wa_new, wa_old, wk_new;
    wire  [9:0] wc_addr;
    wire  [3:0] wc_new, wc_old;
    wire [14:0] wk_addr;
    wire        cart_req_tgl, cart_req_we, cart_req_cached, cart_ack_tgl;
    wire [22:0] cart_req_addr;
    wire  [7:0] cart_req_data, cart_fill_be;
    wire  [5:0] cart_req_slot;
    wire  [8:0] cart_fill_addr;
    wire [63:0] cart_fill_data;
    wire        sb_we, sb_ldreq, pos_set, pos_arm_in, pos_release, pos_arm, pos_hold;
    wire [11:0] sb_waddr;
    wire [31:0] sb_wdata;
    wire  [4:0] sb_ldmask, sb_ldarm;
    wire  [5:0] pos_cyc;
    wire [19:0] resume_phase, rp_pend;
    wire        rp_set;
""")
open('ring/tb_ring_top.v','w').write(s)
b=open('statebus/build.sh').read()
b=b.replace("# Build tb_sb_top (c64_core with its gate, freeze_resp.v, trx_sys.v) into a\n# Verilator model for tb_statebus.cpp.","# Build tb_ring_top (tb_sb_top's machine, the engine trx_engine.v, mig_arb\n# and the MIG model) into a Verilator model for tb_ring.cpp.")
b=b.replace("#   build.sh [--trace]   -> build/tb_statebus[_trace], run directory build/run","#   build.sh [--trace]   -> build/tb_ring[_trace], run directory build/run")
a="$D/c64_text.v $D/pal_ram.v $here/tb_sb_top.v"
assert a in b
b=b.replace(a,"$D/c64_text.v $D/pal_ram.v $D/trx_engine.v $D/ddr2_port.v $D/cart_fill.v $D/sim/mig_ui_model.v $here/tb_ring_top.v")
b=b.replace("--top-module tb_sb_top","--top-module tb_ring_top").replace("-o tb_statebus","-o tb_ring").replace('"$here/tb_statebus.cpp"','"$here/tb_ring.cpp"')
b=b.replace('rm -f "$out/tb_statebus$suffix"\ncp "$out/obj$suffix/tb_statebus" "$out/tb_statebus$suffix"\necho "built $out/tb_statebus$suffix"','rm -f "$out/tb_ring$suffix"\ncp "$out/obj$suffix/tb_ring" "$out/tb_ring$suffix"\necho "built $out/tb_ring$suffix"')
assert 'tb_statebus' not in b.replace('sim/statebus','')
b=b.replace('out=$here/build\n', 'out=$here/build${SB_FULL:+_full}   # SB_FULL=1: every CPU/CIA flip-flop loaded (A/B)\n')
b=b.replace('  -e proc_core >', '  ${SB_FULL:+-gg_sb_full=true} -e proc_core >')
b=b.replace('verilator --cc --exe --build', 'verilator ${SB_FULL:+-DSB_FULL} --cc --exe --build')
assert 'SB_FULL:+-DSB_FULL' in b and 'gg_sb_full' in b
# the statebus bench writes the ROMs through the app's window; this one preloads them
a='# the stock ROMs from roms/ (sim only): the bench writes them through the\n# app\'s ROM window before it lets the C64 out of reset, as the app does\n'
assert a in b
b=b.replace(a,'# the stock ROMs from roms/, preloaded into c64_mem\'s ROM block (sim only,\n# TRX_SIM_ROMS; on the board the app writes them)\n')
b=b.replace('verilator ${SB_FULL:+-DSB_FULL} --cc', 'verilator ${SB_FULL:+-DSB_FULL} -DTRX_SIM_ROMS --cc')
assert '-DTRX_SIM_ROMS' in b
open('ring/build.sh','w').write(b)
