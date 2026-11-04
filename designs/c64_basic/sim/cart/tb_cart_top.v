// SPDX-License-Identifier: GPL-3.0-or-later
// Top of the cartridge simulation (tb_cart.cpp): c64_core with its cartridge, and the DDR2
// side as top.v has it (cart_fill and the RISC-V's port on mig_arb). The MIG app port
// is modelled in C++; mig_arb's client 0 (the RISC-V's port) is driven from
// C++ too, so its accesses go on while cache lines are fetched. The flash
// responder's configuration, its pending operation, ACK and cache invalidation
// are trx_io's in top.v; here C++ plays them (and the app's handler).
module tb_cart_top (
    input  wire        clk,
    input  wire        clk2x,               // 2 x clk, edge-aligned (the SID engine, T29's CPU and RAM port)
    input  wire        ui_clk,
    input  wire        rst,
    input  wire        ui_rst,
    input  wire        cpu_reset_req,
    input  wire  [7:0] cart_type,
    input  wire        cart_kill_tgl,
    input  wire        cart_force_tgl,
    input  wire [63:0] keys,                // the keyboard matrix, col * 8 + row (--key)
    output wire        cart_active,
    input  wire [15:0] dbg_addr,
    output wire  [7:0] dbg_rdata,
    output wire  [3:0] pix_color,
    output wire  [9:0] pix_x,
    output wire  [8:0] pix_y,

    input  wire        fl_enable,
    input  wire [15:0] fl_magic1,
    input  wire [15:0] fl_magic2,
    input  wire [15:0] fl_mask,
    input  wire [23:0] fl_id,
    input  wire [343:0] cart_desc,
    output wire        evt_tgl,
    output wire [33:0] evt_word,
    input  wire  [1:0] rom_sel,             // trx_io ROM_SEL: 0 Gideon's 0x3C00000, 1 the expansion-port block (8 MB, T51)
    input  wire        fl_ack_tgl,
    input  wire        inval_tgl,

    // mig_arb client 0 (toggle handshake)
    input  wire        a0_tgl,
    input  wire [25:0] a0_addr,
    input  wire        a0_we,
    input  wire [31:0] a0_wdata,
    output wire        a0_done_tgl,
    output wire [31:0] a0_rdata,

    output wire [26:0] app_addr,
    output wire  [2:0] app_cmd,
    output wire        app_en,
    input  wire        app_rdy,
    output wire [127:0] app_wdf_data,
    output wire [15:0] app_wdf_mask,
    output wire        app_wdf_wren,
    input  wire        app_wdf_rdy,
    input  wire [127:0] app_rd_data,
    input  wire        app_rd_data_valid,

    output wire        wait_cpu,
    output wire        fill_req,
    output wire        fill_done,

    // the lens unit (dma_win.v LK, T21 §3.4.1), driven from C++ like
    // trx_sys would: the machine paused (a BUFGCE model as in sim/statebus),
    // c64_core's look (T12 §11 Q2) or a REAL bus cycle with the select forced,
    // cart_fill's DDR2 read; and the app's bus DMA (one cycle per byte while
    // paused, ultimax as C64_MODE) to compare with
    input  wire        halt,
    output wire        halted,
    input  wire        lk_go,
    input  wire [18:0] lk_req,
    output wire [31:0] lk_res,
    input  wire        dma_req,
    input  wire [15:0] dma_addr,
    input  wire        ultimax,
    output wire        dma_done,
    output wire  [7:0] dma_rdata
);
    wire gate_ce, gclk;
    wire [15:0] pk_addr;
    wire  [1:0] pk_cls;
    wire        pk_hit, pk_mem, pk_ext;
    wire [23:0] pk_maddr;
    wire  [7:0] pk_data;
    wire        pkd_tgl, pkd_ack_tgl;
    wire [23:0] pkd_addr;
    wire  [7:0] pkd_data;
    wire        rsp_cyc_req, rsp_cyc_we, core_ultimax;
    wire [15:0] rsp_cyc_addr;
    wire  [7:0] rsp_cyc_wdata;
    wire  [1:0] rsp_force;
    wire        lens_use = lk_res[31];
    reg  gate_l = 1'b1;
    always @(negedge clk) gate_l <= gate_ce;
    assign gclk = clk & gate_l;
    wire         req_tgl, req_we, req_cached, ack_tgl;
    wire [23:0]  req_addr;
    wire [7:0]   req_data, fill_be;
    wire [5:0]   req_slot;
    wire [8:0]   fill_addr;
    wire [63:0]  fill_data;

    c64_core c64 (
        .dbg_req(1'b1), .clk2x(clk2x),            // T29: the outside port is wanted in every clock here
        .vmode(2'd0),                   // T40: PAL
        .clk(clk), .gclk(gclk), .gate_ce(gate_ce), .rst(rst), .halt_req(halt), .halted(halted),
        .snd_clk(clk), .snd_rst(rst),
        .audio_sample_en(1'b0), .audio_l(), .audio_r(),
        .pix_color(pix_color), .pix_x(pix_x), .pix_y(pix_y), .leds(),
        .keys(keys), .joy1(5'd0), .joy2(5'd0), .restore(1'b0),
        .dbg_addr(dbg_addr), .dbg_we(1'b0), .dbg_wdata(8'd0), .dbg_rdata(dbg_rdata),
        .reu_cmd_addr(), .reu_cmd_we(), .reu_cmd_valid(), .reu_cmd_ready(1'b1), .reu_en(1'b1), .reu_size(3'd5),
        .reu_wdata(), .reu_wdata_we(), .reu_rdata(16'd0), .reu_rdata_valid(1'b0),
        .cpu_reset_req(cpu_reset_req), .cpu_pause_req(1'b0),
        .cart_type(cart_type), .cart_kill_tgl(cart_kill_tgl), .cart_force_tgl(cart_force_tgl),
        .cart_active(cart_active), .cart_wclk(ui_clk),
        .cart_req_tgl(req_tgl), .cart_req_we(req_we), .cart_req_addr(req_addr),
        .cart_req_data(req_data), .cart_req_slot(req_slot), .cart_req_cached(req_cached),
        .cart_ack_tgl(ack_tgl),
        .cart_fill_be(fill_be), .cart_fill_addr(fill_addr), .cart_fill_data(fill_data),
        .cart_fl_enable(fl_enable), .cart_fl_magic1(fl_magic1), .cart_fl_magic2(fl_magic2),
        .cart_fl_mask(fl_mask), .cart_fl_id(fl_id), .cart_desc(cart_desc), .cart_evt_tgl(evt_tgl), .cart_evt_word(evt_word),
        .cart_fl_ack_tgl(fl_ack_tgl), .cart_inval_tgl(inval_tgl), .cart_ee_tgl(),
        .cart_ee_clk(clk), .cart_ee_addr(11'd0), .cart_ee_we(1'b0), .cart_ee_wdata(8'd0), .cart_ee_rdata(),
        .dma_req(dma_req || rsp_cyc_req), .dma_addr(lens_use ? rsp_cyc_addr : dma_addr),
        .dma_we(lens_use ? rsp_cyc_we : 1'b0), .dma_wdata(lens_use ? rsp_cyc_wdata : 8'd0),
        .dma_done(dma_done), .dma_rdata(dma_rdata), .ultimax(core_ultimax), .dma_force(rsp_force),
        .iec_atn_o(), .iec_clk_o(), .iec_data_o(),
        .iec_atn_i(1'b1), .iec_clk_i(1'b1), .iec_data_i(1'b1),
        .sb_addr(12'd0), .sb_rdata(), .sb_frozen(), .dbg_rom(1'b0),
        .cap_every(8'd0), .cap_gen(), .cap_tgl(), .cap_nosnap(),
        .cram_addr(10'd0), .cram_we(1'b0), .cram_wdata(4'd0), .cram_rdata(),
        .pk_addr(pk_addr), .pk_cls(pk_cls), .pk_hit(pk_hit), .pk_mem(pk_mem), .pk_maddr(pk_maddr),
        .pk_data(pk_data), .pk_ext(pk_ext)
    );
    // the responder's lens unit (no window, no session: sh_mode 0)
    // the DMA window's lens unit (no window traffic)
    dma_win win (
        .clk(clk),
        .dma_req_tgl(1'b0), .dma_addr(16'd0), .dma_we(1'b0), .dma_wdata(8'd0),
        .dma_bus(1'b0), .dma_rom(1'b0), .dma_done_tgl(), .dma_rdata(),
        .dbg_busy(1'b0), .port_use(), .port_addr(), .port_rom(), .port_we(), .port_wdata(),
        .port_rdata(8'd0),
        .cyc_req(rsp_cyc_req), .cyc_addr(rsp_cyc_addr), .cyc_we(rsp_cyc_we), .cyc_wdata(rsp_cyc_wdata),
        .cyc_done(dma_done), .cyc_rdata(dma_rdata), .cyc_force(rsp_force),
        .cram_addr(), .cram_we(), .cram_wdata(), .cram_rdata(4'd0), .cram_use(),
        .halted(halted), .app_ultimax(ultimax), .core_ultimax(core_ultimax),
        .pk_addr(pk_addr), .pk_cls(pk_cls), .pk_hit(pk_hit), .pk_mem(pk_mem), .pk_maddr(pk_maddr),
        .pk_data(pk_data), .cart_ext(pk_ext),
        .lk_go(lk_go), .lk_req(lk_req), .lk_res(lk_res),
        .pkd_tgl(pkd_tgl), .pkd_addr(pkd_addr), .pkd_ack_tgl(pkd_ack_tgl), .pkd_data(pkd_data)
    );
    assign wait_cpu = c64.xc_wait;
    assign fill_req  = req_tgl;          // a request (fetch or write) begins
    assign fill_done = ack_tgl;

    wire [26:0]  c_addr;
    wire [2:0]   c_cmd;
    wire         c_en, c_rdy, c_wdf_wren, c_wdf_rdy, c_rd_valid;
    wire [127:0] c_wdf_data, c_rd_data;
    wire [15:0]  c_wdf_mask;
    wire [63:0]  arb_rdata;
    wire [2:0]   arb_done;

    mig_arb arb (
        .ui_clk(ui_clk), .ui_rst(ui_rst), .calib_done(1'b1),
        .req_tgl({2'b00, a0_tgl}), .addr({52'd0, a0_addr}), .we({2'b00, a0_we}),
        .wdata({64'd0, a0_wdata}), .be({8'd0, 4'hF}),
        .done_tgl(arb_done), .rdata(arb_rdata), .rdata_line(),
        .c_addr(c_addr), .c_cmd(c_cmd), .c_en(c_en), .c_rdy(c_rdy), .c_wdf_data(c_wdf_data),
        .c_wdf_mask(c_wdf_mask), .c_wdf_wren(c_wdf_wren), .c_wdf_rdy(c_wdf_rdy),
        .c_rd_data(c_rd_data), .c_rd_data_valid(c_rd_valid),
        .v_addr(27'd0), .v_cmd(3'b001), .v_en(1'b0), .v_rdy(), .v_wdf_data(128'd0), .v_wdf_mask(16'd0),
        .v_wdf_wren(1'b0), .v_wdf_rdy(), .v_rd_data_valid(), .v_urgent(1'b0),
        .e_addr(27'd0), .e_cmd(3'd0), .e_en(1'b0), .e_rdy(), .e_wdf_data(128'd0), .e_wdf_mask(16'd0),
        .e_wdf_wren(1'b0), .e_rd_data_valid(),
        .app_addr(app_addr), .app_cmd(app_cmd), .app_en(app_en), .app_rdy(app_rdy),
        .app_wdf_data(app_wdf_data), .app_wdf_mask(app_wdf_mask), .app_wdf_wren(app_wdf_wren),
        .app_wdf_rdy(app_wdf_rdy), .app_rd_data(app_rd_data), .app_rd_data_valid(app_rd_data_valid)
    );
    assign a0_done_tgl = arb_done[0];
    assign a0_rdata    = arb_rdata[31:0];

    cart_fill cfill (
        .ui_clk(ui_clk), .ui_rst(ui_rst), .rom_sel(rom_sel),
        .req_tgl(req_tgl), .req_we(req_we), .req_addr(req_addr),
        .req_data(req_data), .req_slot(req_slot), .req_cached(req_cached), .ack_tgl(ack_tgl),
        .c_be(fill_be), .c_addr(fill_addr), .c_data(fill_data),
        .app_addr(c_addr), .app_cmd(c_cmd), .app_en(c_en), .app_rdy(c_rdy),
        .app_wdf_data(c_wdf_data), .app_wdf_mask(c_wdf_mask), .app_wdf_wren(c_wdf_wren),
        .app_wdf_rdy(c_wdf_rdy), .app_rd_data(c_rd_data), .app_rd_data_valid(c_rd_valid),
        .rbw(1'b0), .ob_v(), .ob_addr(), .ob_old(),
        .pk_tgl(pkd_tgl), .pk_addr(pkd_addr), .pk_ack_tgl(pkd_ack_tgl), .pk_data(pkd_data)
    );
endmodule
