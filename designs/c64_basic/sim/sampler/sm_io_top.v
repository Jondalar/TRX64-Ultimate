// SPDX-License-Identifier: GPL-3.0-or-later
// T25.6 sim wrapper: trx_io (the app's side, cpu_clk) next to trx_sampler (clk, clk2x), joined as top.v / c64_core.v do.
module sm_io_top (
    input  wire        cpu_clk, clk, clk2x, rst,
    // the app's bus (trx_io: addr b16 = EEPROM window, else 0x101A0000 + addr; the sampler is page 8)
    input  wire        we, re,
    input  wire [16:0] addr,
    input  wire  [7:0] wdata,
    output wire  [7:0] rdata,
    output wire        ack,
    // C64 slot side
    input  wire        c64_en,
    input  wire [15:0] bus_addr,
    input  wire        io_ev,
    input  wire [15:0] io_ev_addr,
    input  wire  [7:0] io_ev_data,
    input  wire        io_ev_we, io_ev_io,
    output wire        oe,
    output wire  [7:0] dout,
    output wire        irq,
    // DDR2
    output wire        f_req_tgl,
    output wire [26:0] f_addr,
    input  wire        f_done_tgl,
    input  wire [31:0] f_data,
    output wire signed [22:0] smp_l, smp_r
);
    wire        sm_tgl, sm_we, sm_rs_tgl;
    wire  [7:0] sm_addr, sm_wdata, sm_rdata;
    trx_io tio (
        .clk(cpu_clk), .we(we), .re(re), .addr(addr), .wdata(wdata), .rdata(rdata), .ack(ack), .irq(),
        .rq_tgl(), .rq_addr(), .rq_we(), .rq_wdata(), .rs_tgl(1'b0), .rs_rdata(8'd0),
        .stop_tgl(1'b0), .cap_tgl(1'b0), .eng_tgl(1'b0), .gap_tgl(1'b0),
        .sm_tgl(sm_tgl), .sm_addr(sm_addr), .sm_we(sm_we), .sm_wdata(sm_wdata), .sm_rs_tgl(sm_rs_tgl), .sm_rdata(sm_rdata),
        .p6_we(), .p6_re(), .p6_rdata(8'd0), .ev_vsync(1'b0), .ev_flip(1'b0),
        .fl_enable(), .fl_magic1(), .fl_magic2(), .fl_mask(), .fl_id(), .cart_desc(),
        .evt_tgl(1'b0), .evt_word(32'd0), .fl_ack_tgl(), .inval_tgl(), .rom_sel(), .ee_tgl(1'b0),
        .ee_addr(), .ee_we(), .ee_wdata(), .ee_rdata(8'd0), .build(32'd0), .ddr_cal(64'd0));
    trx_sampler smp (
        .clk(clk), .clk2x(clk2x), .rst(rst), .vmode(2'd0), .c64_en(c64_en),
        .bus_addr(bus_addr), .io_ev(io_ev), .io_ev_addr(io_ev_addr), .io_ev_data(io_ev_data), .io_ev_we(io_ev_we),
        .io_ev_io(io_ev_io), .oe(oe), .dout(dout), .irq(irq),
        .a_tgl(sm_tgl), .a_we(sm_we), .a_addr(sm_addr), .a_wdata(sm_wdata), .a_rs_tgl(sm_rs_tgl), .a_rdata(sm_rdata),
        .f_req_tgl(f_req_tgl), .f_addr(f_addr), .f_done_tgl(f_done_tgl), .f_data(f_data),
        .smp_l(smp_l), .smp_r(smp_r));
endmodule
