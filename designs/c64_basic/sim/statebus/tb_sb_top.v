// SPDX-License-Identifier: GPL-3.0-or-later
// Top of the state bus / freeze bench (tb_statebus.cpp): c64_core
// with its gate (a BUFGCE model as in sim/drive/tb_drive_top.v), and the
// C64-clock half of top.v's IO block and DMA window as top.v wires them:
// dma_win (the app's DMA window and the lens unit) and trx_sys
// (gate/capture control, state bus, TX_VIEW). The app's side of the
// window is ultimate_sys's toggle handshake; trx_sys is reached through its
// JTAG word port and its byte handshake (what trx_io.v forwards). The C64
// text layer (c64_text.v) as top.v wires it: the second RAM and colour RAM
// ports in the clocks the window and JTAG leave free, its registers on clk,
// its video side on pclk with a 2688 x 1125 counter and a flat picture below.
module tb_sb_top (
    input  wire        clk,
    input  wire        clk2x,               // 2 x clk, edge-aligned (the SID engine, T29's CPU and RAM port)
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
    wire        lk_go;
    wire [18:0] lk_req;
    wire [31:0] lk_res;
    wire  [1:0] cyc_force;
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

    dma_win win (
        .clk(clk),
        .dma_req_tgl(dma_req_tgl), .dma_addr(dma_addr), .dma_we(dma_we), .dma_wdata(dma_wdata),
        .dma_bus(dma_bus), .dma_rom(dma_rom), .dma_done_tgl(dma_done_tgl), .dma_rdata(dma_rdata),
        .dbg_busy(dbg_re || dbg_we), .port_use(port_use), .port_addr(port_addr),
        .port_rom(port_rom), .port_we(port_we), .port_wdata(port_wdata), .port_rdata(ram_rdata),
        .cyc_req(cyc_req), .cyc_addr(cyc_addr), .cyc_we(cyc_we), .cyc_wdata(cyc_wdata),
        .cyc_done(cyc_done), .cyc_rdata(cyc_rdata), .cyc_force(cyc_force),
        .cram_addr(win_cram_addr), .cram_we(cram_we), .cram_wdata(cram_wdata), .cram_rdata(cram_rdata), .cram_use(win_cram_use),
        .halted(halted), .app_ultimax(app_ultimax), .core_ultimax(core_ultimax),
        .pk_addr(pk_addr), .pk_cls(pk_cls), .pk_hit(pk_hit), .pk_mem(pk_mem), .pk_maddr(pk_maddr),
        .pk_data(pk_data), .cart_ext(1'b0),
        .lk_go(lk_go), .lk_req(lk_req), .lk_res(lk_res),
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
        .lk_go(lk_go), .lk_req(lk_req), .lk_res(lk_res),
        .sb_addr(sb_addr), .sb_rdata(sb_rdata),
        .sb_frozen(sb_frozen), .halted(halted), .cap_gen(cap_gen), .cap_nosnap(cap_nosnap),
        .tx_view(tx_view)
    );

    // as top.v: the window, JTAG, then the text layer
    wire        tx_go   = tx_req && !port_use && !(dbg_re || dbg_we);
    wire [15:0] p_addr  = port_use ? port_addr : tx_go ? tx_paddr : dbg_addr;
    wire        p_we    = port_use ? port_we : dbg_we;
    wire  [7:0] p_wdata = port_use ? port_wdata : dbg_wdata;
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
        .cart_type(8'd0), .cart_desc(344'd0), .cart_kill_tgl(1'b0), .cart_force_tgl(1'b0), .cart_active(),
        .cart_wclk(clk), .cart_req_tgl(), .cart_req_we(), .cart_req_addr(), .cart_req_data(),
        .cart_req_slot(), .cart_req_cached(), .cart_ack_tgl(1'b0),
        .cart_fl_enable(1'b0), .cart_fl_magic1(16'd0), .cart_fl_magic2(16'd0), .cart_fl_mask(16'd0),
        .cart_fl_id(24'd0), .cart_evt_tgl(), .cart_evt_word(), .cart_fl_ack_tgl(1'b0), .cart_inval_tgl(1'b0),
        .cart_ee_tgl(),
        .cart_ee_clk(clk), .cart_ee_addr(11'd0), .cart_ee_we(1'b0), .cart_ee_wdata(8'd0), .cart_ee_rdata(),
        .cart_fill_be(8'd0), .cart_fill_addr(9'd0), .cart_fill_data(64'd0),
        .dma_req(cyc_req), .dma_addr(cyc_addr), .dma_we(cyc_we), .dma_wdata(cyc_wdata),
        .dma_done(cyc_done), .dma_rdata(cyc_rdata), .ultimax(core_ultimax),
        .dma_force(cyc_force),
        .iec_atn_o(), .iec_clk_o(), .iec_data_o(),
        .iec_atn_i(1'b1), .iec_clk_i(1'b1), .iec_data_i(1'b1),
        .sb_addr(sb_addr), .sb_rdata(sb_rdata), .sb_frozen(sb_frozen),
        .cap_every(cap_every), .cap_gen(cap_gen), .cap_tgl(cap_tgl), .cap_nosnap(cap_nosnap),
        .cram_addr(cram_addr), .cram_we(cram_we), .cram_wdata(cram_wdata), .cram_rdata(cram_rdata),
        .pk_addr(pk_addr), .pk_cls(pk_cls), .pk_hit(pk_hit), .pk_mem(pk_mem), .pk_maddr(pk_maddr),
        .pk_data(pk_data)
    );
    assign dbg_rdata = ram_rdata;
endmodule
