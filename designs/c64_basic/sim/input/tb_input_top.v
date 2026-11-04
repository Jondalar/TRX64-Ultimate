// SPDX-License-Identifier: GPL-3.0-or-later
// T38 on the real machine: c64_core (6510, CIA 1, KERNAL) with app_input.v
// as top.v puts them together, for tb_input_core.cpp. clk = C64 (31.5 MHz);
// the app's state comes in as top.v's ultimate_sys ports would bring it
// (any clock: app_input synchronizes), the physical keyboard and joysticks
// as already synchronized levels.
module tb_input_top (
    input  wire        clk,
    input  wire        clk2x,               // 2 x clk, edge-aligned: the SID engine (T25.1)
    input  wire        rst,
    input  wire [15:0] dbg_addr,            // C64 RAM debug port (clk)
    input  wire        dbg_we,
    input  wire [7:0]  dbg_wdata,
    output wire [7:0]  dbg_rdata,
    input  wire        ovl_own,
    input  wire [63:0] phys_keys,
    input  wire  [4:0] phys_joy1, phys_joy2,
    input  wire        phys_restore,
    input  wire [63:0] app_keys,
    input  wire  [4:0] app_joy1, app_joy2,
    input  wire        app_restore, app_reset, app_freeze,
    output wire        c64_reset,           // what top.v ORs into c64_reset_req
    output wire [9:0]  pix_x,
    output wire [8:0]  pix_y
);

    wire [3:0] pix_color; wire [4:0] leds;
    wire [23:0] reu_cmd_addr; wire reu_cmd_we, reu_cmd_valid; wire [15:0] reu_wdata; wire [1:0] reu_wdata_we;
    wire signed [23:0] audio_l, audio_r;
    wire gate_ce, gclk;
    reg  gate_l = 1'b1;
    always @(negedge clk) gate_l <= gate_ce;
    assign gclk = clk & gate_l;

    wire [63:0] keys;
    wire  [4:0] joy1, joy2;
    wire        restore, freeze;
    app_input ain (
        .clk(clk), .ovl_own(ovl_own), .phys_keys(phys_keys),
        .phys_joy1(phys_joy1), .phys_joy2(phys_joy2), .phys_restore(phys_restore),
        .app_keys(app_keys), .app_joy1(app_joy1), .app_joy2(app_joy2),
        .app_restore(app_restore), .app_reset(app_reset), .app_freeze(app_freeze),
        .keys(keys), .joy1(joy1), .joy2(joy2), .restore(restore),
        .reset(c64_reset), .freeze(freeze)
    );

    c64_core c64 (
        .dbg_req(1'b1),            // T29: the outside port is wanted in every clock here
        .vmode(2'd0),                   // T40: PAL
        .clk(clk), .gclk(gclk), .gate_ce(gate_ce), .rst(rst), .halt_req(1'b0), .halted(),
        .snd_clk(clk), .snd_rst(rst), .audio_sample_en(1'b0),
        .clk2x(clk2x),
        .audio_l(audio_l), .audio_r(audio_r),
        .pix_color(pix_color), .pix_x(pix_x), .pix_y(pix_y), .leds(leds),
        .keys(keys), .joy1(joy1), .joy2(joy2), .restore(restore), .freeze_btn(freeze),
        .dbg_addr(dbg_addr), .dbg_we(dbg_we), .dbg_wdata(dbg_wdata), .dbg_rdata(dbg_rdata),
        .reu_cmd_addr(reu_cmd_addr), .reu_cmd_we(reu_cmd_we), .reu_cmd_valid(reu_cmd_valid),
        .reu_cmd_ready(1'b1), .reu_en(1'b1), .reu_size(3'd5), .reu_wdata(reu_wdata), .reu_wdata_we(reu_wdata_we),
        .reu_rdata(16'd0), .reu_rdata_valid(1'b0),
        .cpu_reset_req(c64_reset), .cpu_pause_req(1'b0),
        .cart_type(8'd0), .cart_desc(344'd0), .cart_kill_tgl(1'b0), .cart_force_tgl(1'b0), .cart_active(),
        .cart_wclk(clk), .cart_req_tgl(), .cart_req_we(), .cart_req_addr(), .cart_req_data(),
        .cart_req_slot(), .cart_req_cached(), .cart_ack_tgl(1'b0),
        .cart_fl_enable(1'b0), .cart_fl_magic1(16'd0), .cart_fl_magic2(16'd0), .cart_fl_mask(16'd0),
        .cart_fl_id(24'd0), .cart_evt_tgl(), .cart_evt_word(), .cart_fl_ack_tgl(1'b0), .cart_inval_tgl(1'b0),
        .cart_ee_tgl(),
        .cart_ee_clk(clk), .cart_ee_addr(11'd0), .cart_ee_we(1'b0), .cart_ee_wdata(8'd0), .cart_ee_rdata(),
        .cart_fill_be(8'd0), .cart_fill_addr(9'd0), .cart_fill_data(64'd0),
        .dma_req(1'b0), .dma_addr(16'd0), .dma_we(1'b0), .dma_wdata(8'd0),
        .dma_done(), .dma_rdata(), .ultimax(1'b0),
        .iec_atn_o(), .iec_clk_o(), .iec_data_o(),
        .iec_atn_i(1'b1), .iec_clk_i(1'b1), .iec_data_i(1'b1),
        .sb_addr(12'd0), .sb_rdata(), .sb_frozen(), .dbg_rom(1'b0),
        .cap_every(8'd0), .cap_gen(), .cap_tgl(), .cap_nosnap(),
        .log_on(1'b0), .cap_block(1'b0),
        .sb_we(1'b0), .sb_waddr(12'd0), .sb_wdata(32'd0), .sb_ldreq(1'b0), .sb_ldmask(5'd0),
        .pos_set(1'b0), .pos_arm_in(1'b0), .pos_cyc(6'd0), .pos_release(1'b0),
        .resume_phase(20'd0), .rp_set(1'b0),
        .cram_addr(10'd0), .cram_we(1'b0), .cram_wdata(4'd0), .cram_rdata(),
        .bus_addr(), .io_ev(), .io_ev_addr(), .io_ev_data(),
        .io_ev_we(), .io_ev_io(), .io_oe(1'b0), .io_dout(8'd0), .io_irq(1'b0)
    );

endmodule
