// SPDX-License-Identifier: GPL-3.0-or-later
// The C64 core and the app's drive A on one serial bus, as top.v puts them
// together (iec_bus.v), for tb_drive.cpp. clk = C64 (31.5 MHz), sys_clk =
// the RISC-V's 100 MHz, ui_clk = MIG's 75 MHz. The board's IEC port lines come
// from the testbench. The memory is as in top.v: the system bus through
// cpu_mem_bridge (client 0), the drive's own port (client 2), mig_arb and a
// model of MIG (sim/mig_ui_model.v). With load set, client 1 reads back to
// back at 64 MB (as the REU would, and the CPU on top), to load the DDR2.
// dp_*: the drive port's accesses (sys_clk cycles from request to done).
// The machine pause as in top.v: c64_core's gate (a BUFGCE model below), the
// drive halted through a synchronizer; the app's DMA port, the VIC's raster
// position and the state bus for tb_pause.cpp.
module tb_drive_top #(
    parameter [22:0] TURBO_HOLD = 23'd5000   // T29: PHI2 cycles at 1 MHz after a reset (the core's 2^22 is longer than a bench boot)
) (
    input  wire        clk,
    input  wire        clk2x,               // 2 x clk, edge-aligned: the SID engine (T25.1)
    input  wire        rst,
    input  wire        sys_clk,
    input  wire        sys_rst,
    input  wire        ui_clk,
    input  wire        load,
    // C64 RAM debug port (clk)
    input  wire [15:0] dbg_addr,
    input  wire        dbg_we,
    input  wire [7:0]  dbg_wdata,
    output wire [7:0]  dbg_rdata,
    input  wire        cpu_reset_req,
    input  wire        halt_req,
    output wire        halted,
    output reg  [31:0] gclk_edges = 0,  // C64 clock edges that ran
    input  wire        dma_req,
    input  wire [15:0] dma_addr,
    input  wire        dma_we,
    input  wire [7:0]  dma_wdata,
    output wire        dma_done,
    output wire [7:0]  dma_rdata,
    output wire [9:0]  pix_x,
    output wire [8:0]  pix_y,
    input  wire [11:0] sb_addr,             // state bus (c64_core.v), data 2 clocks later
    output wire [31:0] sb_rdata,
    // drive (sys_clk)
    input  wire        io_read, io_write,
    input  wire [15:0] io_addr,
    input  wire [7:0]  io_wdata,
    output wire [7:0]  io_rdata,
    output wire        io_ack,
    input  wire        host_request, host_rwn,
    input  wire [25:0] host_address,
    input  wire [3:0]  host_byte_en,
    input  wire [31:0] host_wdata,
    output wire        mem_rack,
    output wire [7:0]  mem_rack_tag,
    output reg  [31:0] dp_reads = 0,
    output reg  [31:0] dp_writes = 0,
    output reg  [31:0] dp_cycles = 0,
    output reg  [15:0] dp_max = 0,
    output reg  [31:0] dp_rcycles = 0,      // reads only
    output reg  [15:0] dp_rmax = 0,
    output reg  [31:0] dp_over50 = 0,       // > 500 ns
    output reg  [31:0] load_reads = 0,
    input  wire        c64_reset_n,
    input  wire [7:0]  turbo_en, turbo_prefer,   // T29: the app's turbo setting
    input  wire        turbo_upd_tgl,
    output wire [31:0] turbo_stat,
    output wire [2:0]  leds_n,
    output wire [255:0] drv_dbg,         // drive_unit's dbg (sys_clk)
    // the bus as seen by each side, for the testbench to watch
    output wire        c64_atn, c64_clk, c64_data,
    output wire        drv_atn, drv_clk, drv_data,
    // the board's IEC port: our pulls out, the lines in (1 = low), for a line model
    output wire        port_atn_o, port_clk_o, port_data_o,
    input  wire        port_atn_i, port_clk_i, port_data_i
);
    wire c64_atn_o, c64_clk_o, c64_data_o;
    wire drv_clk_o, drv_data_o;
    wire [3:0] pix_color; wire [4:0] leds;
    wire [23:0] reu_cmd_addr; wire reu_cmd_we, reu_cmd_valid; wire [15:0] reu_wdata; wire [1:0] reu_wdata_we;
    wire signed [23:0] audio_l, audio_r;

    // top.v's BUFGCE: CE taken while clk is low, so a change made at a rising
    // edge acts from the next one
    wire gate_ce, gclk;
    reg  gate_l = 1'b1;
    always @(negedge clk) gate_l <= gate_ce;
    assign gclk = clk & gate_l;
    always @(posedge gclk) gclk_edges <= gclk_edges + 1;
    reg [1:0] drv_halt_s = 2'b00;
    always @(posedge sys_clk) drv_halt_s <= {drv_halt_s[0], halted};
    wire        mem_request, mem_rwn;
    wire [7:0]  mem_tag, mem_dack_tag;
    wire [25:0] mem_address;
    wire [3:0]  mem_byte_en;
    wire [31:0] mem_wdata, mem_rdata;
    wire        dp_tgl, dp_done_tgl, dp_we;
    wire [25:0] dp_addr;
    wire [31:0] dp_wdata;
    wire [3:0]  dp_be;
    wire [127:0] dp_rdata;

    c64_core #(.TURBO_HOLD(TURBO_HOLD)) c64 (
        .turbo_en(turbo_en), .turbo_prefer(turbo_prefer), .turbo_upd_tgl(turbo_upd_tgl),
        .turbo_stat(turbo_stat), .turbo_ran_clr(1'b0),
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
        .cpu_reset_req(cpu_reset_req), .cpu_pause_req(1'b0),
        .cart_type(8'd0), .cart_desc(344'd0), .cart_kill_tgl(1'b0), .cart_force_tgl(1'b0), .cart_active(),
        .cart_wclk(clk), .cart_req_tgl(), .cart_req_we(), .cart_req_addr(), .cart_req_data(),
        .cart_req_slot(), .cart_req_cached(), .cart_ack_tgl(1'b0),
        .cart_fl_enable(1'b0), .cart_fl_magic1(16'd0), .cart_fl_magic2(16'd0), .cart_fl_mask(16'd0),
        .cart_fl_id(24'd0), .cart_evt_tgl(), .cart_evt_word(), .cart_fl_ack_tgl(1'b0), .cart_inval_tgl(1'b0),
        .cart_ee_tgl(),
        .cart_ee_clk(clk), .cart_ee_addr(11'd0), .cart_ee_we(1'b0), .cart_ee_wdata(8'd0), .cart_ee_rdata(),
        .cart_fill_be(8'd0), .cart_fill_addr(9'd0), .cart_fill_data(64'd0),
        .dma_req(dma_req), .dma_addr(dma_addr), .dma_we(dma_we), .dma_wdata(dma_wdata),
        .dma_done(dma_done), .dma_rdata(dma_rdata), .ultimax(1'b0),
        .iec_atn_o(c64_atn_o), .iec_clk_o(c64_clk_o), .iec_data_o(c64_data_o),
        .iec_atn_i(c64_atn), .iec_clk_i(c64_clk), .iec_data_i(c64_data),
        .sb_addr(sb_addr), .sb_rdata(sb_rdata), .sb_frozen(), .dbg_rom(1'b0),
        .cap_every(8'd0), .cap_gen(), .cap_tgl(), .cap_nosnap(),
        .cram_addr(10'd0), .cram_we(1'b0), .cram_wdata(4'd0), .cram_rdata()
    );

    drive_flat drive (
        .clock(sys_clk), .reset(sys_rst),
        .io_read(io_read), .io_write(io_write), .io_addr(io_addr), .io_wdata(io_wdata),
        .io_rdata(io_rdata), .io_ack(io_ack),
        .host_request(host_request), .host_rwn(host_rwn), .host_address(host_address),
        .host_byte_en(host_byte_en), .host_wdata(host_wdata),
        .mem_request(mem_request), .mem_tag(mem_tag), .mem_rwn(mem_rwn), .mem_address(mem_address),
        .mem_byte_en(mem_byte_en), .mem_wdata(mem_wdata), .mem_rdata(mem_rdata),
        .mem_rack(mem_rack), .mem_rack_tag(mem_rack_tag), .mem_dack_tag(mem_dack_tag),
        .drive_stop(1'b0), .drive_halt(drv_halt_s[1]), .c64_reset_n(c64_reset_n),
        .iec_atn_i(drv_atn), .iec_clk_i(drv_clk), .iec_data_i(drv_data),
        .iec_clk_o(drv_clk_o), .iec_data_o(drv_data_o), .leds_n(leds_n), .drive_dbg(drv_dbg),
        .dp_req_tgl(dp_tgl), .dp_addr(dp_addr), .dp_we(dp_we), .dp_wdata(dp_wdata), .dp_be(dp_be),
        .dp_done_tgl(dp_done_tgl), .dp_rdata(dp_rdata)
    );

    wire        cpu_tgl, cpu_done_tgl, cpu_we;
    wire [25:0] cpu_addr;
    wire [31:0] cpu_wdata, cpu_done_rdata;
    wire [3:0]  cpu_be;
    cpu_mem_bridge bridge (
        .clk(sys_clk), .rst(sys_rst),
        .request(mem_request), .tag(mem_tag), .read_writen(mem_rwn), .address(mem_address),
        .byte_en(mem_byte_en), .wdata(mem_wdata), .rdata(mem_rdata), .rack(mem_rack),
        .rack_tag(mem_rack_tag), .dack_tag(mem_dack_tag),
        .req_tgl(cpu_tgl), .a_addr(cpu_addr), .a_we(cpu_we), .a_wdata(cpu_wdata), .a_be(cpu_be),
        .done_tgl(cpu_done_tgl), .done_rdata(cpu_done_rdata)
    );

    // client 1: reads back to back while load is set
    reg         ld_tgl = 1'b0;
    reg  [25:0] ld_addr = 26'h2000000;
    reg  [1:0]  ld_s = 2'b00;
    wire        ld_done;
    wire [31:0] ld_rdata;
    always @(posedge sys_clk) begin
        ld_s <= {ld_s[0], ld_done};
        if (ld_s[1] == ld_tgl && load) begin
            ld_tgl  <= ~ld_tgl;
            ld_addr <= 26'h2000000 | {ld_addr[19:0] * 20'd40503 + 20'd1, 1'b0};
            load_reads <= load_reads + 1;
        end
    end

    wire ui_rst, calib;
    wire [26:0] app_addr; wire [2:0] app_cmd; wire app_en, app_rdy, app_wdf_wren, app_wdf_rdy;
    wire [127:0] app_wdf_data, app_rd_data; wire [15:0] app_wdf_mask; wire app_rd_data_valid;
    mig_arb arb (
        .ui_clk(ui_clk), .ui_rst(ui_rst), .calib_done(calib),
        .req_tgl({dp_tgl, ld_tgl, cpu_tgl}), .addr({dp_addr, ld_addr, cpu_addr}),
        .we({dp_we, 1'b0, cpu_we}), .wdata({dp_wdata, 32'd0, cpu_wdata}), .be({dp_be, 4'd0, cpu_be}),
        .done_tgl({dp_done_tgl, ld_done, cpu_done_tgl}), .rdata({ld_rdata, cpu_done_rdata}),
        .rdata_line(dp_rdata),
        .c_addr(27'd0), .c_cmd(3'd0), .c_en(1'b0), .c_rdy(), .c_wdf_data(128'd0), .c_wdf_mask(16'd0),
        .c_wdf_wren(1'b0), .c_wdf_rdy(), .c_rd_data(), .c_rd_data_valid(),
        .v_addr(27'd0), .v_cmd(3'b001), .v_en(1'b0), .v_rdy(), .v_wdf_data(128'd0), .v_wdf_mask(16'd0),
        .v_wdf_wren(1'b0), .v_wdf_rdy(), .v_rd_data_valid(), .v_urgent(1'b0),
        .e_addr(27'd0), .e_cmd(3'd0), .e_en(1'b0), .e_rdy(), .e_wdf_data(128'd0), .e_wdf_mask(16'd0),
        .e_wdf_wren(1'b0), .e_rd_data_valid(),
        .app_addr(app_addr), .app_cmd(app_cmd), .app_en(app_en), .app_rdy(app_rdy),
        .app_wdf_data(app_wdf_data), .app_wdf_mask(app_wdf_mask), .app_wdf_wren(app_wdf_wren),
        .app_wdf_rdy(app_wdf_rdy), .app_rd_data(app_rd_data), .app_rd_data_valid(app_rd_data_valid)
    );
    mig_ui_model mig (
        .ui_clk(ui_clk), .ui_rst(ui_rst), .calib_done(calib),
        .app_addr(app_addr), .app_cmd(app_cmd), .app_en(app_en), .app_rdy(app_rdy),
        .app_wdf_data(app_wdf_data), .app_wdf_mask(app_wdf_mask), .app_wdf_wren(app_wdf_wren),
        .app_wdf_rdy(app_wdf_rdy), .app_rd_data(app_rd_data), .app_rd_data_valid(app_rd_data_valid)
    );

    // the drive port's accesses, seen as drive_mem sees them
    reg  [1:0]  dp_s = 2'b00;
    reg         dp_open = 1'b0, dp_last = 1'b0, dp_rd = 1'b0;
    reg  [15:0] dp_n;
    always @(posedge sys_clk) begin
        dp_s <= {dp_s[0], dp_done_tgl};
        if (dp_tgl != dp_last) begin
            dp_last <= dp_tgl; dp_open <= 1'b1; dp_n <= 16'd1; dp_rd <= !dp_we;
            if (dp_we) dp_writes <= dp_writes + 1; else dp_reads <= dp_reads + 1;
        end else if (dp_open) begin
            if (dp_s[1] == dp_tgl) begin
                dp_open <= 1'b0;
                dp_cycles <= dp_cycles + dp_n;
                if (dp_n > dp_max) dp_max <= dp_n;
                if (dp_rd) dp_rcycles <= dp_rcycles + dp_n;
                if (dp_rd && dp_n > dp_rmax) dp_rmax <= dp_n;
                if (dp_n > 50) dp_over50 <= dp_over50 + 1;
            end else dp_n <= dp_n + 1'b1;
        end
    end

    iec_bus iec (
        .clk(clk), .c64_atn_o(c64_atn_o), .c64_clk_o(c64_clk_o), .c64_data_o(c64_data_o),
        .c64_atn_i(c64_atn), .c64_clk_i(c64_clk), .c64_data_i(c64_data),
        .cpu_clk(sys_clk), .drv_clk_o(drv_clk_o), .drv_data_o(drv_data_o),
        .drv_atn_i(drv_atn), .drv_clk_i(drv_clk), .drv_data_i(drv_data),
        .port_off(1'b0), .port_atn_i(port_atn_i), .port_clk_i(port_clk_i), .port_data_i(port_data_i),
        .port_atn_o(port_atn_o), .port_clk_o(port_clk_o), .port_data_o(port_data_o)
    );
endmodule
