// SPDX-License-Identifier: GPL-3.0-or-later
// Top of the debugger-stop bench (tb_dbgsess.cpp, T12 §14.8): c64_core with its
// gate (a BUFGCE model as in sim/statebus), dma_win.v (the app's window and
// the lens unit) and trx_sys.v (page 3 debug registers, page 5, state bus) wired
// as top.v wires them. There is no freeze session (SLIM): a stop is the gate
// shut, the app's window a bus cycle. The
// cartridge side as sim/cart has it: cart_fill and mig_arb on a DDR2 port that
// C++ models (the image, the MIG's stalls). No text layer, no drive.
module tb_dbgs_top (
    input  wire        clk,
    input  wire        clk2x,               // 2 x clk, edge-aligned (T29: CPU port, c64_mem's second port)
    input  wire        ui_clk,
    input  wire        rst,
    input  wire        ui_rst,
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
    input  wire        dma_rom,
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
    input  wire [63:0] keys,
    input  wire  [4:0] joy1,
    input  wire  [4:0] joy2,
    // to look at
    output wire  [9:0] pix_x,
    output wire  [8:0] pix_y,
    output wire  [3:0] pix_color,
    output wire        dbg_hold_o,
    output wire        dbg_stop_o,
    output wire        dbg_rel_o,
    output wire        dbg_own_o,
    output wire        d7ff_ev,             // T62 G17: a CPU write to $D7FF (c64_core), and its byte
    output wire  [7:0] d7ff_data,
    // the cartridge (tb_cart_top's ports)
    input  wire  [7:0] cart_type,
    input  wire [343:0] cart_desc,
    input  wire        cart_kill_tgl,
    output wire        cart_active,
    output wire        wait_cpu,
    output wire [26:0] app_addr,
    output wire  [2:0] app_cmd,
    output wire        app_en,
    input  wire        app_rdy,
    output wire [127:0] app_wdf_data,
    output wire [15:0] app_wdf_mask,
    output wire        app_wdf_wren,
    input  wire        app_wdf_rdy,
    input  wire [127:0] app_rd_data,
    input  wire        app_rd_data_valid
);
    wire gate_ce, gclk;
    reg  gate_l = 1'b1;
    always @(negedge clk) gate_l <= gate_ce;
    assign gclk = clk & gate_l;
    always @(posedge gclk) gclk_edges <= gclk_edges + 1;

    wire        dbg_own, dbg_ev, dbg_hold, dbg_stop, dbg_rel;
    wire        gate_frz;
    wire        app_pause = gate_frz && !jtag_cpu_only;
    wire        app_stop_m = app_stop & !dbg_own;       // top.v: the debugger owns run control
    wire        halt_req = jtag_halt | (app_stop_m & app_pause);
    wire        core_halt = halt_req;
    assign dbg_hold_o = dbg_hold;
    assign dbg_stop_o = dbg_stop;
    assign dbg_rel_o = dbg_rel;
    assign dbg_own_o = dbg_own;

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
    wire        pk_hit, pk_mem, pk_ext;
    wire [23:0] pk_maddr;
    wire  [7:0] pk_data;
    wire        pkd_tgl, pkd_ack_tgl;
    wire [23:0] pkd_addr;
    wire  [7:0] pkd_data;
    wire [15:0] tx_view;
    wire  [7:0] cap_every;
    wire [15:0] cap_gen, cap_nosnap;
    wire        dr_we, dr_re;
    wire [10:0] dr_addr;
    wire [31:0] dr_wdata, dr_rdata;

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
        .pk_data(pk_data), .cart_ext(pk_ext),
        .lk_go(lk_go), .lk_req(lk_req), .lk_res(lk_res),
        .pkd_tgl(pkd_tgl), .pkd_addr(pkd_addr), .pkd_ack_tgl(pkd_ack_tgl), .pkd_data(pkd_data)
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
        .dr_we(dr_we), .dr_re(dr_re), .dr_addr(dr_addr), .dr_wdata(dr_wdata), .dr_rdata(dr_rdata),
        .sb_addr(sb_addr), .sb_rdata(sb_rdata),
        .sb_frozen(sb_frozen), .halted(halted), .cap_gen(cap_gen), .cap_nosnap(cap_nosnap),
        .tx_view(tx_view)
    );

    // as top.v: the window, then JTAG, on the RAM's second port; the colour RAM's address
    wire  [15:0] p_addr  = port_use ? port_addr : dbg_addr;
    wire         p_we    = port_use ? port_we : dbg_we;
    wire   [7:0] p_wdata = port_use ? port_wdata : dbg_wdata;
    wire         p_rom   = port_use ? port_rom : 1'b0;
    assign cram_addr = win_cram_addr;

    wire [4:0] leds;
    wire [23:0] reu_cmd_addr; wire reu_cmd_we, reu_cmd_valid; wire [15:0] reu_wdata; wire [1:0] reu_wdata_we;
    wire signed [23:0] audio_l, audio_r;
    wire         req_tgl, req_we, req_cached, ack_tgl;
    wire [23:0]  req_addr;
    wire [7:0]   req_data, fill_be;
    wire [5:0]   req_slot;
    wire [8:0]   fill_addr;
    wire [63:0]  fill_data;
    c64_core c64 (
        .vmode(2'd0),                   // T40: PAL
        .d7ff_ev(d7ff_ev), .d7ff_data(d7ff_data),
        .clk(clk), .clk2x(clk2x), .dbg_req(1'b1), .gclk(gclk), .gate_ce(gate_ce), .rst(rst), .halt_req(core_halt), .halted(halted),
        .snd_clk(clk), .snd_rst(rst), .audio_sample_en(1'b0),
        .audio_l(audio_l), .audio_r(audio_r),
        .pix_color(pix_color), .pix_x(pix_x), .pix_y(pix_y), .leds(leds),
        .keys(keys), .joy1(joy1), .joy2(joy2), .restore(1'b0),
        .dbg_addr(p_addr), .dbg_rom(p_rom), .dbg_we(p_we), .dbg_wdata(p_wdata),
        .dbg_rdata(ram_rdata),
        .reu_cmd_addr(reu_cmd_addr), .reu_cmd_we(reu_cmd_we), .reu_cmd_valid(reu_cmd_valid),
        .reu_cmd_ready(1'b1), .reu_en(1'b1), .reu_size(3'd5), .reu_wdata(reu_wdata), .reu_wdata_we(reu_wdata_we),
        .reu_rdata(16'd0), .reu_rdata_valid(1'b0),
        .cpu_reset_req(core_reset_req), .cpu_pause_req(app_stop_m && !app_pause),
        .cart_type(cart_type), .cart_desc(cart_desc), .cart_kill_tgl(cart_kill_tgl), .cart_force_tgl(1'b0),
        .cart_active(cart_active), .cart_wclk(ui_clk),
        .cart_req_tgl(req_tgl), .cart_req_we(req_we), .cart_req_addr(req_addr),
        .cart_req_data(req_data), .cart_req_slot(req_slot), .cart_req_cached(req_cached),
        .cart_ack_tgl(ack_tgl),
        .cart_fill_be(fill_be), .cart_fill_addr(fill_addr), .cart_fill_data(fill_data),
        .cart_fl_enable(1'b0), .cart_fl_magic1(16'd0), .cart_fl_magic2(16'd0), .cart_fl_mask(16'd0),
        .cart_fl_id(24'd0), .cart_evt_tgl(), .cart_evt_word(), .cart_fl_ack_tgl(1'b0), .cart_inval_tgl(1'b0),
        .cart_ee_tgl(),
        .cart_ee_clk(clk), .cart_ee_addr(11'd0), .cart_ee_we(1'b0), .cart_ee_wdata(8'd0), .cart_ee_rdata(),
        .dma_req(cyc_req), .dma_addr(cyc_addr), .dma_we(cyc_we), .dma_wdata(cyc_wdata),
        .dma_done(cyc_done), .dma_rdata(cyc_rdata), .ultimax(core_ultimax),
        .dma_force(cyc_force),
        .iec_atn_o(), .iec_clk_o(), .iec_data_o(),
        .iec_atn_i(1'b1), .iec_clk_i(1'b1), .iec_data_i(1'b1),
        .sb_addr(sb_addr), .sb_rdata(sb_rdata), .sb_frozen(sb_frozen),
        .cap_every(cap_every), .cap_gen(cap_gen), .cap_tgl(), .cap_nosnap(cap_nosnap),
        .cram_addr(cram_addr), .cram_we(cram_we), .cram_wdata(cram_wdata), .cram_rdata(cram_rdata),
        .pk_addr(pk_addr), .pk_cls(pk_cls), .pk_hit(pk_hit), .pk_mem(pk_mem), .pk_maddr(pk_maddr),
        .pk_data(pk_data), .pk_ext(pk_ext),
        .dr_we(dr_we), .dr_re(dr_re), .dr_addr(dr_addr), .dr_wdata(dr_wdata), .dr_rdata(dr_rdata),
        .dbg_own(dbg_own), .dbg_ev(dbg_ev), .dbg_hold(dbg_hold), .dbg_stop(dbg_stop), .dbg_rel(dbg_rel),
        .fw_stop(app_stop), .drv_stop(1'b0), .drv_done(1'b0),
        .dbg_log_put(), .dbg_log_rec()
    );
    assign wait_cpu = c64.xc_wait;

    // the cartridge's DDR2 side as tb_cart_top has it
    wire [26:0]  c_addr;
    wire [2:0]   c_cmd;
    wire         c_en, c_rdy, c_wdf_wren, c_wdf_rdy, c_rd_valid;
    wire [127:0] c_wdf_data, c_rd_data;
    wire [15:0]  c_wdf_mask;
    mig_arb arb (
        .ui_clk(ui_clk), .ui_rst(ui_rst), .calib_done(1'b1),
        .req_tgl(3'b000), .addr(78'd0), .we(3'b000),
        .wdata(96'd0), .be(12'd0),
        .done_tgl(), .rdata(), .rdata_line(),
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
    cart_fill cfill (
        .ui_clk(ui_clk), .ui_rst(ui_rst), .rom_sel(2'd0),   // Gideon's base: the bench holds the image at 0x3C00000
        .req_tgl(req_tgl), .req_we(req_we), .req_addr(req_addr),
        .req_data(req_data), .req_slot(req_slot), .req_cached(req_cached), .ack_tgl(ack_tgl),
        .c_be(fill_be), .c_addr(fill_addr), .c_data(fill_data),
        .app_addr(c_addr), .app_cmd(c_cmd), .app_en(c_en), .app_rdy(c_rdy),
        .app_wdf_data(c_wdf_data), .app_wdf_mask(c_wdf_mask), .app_wdf_wren(c_wdf_wren),
        .app_wdf_rdy(c_wdf_rdy), .app_rd_data(c_rd_data), .app_rd_data_valid(c_rd_valid),
        .rbw(1'b0), .ob_v(), .ob_addr(), .ob_old(),
        .pk_tgl(pkd_tgl), .pk_addr(pkd_addr), .pk_ack_tgl(pkd_ack_tgl), .pk_data(pkd_data)
    );
    assign dbg_rdata = ram_rdata;
endmodule
