// SPDX-License-Identifier: GPL-3.0-or-later
// Top of the frame ring bench (tb_ring.cpp, T20 §13.1). Made by gen_top.py
// from ../statebus/tb_sb_top.v and tb_ring_eng.vh: change those, not this.
// tb_sb_top's machine (c64_core with its gate, dma_win, trx_sys, the text
// layer) plus the engine (trx_engine.v), cart_fill, mig_arb and the MIG UI
// model, as top.v wires them. ui_clk is the MIG's 75 MHz; the model's memory
// is 64 MB (addresses wrap: 0x5C00000 lands at 0x1C00000).
module tb_ring_top (
    input  wire        clk,
    input  wire        clk2x,               // 2 x clk, edge-aligned (the SID engine, T29's CPU and RAM port)
    input  wire        ui_clk,
    input  wire        rst,
    // C64 RAM debug port (JTAG's), clk
    input  wire        dbg_re,
    input  wire [15:0] dbg_addr,
    input  wire        dbg_we,
    input  wire  [7:0] dbg_wdata,
    output wire  [7:0] dbg_rdata,
    // machine control as top.v has it
    input  wire        jtag_halt,           // register 9 bit 3
    input  wire        jtag_cpu_only,       // register 9 bit 4
    input  wire        app_stop,            // C64_STOP
    input  wire        app_ultimax,         // C64_MODE b1
    input  wire        app_reset,           // C64_MODE reset
    output wire        halted,
    output reg  [31:0] gclk_edges = 0,
    // the app's DMA window (ultimate_sys's handshake)
    input  wire        dma_req_tgl,
    input  wire [15:0] dma_addr,
    input  wire        dma_we,
    input  wire  [7:0] dma_wdata,
    input  wire        dma_bus,
    input  wire        dma_rom,             // the app's ROM apertures (dma_win.v)
    output wire        dma_done_tgl,
    output wire  [7:0] dma_rdata,
    // trx_sys: JTAG words and the app's bytes
    input  wire        j_re,
    input  wire        j_we,
    input  wire [11:0] j_addr,
    input  wire [31:0] j_wdata,
    output wire [31:0] j_rdata,
    output wire        j_rvalid,
    input  wire        rq_tgl,
    input  wire [15:0] rq_addr,
    input  wire        rq_we,
    input  wire  [7:0] rq_wdata,
    output wire        rs_tgl,
    output wire  [7:0] rs_rdata,
    // keyboard and joysticks (1 = pressed)
    input  wire [63:0] keys,
    input  wire  [4:0] joy1,
    input  wire  [4:0] joy2,
    // to look at
    output wire  [9:0] pix_x,
    output wire  [8:0] pix_y,
    output wire  [3:0] pix_color,
    output wire        cap_tgl,
    output wire        eng_tgl,
    output wire        gap_tgl,
    // the cartridge (cart_fill on mig_arb's cart client) and the write taps
    input  wire  [7:0] cart_type,
    output reg  [31:0] n_s0 = 0,            // stream 0 / 1 writes made so far (log on)
    output reg  [31:0] n_s1 = 0,
    output reg  [31:0] n_s0_mk = 0,         // ... at the last marker's boundary
    output reg  [31:0] n_s1_mk = 0,
    output reg  [31:0] n_mk = 0,
    // the text layer: registers (page 6 offsets, clk), video (pclk)
    input  wire        tx_we,
    input  wire        tx_re,
    input  wire [11:0] tx_addr,
    input  wire  [7:0] tx_wdata,
    output wire  [7:0] tx_rdata,
    input  wire        pclk,
    output wire [23:0] o_rgb,
    output wire        o_de,
    output wire [11:0] o_cx,
    output wire [10:0] o_cy,
    output reg  [31:0] tx_port_clocks = 0   // clocks the text layer had the RAM port
);
    `include "desc_app.vh"             // cart_desc(CART_TYPE): the app's descriptors (T33)
    wire gate_ce, gclk;
    reg  gate_l = 1'b1;
    always @(negedge clk) gate_l <= gate_ce;
    assign gclk = clk & gate_l;
    always @(posedge gclk) gclk_edges <= gclk_edges + 1;

    wire        gate_frz;
    wire        app_pause = gate_frz && !jtag_cpu_only;
    wire        halt_req = jtag_halt | (app_stop & app_pause);
    wire        core_halt = halt_req;

    wire        port_use, port_rom, port_we;
    wire [15:0] port_addr;
    wire  [7:0] port_wdata, ram_rdata;
    wire        cyc_req, cyc_we, cyc_done;
    wire [15:0] cyc_addr;
    wire  [7:0] cyc_wdata, cyc_rdata;
    wire        core_reset_req, core_ultimax;
    wire [11:0] sb_addr;
    wire [31:0] sb_rdata;
    wire        sb_frozen;
    wire  [9:0] cram_addr, win_cram_addr;
    wire        cram_we, win_cram_use;
    wire  [3:0] cram_wdata, cram_rdata;
    wire [15:0] pk_addr;
    wire  [1:0] pk_cls;
    wire        pk_hit, pk_mem;
    wire [23:0] pk_maddr;
    wire  [7:0] pk_data;
    wire        pkd_tgl;
    wire [23:0] pkd_addr;
    wire  [9:0] tx_cram_addr;
    wire [15:0] tx_view;
    wire        tx_req, tx_rom;
    wire [15:0] tx_paddr;
    wire  [7:0] cap_every;
    wire [15:0] cap_gen, cap_nosnap;
    wire        log_on, seq_busy, mk, mk_snap, mk_cap, mk_skip, mk_intg, reu_busy, phi2_tick;
    wire        tr_ib;                  // the trace former's feed (T21 §4.11.1)
    wire [15:0] tr_pc;
    wire  [7:0] tr_op, tr_b1, tr_b2;
    wire [39:0] tr_regs;
    wire  [9:0] mk_x;
    wire  [8:0] mk_line;
    wire        p2_we, p2_re, seq_sb_req, seq_sb_gnt;
    wire  [9:0] p2_addr;
    wire [31:0] p2_wdata, p2_rdata;
    wire [11:0] seq_sb_addr;
    wire        wa_we, wc_we, wk_we;
    wire [15:0] wa_addr;
    wire  [7:0] wa_new, wa_old, wk_new;
    wire  [9:0] wc_addr;
    wire  [3:0] wc_new, wc_old;
    wire [14:0] wk_addr;
    wire        cart_req_tgl, cart_req_we, cart_req_cached, cart_ack_tgl;
    wire [23:0] cart_req_addr;
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

    dma_win win (
        .clk(clk),
        .dma_req_tgl(dma_req_tgl), .dma_addr(dma_addr), .dma_we(dma_we), .dma_wdata(dma_wdata),
        .dma_bus(dma_bus), .dma_rom(dma_rom), .dma_done_tgl(dma_done_tgl), .dma_rdata(dma_rdata),
        .dbg_busy(dbg_re || dbg_we), .port_use(port_use), .port_addr(port_addr),
        .port_rom(port_rom), .port_we(port_we), .port_wdata(port_wdata), .port_rdata(ram_rdata),
        .cyc_req(cyc_req), .cyc_addr(cyc_addr), .cyc_we(cyc_we), .cyc_wdata(cyc_wdata),
        .cyc_done(cyc_done), .cyc_rdata(cyc_rdata),
        .cram_addr(win_cram_addr), .cram_we(cram_we), .cram_wdata(cram_wdata), .cram_rdata(cram_rdata), .cram_use(win_cram_use),
        .halted(halted), .app_ultimax(app_ultimax), .core_ultimax(core_ultimax),
        .pk_addr(pk_addr), .pk_cls(pk_cls), .pk_hit(pk_hit), .pk_mem(pk_mem), .pk_maddr(pk_maddr),
        .pk_data(pk_data), .cart_ext(1'b0),
        .pkd_tgl(pkd_tgl), .pkd_addr(pkd_addr), .pkd_ack_tgl(pkd_tgl), .pkd_data(8'hEE)
    );
    assign core_reset_req = app_reset;

    trx_sys tsys (
        .clk(clk),
        .rq_tgl(rq_tgl), .rq_addr(rq_addr), .rq_we(rq_we), .rq_wdata(rq_wdata),
        .rs_tgl(rs_tgl), .rs_rdata(rs_rdata),
        .j_re(j_re), .j_we(j_we), .j_addr(j_addr), .j_wdata(j_wdata), .j_rdata(j_rdata),
        .j_rvalid(j_rvalid),
        .gate_frz(gate_frz), .cap_every(cap_every),
        .p2_we(p2_we), .p2_re(p2_re), .p2_addr(p2_addr), .p2_wdata(p2_wdata), .p2_rdata(p2_rdata),
        .sb_we(sb_we), .sb_waddr(sb_waddr), .sb_wdata(sb_wdata), .sb_ldreq(sb_ldreq),
        .sb_ldmask(sb_ldmask), .sb_ldarm(sb_ldarm), .pos_set(pos_set), .pos_arm_in(pos_arm_in),
        .pos_cyc(pos_cyc), .pos_release(pos_release), .pos_arm(pos_arm), .pos_hold(pos_hold),
        .resume_phase(resume_phase), .rp_set(rp_set), .rp_pend(rp_pend),
        .seq_sb_req(seq_sb_req),
        .seq_sb_addr(seq_sb_addr), .seq_sb_gnt(seq_sb_gnt), .sb_addr(sb_addr), .sb_rdata(sb_rdata),
        .sb_frozen(sb_frozen), .halted(halted), .cap_gen(cap_gen), .cap_nosnap(cap_nosnap),
        .tx_view(tx_view)
    );

    // as top.v: the responder, JTAG, then the text layer
    wire        tx_go   = tx_req && !port_use && !(dbg_re || dbg_we);
    // the engine's APPLY last: a byte in a clock none of them uses (top.v)
    wire        ap_req, ap_gnt;
    wire [15:0] ap_addr;
    wire  [7:0] ap_wdata;
    assign      ap_gnt  = ap_req && !port_use && !tx_go && !(dbg_re || dbg_we);
    wire [15:0] p_addr  = port_use ? port_addr : tx_go ? tx_paddr : ap_gnt ? ap_addr : dbg_addr;
    wire        p_we    = port_use ? port_we : ap_gnt || dbg_we;
    wire  [7:0] p_wdata = port_use ? port_wdata : ap_gnt ? ap_wdata : dbg_wdata;
    wire        p_rom   = port_use ? port_rom : tx_go && tx_rom;
    assign cram_addr = win_cram_use ? win_cram_addr : tx_cram_addr;
    always @(posedge clk) tx_port_clocks <= tx_port_clocks + tx_go;

    reg  [11:0] vcx = 12'd0;
    reg  [10:0] vcy = 11'd0;
    always @(posedge pclk) begin
        vcx <= (vcx == 12'd2687) ? 12'd0 : vcx + 12'd1;
        if (vcx == 12'd2687) vcy <= (vcy == 11'd1124) ? 11'd0 : vcy + 11'd1;
    end
    wire vde = vcx >= 12'd768 && vcy >= 11'd45;
    c64_text txt (
        .clk_sys(clk), .we(tx_we), .re(tx_re), .addr(tx_addr), .wdata(tx_wdata), .rdata(tx_rdata),
        .pal_clk(clk), .pal_we(1'b0), .pal_addr(6'd0), .pal_wdata(8'd0),
        .cclk(clk), .halted(halted), .vic_view(tx_view),
        .port_req(tx_req), .port_addr(tx_paddr), .port_rom(tx_rom), .port_grant(tx_go),
        .port_rdata(ram_rdata), .cram_addr(tx_cram_addr), .cram_grant(!win_cram_use),
        .cram_rdata(cram_rdata),
        .clk(pclk), .rgb_in(24'h123456), .de_in(vde), .hs_in(1'b0), .vs_in(1'b0),
        .cx_in(vcx), .cy_in(vcy),
        .rgb(o_rgb), .de(o_de), .hs(), .vs(), .cx(o_cx), .cy(o_cy)
    );

    wire [4:0] leds;
    wire [23:0] reu_cmd_addr; wire reu_cmd_we, reu_cmd_valid; wire [15:0] reu_wdata; wire [1:0] reu_wdata_we;
    wire signed [23:0] audio_l, audio_r;
    c64_core c64 (
        .dbg_req(1'b1), .clk2x(clk2x),            // T29: the outside port is wanted in every clock here
        .vmode(2'd0),                   // T40: PAL
        .clk(clk), .gclk(gclk), .gate_ce(gate_ce), .rst(rst), .halt_req(core_halt), .halted(halted),
        .snd_clk(clk), .snd_rst(rst), .audio_sample_en(1'b0),
        .audio_l(audio_l), .audio_r(audio_r),
        .pix_color(pix_color), .pix_x(pix_x), .pix_y(pix_y), .leds(leds),
        .keys(keys), .joy1(joy1), .joy2(joy2), .restore(1'b0),
        .dbg_addr(p_addr), .dbg_rom(p_rom), .dbg_we(p_we), .dbg_wdata(p_wdata),
        .dbg_rdata(ram_rdata),
        .reu_cmd_addr(reu_cmd_addr), .reu_cmd_we(reu_cmd_we), .reu_cmd_valid(reu_cmd_valid),
        .reu_cmd_ready(1'b1), .reu_en(1'b1), .reu_size(3'd5), .reu_wdata(reu_wdata), .reu_wdata_we(reu_wdata_we),
        .reu_rdata(16'd0), .reu_rdata_valid(1'b0),
        .cpu_reset_req(core_reset_req), .cpu_pause_req(app_stop && !app_pause),
        .cart_type(cart_type), .cart_desc(cart_desc(cart_type)), .cart_kill_tgl(1'b0), .cart_force_tgl(1'b0), .cart_active(),
        .cart_wclk(ui_clk), .cart_req_tgl(cart_req_tgl), .cart_req_we(cart_req_we),
        .cart_req_addr(cart_req_addr), .cart_req_data(cart_req_data),
        .cart_req_slot(cart_req_slot), .cart_req_cached(cart_req_cached), .cart_ack_tgl(cart_ack_tgl),
        .cart_fl_enable(1'b0), .cart_fl_magic1(16'd0), .cart_fl_magic2(16'd0), .cart_fl_mask(16'd0),
        .cart_fl_id(24'd0), .cart_evt_tgl(), .cart_evt_word(), .cart_fl_ack_tgl(1'b0), .cart_inval_tgl(1'b0),
        .cart_ee_tgl(),
        .cart_ee_clk(clk), .cart_ee_addr(11'd0), .cart_ee_we(1'b0), .cart_ee_wdata(8'd0), .cart_ee_rdata(),
        .cart_fill_be(cart_fill_be), .cart_fill_addr(cart_fill_addr), .cart_fill_data(cart_fill_data),
        .wa_we(wa_we), .wa_addr(wa_addr), .wa_new(wa_new), .wa_old(wa_old),
        .wc_we(wc_we), .wc_addr(wc_addr), .wc_new(wc_new), .wc_old(wc_old),
        .wk_we(wk_we), .wk_addr(wk_addr), .wk_new(wk_new),
        .sb_we(sb_we), .sb_waddr(sb_waddr), .sb_wdata(sb_wdata), .sb_ldreq(sb_ldreq),
        .sb_ldmask(sb_ldmask), .sb_ldarm(sb_ldarm), .pos_set(pos_set), .pos_arm_in(pos_arm_in),
        .pos_cyc(pos_cyc), .pos_release(pos_release), .pos_arm(pos_arm), .pos_hold(pos_hold),
        .resume_phase(resume_phase), .rp_set(rp_set), .rp_pend(rp_pend),
        .dma_req(cyc_req), .dma_addr(cyc_addr), .dma_we(cyc_we), .dma_wdata(cyc_wdata),
        .dma_done(cyc_done), .dma_rdata(cyc_rdata), .ultimax(core_ultimax),
        .iec_atn_o(), .iec_clk_o(), .iec_data_o(),
        .iec_atn_i(1'b1), .iec_clk_i(1'b1), .iec_data_i(1'b1),
        .sb_addr(sb_addr), .sb_rdata(sb_rdata), .sb_frozen(sb_frozen),
        .cap_every(cap_every), .cap_gen(cap_gen), .cap_tgl(cap_tgl), .cap_nosnap(cap_nosnap),
        .log_on(log_on), .cap_block(seq_busy), .mk(mk), .mk_snap(mk_snap), .mk_cap(mk_cap),
        .mk_skip(mk_skip), .mk_intg(mk_intg), .reu_busy(reu_busy), .mk_x(mk_x), .mk_line(mk_line),
        .phi2_tick(phi2_tick),
        .tr_ib(tr_ib), .tr_pc(tr_pc), .tr_op(tr_op), .tr_b1(tr_b1), .tr_b2(tr_b2), .tr_regs(tr_regs),
        .cram_addr(cram_addr), .cram_we(cram_we), .cram_wdata(cram_wdata), .cram_rdata(cram_rdata),
        .pk_addr(pk_addr), .pk_cls(pk_cls), .pk_hit(pk_hit), .pk_mem(pk_mem), .pk_maddr(pk_maddr),
        .pk_data(pk_data)
    );
    assign dbg_rdata = ram_rdata;

    // ---- the engine, cart_fill, mig_arb (CPU/REU/drive/video idle), the MIG model ----
    wire         ui_rst, calib;
    wire [26:0]  app_addr, e_addr, c_addr;
    wire [2:0]   app_cmd, e_cmd, c_cmd;
    wire         app_en, app_rdy, app_wdf_wren, app_wdf_rdy, app_rd_data_valid;
    wire [127:0] app_wdf_data, app_rd_data, e_wdf_data, c_wdf_data, c_rd_data;
    wire [15:0]  app_wdf_mask, e_wdf_mask, c_wdf_mask;
    wire         e_en, e_rdy, e_wdf_wren, e_rd_valid;
    wire         c_en, c_rdy, c_wdf_wren, c_wdf_rdy, c_rd_valid;
    wire         cart_rbw, ob_v;
    wire [14:0]  ob_addr;
    wire  [7:0]  ob_old;
    trx_engine eng (
        .clk(clk),
        .r_we(p2_we), .r_re(p2_re), .r_addr(p2_addr), .r_wdata(p2_wdata), .r_rdata(p2_rdata),
        .log_on(log_on), .seq_busy(seq_busy), .mk(mk), .mk_snap(mk_snap), .mk_cap(mk_cap),
        .mk_skip(mk_skip), .mk_intg(mk_intg), .reu_busy(reu_busy), .mk_x(mk_x), .mk_line(mk_line),
        .phi2_tick(phi2_tick), .cap_gen(cap_gen),
        .tr_ib(tr_ib), .tr_pc(tr_pc), .tr_op(tr_op), .tr_b1(tr_b1), .tr_b2(tr_b2), .tr_regs(tr_regs),
        .sb_req(seq_sb_req), .sb_addr(seq_sb_addr), .sb_gnt(seq_sb_gnt), .sb_rdata(sb_rdata),
        .wa_we(wa_we), .wa_addr(wa_addr), .wa_new(wa_new), .wa_old(wa_old),
        .wb_we(p_we && !p_rom && !ap_gnt), .wb_addr(p_addr), .wb_new(p_wdata), .wb_old(ram_rdata),
        .wc_we(wc_we), .wc_addr(wc_addr), .wc_new(wc_new), .wc_old(wc_old),
        .wd_we(cram_we), .wd_addr(cram_addr), .wd_new(cram_wdata), .wd_old(cram_rdata),
        .wk_we(wk_we), .wk_addr(wk_addr), .wk_new(wk_new), .cart_rbw(cart_rbw),
        .ap_req(ap_req), .ap_addr(ap_addr), .ap_wdata(ap_wdata), .ap_gnt(ap_gnt),
        .eng_tgl(eng_tgl), .gap_tgl(gap_tgl),
        .ui_clk(ui_clk), .ui_rst(ui_rst || !calib),
        .e_addr(e_addr), .e_cmd(e_cmd), .e_en(e_en), .e_rdy(e_rdy), .e_wdf_data(e_wdf_data),
        .e_wdf_mask(e_wdf_mask), .e_wdf_wren(e_wdf_wren), .e_rd_data(c_rd_data),
        .e_rd_data_valid(e_rd_valid), .ob_v(ob_v), .ob_addr(ob_addr), .ob_old(ob_old)
    );
    cart_fill cfill (
        .ui_clk(ui_clk), .ui_rst(ui_rst || !calib), .rom_sel(2'd0),   // Gideon's base: the bench holds the image at 0x3C00000
        .req_tgl(cart_req_tgl), .req_we(cart_req_we), .req_addr(cart_req_addr),
        .req_data(cart_req_data), .req_slot(cart_req_slot), .req_cached(cart_req_cached),
        .ack_tgl(cart_ack_tgl),
        .c_be(cart_fill_be), .c_addr(cart_fill_addr), .c_data(cart_fill_data),
        .app_addr(c_addr), .app_cmd(c_cmd), .app_en(c_en), .app_rdy(c_rdy),
        .app_wdf_data(c_wdf_data), .app_wdf_mask(c_wdf_mask), .app_wdf_wren(c_wdf_wren),
        .app_wdf_rdy(c_wdf_rdy), .app_rd_data(c_rd_data), .app_rd_data_valid(c_rd_valid),
        .rbw(cart_rbw), .ob_v(ob_v), .ob_addr(ob_addr), .ob_old(ob_old),
        .pk_tgl(1'b0), .pk_addr(23'd0), .pk_ack_tgl(), .pk_data()
    );
    wire [2:0]  done_tgl;
    wire [63:0] rdata;
    wire [127:0] rdata_line;
    mig_arb arb (.ui_clk(ui_clk), .ui_rst(ui_rst), .calib_done(calib),
        .req_tgl(3'b000), .addr(78'd0), .we(3'b000), .wdata(96'd0), .be(12'd0),
        .done_tgl(done_tgl), .rdata(rdata), .rdata_line(rdata_line),
        .c_addr(c_addr), .c_cmd(c_cmd), .c_en(c_en), .c_rdy(c_rdy), .c_wdf_data(c_wdf_data),
        .c_wdf_mask(c_wdf_mask), .c_wdf_wren(c_wdf_wren), .c_wdf_rdy(c_wdf_rdy),
        .c_rd_data(c_rd_data), .c_rd_data_valid(c_rd_valid),
        .v_addr(27'd0), .v_cmd(3'd0), .v_en(1'b0), .v_rdy(), .v_wdf_data(128'd0), .v_wdf_mask(16'd0),
        .v_wdf_wren(1'b0), .v_wdf_rdy(), .v_rd_data_valid(), .v_urgent(1'b0),
        .e_addr(e_addr), .e_cmd(e_cmd), .e_en(e_en), .e_rdy(e_rdy), .e_wdf_data(e_wdf_data),
        .e_wdf_mask(e_wdf_mask), .e_wdf_wren(e_wdf_wren), .e_rd_data_valid(e_rd_valid),
        .app_addr(app_addr), .app_cmd(app_cmd), .app_en(app_en), .app_rdy(app_rdy),
        .app_wdf_data(app_wdf_data), .app_wdf_mask(app_wdf_mask), .app_wdf_wren(app_wdf_wren),
        .app_wdf_rdy(app_wdf_rdy), .app_rd_data(app_rd_data), .app_rd_data_valid(app_rd_data_valid));
    mig_ui_model #(.MEM_WORDS(1 << 22)) mig (.ui_clk(ui_clk), .ui_rst(ui_rst), .calib_done(calib),
        .app_addr(app_addr), .app_cmd(app_cmd), .app_en(app_en), .app_rdy(app_rdy),
        .app_wdf_data(app_wdf_data), .app_wdf_mask(app_wdf_mask), .app_wdf_wren(app_wdf_wren),
        .app_wdf_rdy(app_wdf_rdy), .app_rd_data(app_rd_data), .app_rd_data_valid(app_rd_data_valid));

    // the bench's own count of the taps' writes, and of them at each marker's
    // boundary (a write at the marker's clock is the next frame's)
    wire on_ = eng.on;
    wire [2:0] te = eng.tap_en;
    wire [1:0] w0 = (wa_we && on_) + (p_we && !p_rom && on_ && te[0]);
    wire [1:0] w1 = (wc_we && on_ && te[2]) + (cram_we && on_ && te[0] && te[2]) + (wk_we && on_ && te[1]);
    always @(posedge clk) begin
        n_s0 <= n_s0 + w0;
        n_s1 <= n_s1 + w1;
        if (mk && on_) begin
            n_s0_mk <= n_s0;
            n_s1_mk <= n_s1;
            n_mk    <= n_mk + 1;
        end
    end
endmodule
