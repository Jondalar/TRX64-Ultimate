// SPDX-License-Identifier: GPL-3.0-or-later
// The debugger-stop bench through ultimate_sys (tb_ultsys.cpp): tb_dbgs_top (c64_core, trx_dbg,
// dma_win, trx_sys; sim/dbgsess) with the app's window NOT driven from C++ but by the real
// ultimate_sys (ghdl synth of designs/c64_basic/ultimate_sys.vhd, build.sh) wired as top.v wires it:
// dma_* to dma_win, C64_STOP/C64_MODE out as app_stop/app_ultimax/app_reset, the debugger's
// stop back in. The RISC-V is replaced by rvlite_bfm.v: C++ makes the I/O accesses the app makes.
module tb_us_top (
    input  wire        clk,                 // C64 clock (31.5 MHz)
    input  wire        clk2x,               // 2 x clk (T29)
    input  wire        ui_clk,
    input  wire        cpu_clk,             // the RISC-V's clock (100 MHz), ultimate_sys
    input  wire        rst,
    input  wire        ui_rst,
    input  wire        cpu_rst,
    input  wire        ign_dbg,             // control run: ultimate_sys does not see the debugger's stop
    input  wire [63:0] ult_keys,            // the physical keyboard matrix and joysticks ultimate_sys's KEYB_ROW / KEYB_JOY read
    input  wire        ult_restore,         // T62 G04: RESTORE pressed, in the cpu clock
    output wire        ult_scan_o,          // T62 G09: scanlines out of ultimate_sys
    input  wire  [9:0] ult_joys,            // port 2 in 9:5, port 1 in 4:0, 1 = pressed
    input  wire        dbg_re,
    input  wire [15:0] dbg_addr,
    input  wire        dbg_we,
    input  wire  [7:0] dbg_wdata,
    output wire  [7:0] dbg_rdata,
    input  wire        jtag_halt,
    input  wire        jtag_cpu_only,
    output wire        halted,
    output wire [31:0] gclk_edges,
    input  wire        j_re,
    input  wire        j_we,
    input  wire [11:0] j_addr,
    input  wire [31:0] j_wdata,
    output wire [31:0] j_rdata,
    output wire        j_rvalid,
    output wire        dbg_hold_o,
    output wire        dbg_stop_o,
    output wire        dbg_rel_o,
    output wire        dbg_own_o,
    output wire        app_stop_o,          // C64_STOP as ultimate_sys gives it
    output wire        app_ultimax_o,
    output wire [31:0] dma_count,           // window bytes ultimate_sys has handed over
    output wire        c64_d7ff_seen        // T62 G17: the C64 wrote $D7FF at least once
);
    wire        d7ff_ev;
    wire  [7:0] d7ff_data;
    reg         ult_dbg_tgl = 1'b0;         // as top.v: toggle and held byte in the C64 clock
    reg   [7:0] ult_dbg_data = 8'd0;
    reg         d7ff_any = 1'b0;
    always @(posedge clk) if (d7ff_ev) begin ult_dbg_tgl <= ~ult_dbg_tgl; ult_dbg_data <= d7ff_data; d7ff_any <= 1'b1; end
    assign c64_d7ff_seen = d7ff_any;
    wire        dma_req_tgl, dma_we, dma_bus, dma_rom, dma_done_tgl;
    wire [15:0] dma_addr;
    wire  [7:0] dma_wdata, dma_rdata;
    wire        app_stop, app_ultimax, app_reset;
    reg  [31:0] dma_n = 32'd0;
    reg         dma_t = 1'b0;
    always @(posedge cpu_clk) begin dma_t <= dma_req_tgl; if (dma_t != dma_req_tgl) dma_n <= dma_n + 1; end
    assign dma_count = dma_n;
    assign app_stop_o = app_stop;
    assign app_ultimax_o = app_ultimax;

    tb_dbgs_top dut (
        .clk(clk), .clk2x(clk2x), .ui_clk(ui_clk), .rst(rst), .ui_rst(ui_rst),
        .dbg_re(dbg_re), .dbg_addr(dbg_addr), .dbg_we(dbg_we), .dbg_wdata(dbg_wdata), .dbg_rdata(dbg_rdata),
        .jtag_halt(jtag_halt), .jtag_cpu_only(jtag_cpu_only),
        .app_stop(app_stop), .app_ultimax(app_ultimax), .app_reset(app_reset),
        .halted(halted), .gclk_edges(gclk_edges),
        .dma_req_tgl(dma_req_tgl), .dma_addr(dma_addr), .dma_we(dma_we), .dma_wdata(dma_wdata),
        .dma_bus(dma_bus), .dma_rom(dma_rom), .dma_done_tgl(dma_done_tgl), .dma_rdata(dma_rdata),
        .j_re(j_re), .j_we(j_we), .j_addr(j_addr), .j_wdata(j_wdata), .j_rdata(j_rdata), .j_rvalid(j_rvalid),
        .rq_tgl(1'b0), .rq_addr(16'd0), .rq_we(1'b0), .rq_wdata(8'd0), .rs_tgl(), .rs_rdata(),
        .keys(64'd0), .joy1(5'd0), .joy2(5'd0),
        .pix_x(), .pix_y(), .pix_color(),
        .dbg_hold_o(dbg_hold_o), .dbg_stop_o(dbg_stop_o), .dbg_rel_o(dbg_rel_o),
        .dbg_own_o(dbg_own_o), .d7ff_ev(d7ff_ev), .d7ff_data(d7ff_data),
        .cart_type(8'd0), .cart_desc(344'd0), .cart_kill_tgl(1'b0), .cart_active(),
        .wait_cpu(), .app_addr(), .app_cmd(), .app_en(),
        .app_rdy(1'b1), .app_wdf_data(), .app_wdf_mask(), .app_wdf_wren(),
        .app_wdf_rdy(1'b1), .app_rd_data(128'd0), .app_rd_data_valid(1'b0)
    );

    /*ULT_INST*/
endmodule
