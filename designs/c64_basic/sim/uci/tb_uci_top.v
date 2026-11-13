// SPDX-License-Identifier: GPL-3.0-or-later
// T28 §7.1 on the real machine: c64_core (6510, PLA/arbiter, REU on, no
// cartridge) with uci_unit (through uci_flat.vhd, ghdl synth) as top.v and
// ultimate_sys put them together, for tb_uci_core.cpp. clk = C64 (31.5 MHz),
// sys_clk = the app's 100 MHz; the app's IO bus to the UCI's 8 KB range.
module tb_uci_top (
    input  wire        clk,
    input  wire        clk2x,               // 2 x clk, edge-aligned: the SID engine (T25.1)
    input  wire        rst,
    input  wire        sys_clk,
    input  wire        sys_rst,
    input  wire [15:0] dbg_addr,            // C64 RAM debug port (clk)
    input  wire        dbg_we,
    input  wire [7:0]  dbg_wdata,
    output wire [7:0]  dbg_rdata,
    input  wire        io_read, io_write,   // sys_clk
    input  wire [12:0] io_addr,
    input  wire [7:0]  io_wdata,
    output wire [7:0]  io_rdata,
    output wire        io_ack,
    output wire        io_irq,
    output wire        freeze,
    output wire [9:0]  pix_x,
    output wire [8:0]  pix_y,
    output reg  [31:0] ev_reads = 0,        // UCI register reads/writes the C64 made
    output reg  [31:0] ev_writes = 0,
    // U13: the machine's stop and the lens unit's look (dma_win's side, driven by the bench)
    input  wire        halt_req,
    output wire        halted,
    input  wire [15:0] pk_addr,
    input  wire  [1:0] pk_cls,
    input  wire        pk_on,
    output wire  [7:0] pk_data,
    output wire        pk_ext,
    output wire        pk_wait
);
    wire [3:0] pix_color; wire [4:0] leds;
    wire [23:0] reu_cmd_addr; wire reu_cmd_we, reu_cmd_valid; wire [15:0] reu_wdata; wire [1:0] reu_wdata_we;
    wire signed [23:0] audio_l, audio_r;
    wire gate_ce, gclk;
    reg  gate_l = 1'b1;
    always @(negedge clk) gate_l <= gate_ce;
    assign gclk = clk & gate_l;

    wire [15:0] bus_addr, ev_addr;
    wire  [7:0] ev_data, uci_dout;
    wire        ev, ev_we, ev_io, uci_oe, uci_irq;

    c64_core c64 (
        .dbg_req(1'b1),            // T29: the outside port is wanted in every clock here
        .vmode(2'd0),                   // T40: PAL
        .clk(clk), .gclk(gclk), .gate_ce(gate_ce), .rst(rst), .halt_req(halt_req), .halted(halted),
        .snd_clk(clk), .snd_rst(rst), .audio_sample_en(1'b0),
        .clk2x(clk2x),
        .audio_l(audio_l), .audio_r(audio_r),
        .pix_color(pix_color), .pix_x(pix_x), .pix_y(pix_y), .leds(leds),
        .keys(64'd0), .joy1(5'd0), .joy2(5'd0), .restore(1'b0),
        .dbg_addr(dbg_addr), .dbg_we(dbg_we), .dbg_wdata(dbg_wdata), .dbg_rdata(dbg_rdata),
        .reu_cmd_addr(reu_cmd_addr), .reu_cmd_we(reu_cmd_we), .reu_cmd_valid(reu_cmd_valid),
        .reu_cmd_ready(1'b1), .reu_en(1'b1), .reu_size(3'd5), .reu_wdata(reu_wdata), .reu_wdata_we(reu_wdata_we),
        .reu_rdata(16'd0), .reu_rdata_valid(1'b0),
        .cpu_reset_req(1'b0), .cpu_pause_req(freeze),
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
        .bus_addr(bus_addr), .io_ev(ev), .io_ev_addr(ev_addr), .io_ev_data(ev_data),
        .io_ev_we(ev_we), .io_ev_io(ev_io), .io_oe(uci_oe), .io_dout(uci_dout), .io_irq(uci_irq),
        .pk_addr(pk_addr), .pk_cls(pk_cls), .pk_on(pk_on), .pk_data(pk_data), .pk_ext(pk_ext), .pk_wait(pk_wait)
    );

    uci_flat uci (
        .io_clock(sys_clk), .io_reset(sys_rst), .io_read(io_read), .io_write(io_write),
        .io_addr(io_addr), .io_wdata(io_wdata), .io_rdata(io_rdata), .io_ack(io_ack), .io_irq(io_irq),
        .c64_clock(clk), .bus_addr(bus_addr), .ev(ev), .ev_addr(ev_addr), .ev_data(ev_data),
        .ev_we(ev_we), .ev_io(ev_io), .dout(uci_dout), .oe(uci_oe), .c64_irq(uci_irq), .freeze(freeze)
    );

    // the C64's accesses to $DF1C-$DF1F as the UCI sees them
    always @(posedge clk)
        if (ev && ev_io && ev_addr[15:2] == 14'h37C7) begin
            if (ev_we) ev_writes <= ev_writes + 1;
            else       ev_reads  <= ev_reads + 1;
        end
endmodule
